// SPDX-License-Identifier: MIT
//
// The synchronous encoder transaction (docs/architecture.md §4). The
// decoder keeps a slot pool so completions can be published zero-copy;
// encoding has nothing to publish, so one picture in means one access unit
// out and vaEndPicture can return the bytes.

#include "encode.h"

#include "alloc.h"

#include "../util/error.h"

#include <algorithm>
#include <cstring>
#include <string>

extern "C" {
#include <poll.h>
}

namespace iris {

namespace {

constexpr unsigned kOutputSlots = 4;
constexpr unsigned kCaptureSlots = 4;
constexpr int kPollMs = 20;
// How long an input may still be under a GPU write before the firmware would
// read it. The same bound the decode path gives a client buffer it is about
// to write into (src/iris/session.cc, kImportFenceMs).
constexpr int kInputFenceMs = 2000;
// Below this the wait is not worth a trace record: every picture has one.
constexpr int kFenceTraceMs = 20;
// A client's vaSyncBuffer timeout is a bound, not a schedule. Cap it so a
// millisecond value mistaken for a nanosecond one cannot hold a thread for
// years.
constexpr uint64_t kMaxTimeoutNs = 60ull * 1000 * 1000 * 1000;

int bitrate_mode(RateMode mode)
{
    return mode == RateMode::Cbr ? V4L2_MPEG_VIDEO_BITRATE_MODE_CBR : V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
}

// Iris counts the I frame inside GOP_SIZE: a value of 60 puts the next IDR
// at frame 61, while VA's intra_period is the distance between I frames.
// Program one less; 0 asks the firmware for intra-only.
int32_t gop_control(uint32_t intra_period)
{
    return static_cast<int32_t>(intra_period ? intra_period - 1 : 0);
}

const char* rate_mode_name(RateMode mode)
{
    switch (mode) {
    case RateMode::Cbr:
        return "cbr";
    case RateMode::Cqp:
        return "cqp";
    default:
        return "vbr";
    }
}

} // namespace

EncoderSession::EncoderSession(
    std::unique_ptr<Device> device, const EncodeConfig& config, const Options& options, const Trace& trace)
    : device_(std::move(device))
    , config_(config)
    , options_(options)
    , trace_(trace)
{
}

EncoderSession::~EncoderSession()
{
    try {
        finish();
    } catch (...) {
        // A destructor cannot report; finish() already traced what failed.
    }
}

int EncoderSession::sync_timeout_ms() const
{
    if (cold_ && !options_.sync_timeout_overridden)
        return options_.cold_start_timeout_ms;
    return options_.sync_timeout_ms;
}

EncoderSession::Clock::time_point EncoderSession::deadline() const
{
    return Clock::now() + std::chrono::milliseconds(sync_timeout_ms());
}

void EncoderSession::set_control(uint32_t id, int32_t value, const char* name)
{
    if (!device_->set_control(id, value)) {
        trace_("encode control missing name=%s id=%#x", name, id);
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED, std::string("encoder control not available: ") + name);
    }
}

void EncoderSession::configure(
    const EncodeParams& params, unsigned width, unsigned height, PrepareAccessUnit prepare)
{
    std::lock_guard<std::mutex> lock(mutex_);
    require(!configured_, "encoder configured twice", VA_STATUS_ERROR_INVALID_PARAMETER);

    const bool h264 = config_.codec == V4L2_PIX_FMT_H264;
    const bool hevc = config_.codec == V4L2_PIX_FMT_HEVC;
    require(h264 || hevc, "encoder codec", VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    require(width && height, "encoder geometry", VA_STATUS_ERROR_INVALID_PARAMETER);
    // The context is the client's upper bound. A larger surface would need a
    // reconfigure, and the encoder has no SOURCE_CHANGE.
    require(width <= config_.width && height <= config_.height, "encode surface exceeds the context",
        VA_STATUS_ERROR_INVALID_SURFACE);
    config_.width = width;
    config_.height = height;

    // CAPTURE first: this is what selects the codec. G_FMT is not a
    // formality — iris_venc silently keeps the previous fourcc when it does
    // not recognise the request.
    const FormatInfo capture = device_->set_format(
        Queue::Capture, FormatInfo { config_.codec, config_.width, config_.height, 1, 0, 0 });
    require(capture.pixelformat == config_.codec, "encoder did not accept the codec",
        VA_STATUS_ERROR_UNSUPPORTED_PROFILE);

    const FormatInfo output = device_->set_format(
        Queue::Output, FormatInfo { V4L2_PIX_FMT_NV12, config_.width, config_.height, 1, 0, 0 });
    require(output.pixelformat == V4L2_PIX_FMT_NV12, "encoder did not accept NV12",
        VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    const Layout input_layout = stable_layout(VA_FOURCC_NV12, config_.width, config_.height);
    require(input_layout.size >= output.sizeimage, "surface buffer is smaller than the encoder input",
        VA_STATUS_ERROR_INVALID_SURFACE);

    // The raw port is aligned to 128x32; the crop is what makes the coded
    // size the visible size instead of the aligned one.
    device_->set_crop(0, 0, config_.width, config_.height);

    if (h264) {
        set_control(V4L2_CID_MPEG_VIDEO_H264_PROFILE, params.profile, "h264 profile");
        if (params.level >= 0)
            set_control(V4L2_CID_MPEG_VIDEO_H264_LEVEL, params.level, "h264 level");
        set_control(V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
            params.cabac ? V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC
                         : V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC,
            "h264 entropy mode");
    } else {
        set_control(V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, params.profile, "hevc profile");
        if (params.level >= 0)
            set_control(V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, params.level, "hevc level");
    }

    const bool cqp = params.rate_mode == RateMode::Cqp;
    require(!cqp || params.qp >= 1, "CQP needs a picture QP", VA_STATUS_ERROR_INVALID_PARAMETER);
    set_control(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, cqp ? 0 : 1, "frame rc enable");
    if (cqp) {
        // Rate control off: the I/P QP controls decide every picture.
        set_frame_qp(params.qp);
    } else {
        set_control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, bitrate_mode(params.rate_mode), "bitrate mode");
        if (params.bitrate)
            set_control(V4L2_CID_MPEG_VIDEO_BITRATE, static_cast<int32_t>(params.bitrate), "bitrate");
        if (params.rate_mode == RateMode::Cbr) {
            const uint32_t peak = params.peak_bitrate ? params.peak_bitrate : params.bitrate;
            if (peak)
                set_control(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, static_cast<int32_t>(peak), "peak bitrate");
        }
    }
    set_control(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "b frames");
    set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop_control(params.gop_size), "gop size");
    if (params.min_qp >= 0)
        set_control(h264 ? V4L2_CID_MPEG_VIDEO_H264_MIN_QP : V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, params.min_qp,
            "min qp");
    if (params.max_qp >= 0)
        set_control(h264 ? V4L2_CID_MPEG_VIDEO_H264_MAX_QP : V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, params.max_qp,
            "max qp");
    configure_deblocking(params);
    // Packed headers are not advertised: the firmware writes SPS/PPS itself.
    // Prefixing them to every sync frame is what makes the Annex-B stream
    // decodable from any IDR.
    if (!device_->set_control(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1))
        trace_("encode control missing name=prepend-spspps-to-idr");
    // Which lever a requested IDR has is a property of the kernel, not of
    // the picture: probe it once here so a requested IDR never discovers a
    // missing control halfway through a stream. A kernel with the
    // REQUEST_SYNC_FRAME capability takes the direct request; one without it
    // gets the intra-period lever instead.
    sync_frame_control_ = device_->set_control(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0);
    trace_("encode idr lever=%s", sync_frame_control_ ? "sync-frame" : "intra-period");

    // CAPTURE carries the frame rate the firmware rates against; OUTPUT
    // carries the operating rate, which only affects clocking. One rate is
    // fixed for the session because S_PARM cannot change while streaming;
    // the per-picture timestamps advance on the same value.
    prepare_ = std::move(prepare);
    rate_num_ = params.frame_rate_num ? params.frame_rate_num : 30;
    rate_den_ = params.frame_rate_den ? params.frame_rate_den : 1;
    device_->set_frame_rate(Queue::Capture, rate_num_, rate_den_);
    device_->set_frame_rate(Queue::Output, rate_num_, rate_den_);

    // Iris sizes coded buffers from the current rate-control controls. A
    // format set before them keeps the buffered-RC size, and queueing then
    // fails EINVAL; re-derive it now that the controls are final and before
    // any buffer exists.
    const FormatInfo coded = device_->set_format(
        Queue::Capture, FormatInfo { config_.codec, config_.width, config_.height, 1, 0, 0 });
    require(coded.pixelformat == config_.codec, "encoder lost the codec after configuration",
        VA_STATUS_ERROR_UNSUPPORTED_PROFILE);

    // Everything below touches kernel state. A failure halfway through must
    // leave nothing behind: configured_ stays false, so a retry runs this
    // function again, and a retry that still found the old mappings in
    // capture_ would append a second set and read completions out of stale
    // slots.
    try {
        require(device_->request_buffers(Queue::Output, kOutputSlots, V4L2_MEMORY_DMABUF) >= kOutputSlots,
            "encoder input buffers", VA_STATUS_ERROR_OPERATION_FAILED);
        const unsigned capture_count = device_->request_buffers(Queue::Capture, kCaptureSlots);
        require(capture_count > 0, "encoder output buffers", VA_STATUS_ERROR_OPERATION_FAILED);
        for (unsigned index = 0; index < capture_count; ++index) {
            const BufferInfo info = device_->query_buffer(Queue::Capture, index);
            capture_.push_back(CaptureSlot { index, info.length, static_cast<uint8_t*>(device_->map_buffer(
                                                                         Queue::Capture, info)) });
        }
        free_output_.clear();
        for (unsigned index = 0; index < kOutputSlots; ++index)
            free_output_.push_back(index);

        device_->stream(Queue::Output, true);
        device_->stream(Queue::Capture, true);
        for (const CaptureSlot& slot : capture_)
            device_->queue_buffer(Queue::Capture, slot.index, slot.length, 0, 0);
    } catch (...) {
        for (const CaptureSlot& slot : capture_) {
            try {
                device_->unmap_buffer(slot.mapping, slot.length);
            } catch (...) {
            }
        }
        capture_.clear();
        free_output_.clear();
        for (Queue queue : { Queue::Capture, Queue::Output }) {
            try {
                device_->stream(queue, false);
            } catch (...) {
            }
        }
        try {
            device_->request_buffers(Queue::Capture, 0);
        } catch (...) {
        }
        try {
            device_->request_buffers(Queue::Output, 0, V4L2_MEMORY_DMABUF);
        } catch (...) {
        }
        throw;
    }

    applied_ = params;
    configured_ = true;
    trace_("encode open node=%s codec=%.4s raw=%ux%u crop=%ux%u mode=%s gop=%u rate=%u/%u",
        device_->path().c_str(), reinterpret_cast<const char*>(&config_.codec), output.width, output.height,
        config_.width, config_.height, rate_mode_name(params.rate_mode), params.gop_size,
        params.frame_rate_num, params.frame_rate_den);
}

void EncoderSession::set_frame_qp(int qp)
{
    const bool h264 = config_.codec == V4L2_PIX_FMT_H264;
    set_control(h264 ? V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP : V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, qp,
        "i-frame qp");
    set_control(h264 ? V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP : V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP, qp,
        "p-frame qp");
}

void EncoderSession::configure_deblocking(const EncodeParams& params)
{
    const bool hevc = config_.codec == V4L2_PIX_FMT_HEVC;
    // [0] mode, [1] H.264 alpha_c0 / HEVC tc, [2] beta. Both codecs have all
    // three or none: they came in together with the Iris encoder node.
    const uint32_t ids[] = {
        static_cast<uint32_t>(hevc ? V4L2_CID_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE
                                   : V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE),
        static_cast<uint32_t>(hevc ? V4L2_CID_MPEG_VIDEO_HEVC_LF_TC_OFFSET_DIV2
                                   : V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_ALPHA),
        static_cast<uint32_t>(hevc ? V4L2_CID_MPEG_VIDEO_HEVC_LF_BETA_OFFSET_DIV2
                                   : V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_BETA),
    };
    bool supported = true;
    for (uint32_t id : ids)
        supported = supported && device_->get_control(id).has_value();

    if (!supported) {
        // Kernels without the loop-filter controls encode with the
        // firmware's enabled, 0/0 defaults and nothing else, so a different
        // request is refused rather than quietly replaced. Mode 2 is the
        // exception: it only turns filtering off across slice boundaries,
        // and this driver encodes one slice per picture, where the two modes
        // produce the same stream.
        require((params.deblock_mode == 0 || (hevc && params.deblock_mode == 2)) && !params.deblock_alpha
                && !params.deblock_beta,
            "deblocking request requires Iris loop-filter controls", VA_STATUS_ERROR_UNIMPLEMENTED);
        trace_("encode deblocking mode=%d alpha=%d beta=%d via=default", params.deblock_mode,
            params.deblock_alpha, params.deblock_beta);
        return;
    }

    // The H.264 menu values are the idc values; HEVC numbers disabled first.
    int mode = params.deblock_mode;
    if (hevc) {
        switch (params.deblock_mode) {
        case 1:
            mode = V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED;
            break;
        case 2:
            mode = V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED_AT_SLICE_BOUNDARY;
            break;
        default:
            mode = V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_ENABLED;
            break;
        }
    }
    set_control(ids[0], mode, hevc ? "hevc loop filter mode" : "h264 loop filter mode");
    set_control(ids[1], params.deblock_alpha, hevc ? "hevc loop filter tc" : "h264 loop filter alpha");
    set_control(ids[2], params.deblock_beta, "loop filter beta");
    // The firmware may still override the offsets per frame under rate
    // control; the request is forwarded as asked, not rewritten.
    trace_("encode deblocking mode=%d alpha=%d beta=%d via=V4L2", params.deblock_mode,
        params.deblock_alpha, params.deblock_beta);
}

void EncoderSession::apply_dynamic(const EncodeParams& params)
{
    // Only the controls the kernel marks CAP_FLAG_DYNAMIC_ALLOWED may be
    // written while the raw queue streams; the rest were set in configure().
    // Deblocking is one of the rest, and the session is already streaming by
    // the time a second picture arrives, so a change needs a new context.
    require(params.deblock_mode == applied_.deblock_mode && params.deblock_alpha == applied_.deblock_alpha
            && params.deblock_beta == applied_.deblock_beta,
        "changing deblocking requires a new context", VA_STATUS_ERROR_UNIMPLEMENTED);
    if (params.rate_mode == RateMode::Cqp && params.qp != applied_.qp) {
        set_frame_qp(params.qp);
        applied_.qp = params.qp;
    }
    if (params.bitrate && params.bitrate != applied_.bitrate) {
        set_control(V4L2_CID_MPEG_VIDEO_BITRATE, static_cast<int32_t>(params.bitrate), "bitrate");
        applied_.bitrate = params.bitrate;
    }
    if (params.rate_mode == RateMode::Cbr) {
        const uint32_t peak = params.peak_bitrate ? params.peak_bitrate : params.bitrate;
        if (peak && peak != applied_.peak_bitrate) {
            set_control(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, static_cast<int32_t>(peak), "peak bitrate");
            applied_.peak_bitrate = peak;
        }
    }
    if (params.gop_size != applied_.gop_size) {
        set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop_control(params.gop_size), "gop size");
        applied_.gop_size = params.gop_size;
    }
}

std::optional<Dequeued> EncoderSession::wait_capture(Clock::time_point limit)
{
    for (;;) {
        if (auto done = device_->dequeue_buffer(Queue::Capture))
            return done;
        if (Clock::now() >= limit)
            return std::nullopt;
        device_->poll(POLLIN | POLLPRI, kPollMs);
    }
}

EncoderSession::Clock::time_point EncoderSession::wait_deadline(uint64_t timeout_ns) const
{
    // The client's timeout can only shorten the session's own bound: a
    // firmware that stops answering must not pin a thread forever just
    // because the client asked for VA_TIMEOUT_INFINITE.
    const Clock::time_point session_limit = deadline();
    if (timeout_ns == VA_TIMEOUT_INFINITE)
        return session_limit;
    const uint64_t capped = std::min<uint64_t>(timeout_ns, kMaxTimeoutNs);
    return std::min(session_limit, Clock::now() + std::chrono::nanoseconds(capped));
}

void EncoderSession::take_output_slot(Clock::time_point limit)
{
    // Called with mutex_ held. Every input slot is with the firmware, so the
    // only way forward is to let it finish something: complete_one() takes
    // the CAPTURE completion, requeues that slot, and returns the input
    // buffer. A client that submits deeper than the slot count (FFmpeg's
    // -async_depth) parks here while another thread may never call wait(),
    // so this path has to pump the queue itself or the firmware stalls with
    // every CAPTURE slot unrearmed.
    while (free_output_.empty()) {
        if (complete_one())
            continue;
        if (Clock::now() >= limit)
            throw Error(VA_STATUS_ERROR_TIMEDOUT, "encoder input buffer was not returned");
        // Drop the lock across poll(): a thread waiting on an earlier picture
        // needs it to claim its completion.
        mutex_.unlock();
        try {
            device_->poll(POLLIN | POLLPRI, kPollMs);
        } catch (...) {
            // The enclosing lock_guard still owns mutex_: re-lock before
            // unwinding, or its destructor would unlock an unlocked mutex.
            mutex_.lock();
            throw;
        }
        mutex_.lock();
    }
}

EncodeTicket EncoderSession::submit(int dmabuf_fd, unsigned dmabuf_length, const EncodeParams& params)
{
    // A client often renders straight into the surface and only flushes: the
    // write is then pending in the DMA-BUF's reservation object, and the
    // kernel attaches no fence of its own, so vb2 would queue the fd exactly
    // as it finds it and the firmware would encode whatever was in memory
    // before. POLLOUT waits on the read *and* write fences
    // (dma_resv_usage_rw(true)), which is the set a new writer has to clear;
    // it is the same call the decode path makes before the firmware writes
    // into a client's buffer.
    //
    // Waited for before the session lock: the fence belongs to the client's
    // buffer, not to the session, so a client still drawing must neither pin
    // an input slot nor hold up a thread claiming an access unit.
    const auto fence_start = Clock::now();
    wait_dma_buf(dmabuf_fd, true, kInputFenceMs);
    const auto fence_ms
        = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - fence_start).count();
    if (fence_ms >= kFenceTraceMs)
        trace_("encode input fence fd=%d waited=%lldms", dmabuf_fd, static_cast<long long>(fence_ms));

    std::lock_guard<std::mutex> lock(mutex_);
    require(configured_ && !finished_, "encoder is not streaming", VA_STATUS_ERROR_OPERATION_FAILED);
    require(healthy_, "encoder is in a failed state", VA_STATUS_ERROR_ENCODING_ERROR);
    apply_dynamic(params);

    // The encoder node has no source-change and no way to code against a
    // reference it was not given, so the stream has to start on an intra
    // picture and every inter picture has to predict from the one just
    // coded. Both are checked here rather than in the codec layer because
    // only the session knows what it already submitted.
    require(params.intra || last_reconstruction_ != 0, "the encoder must start with an intra picture",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    if (!params.intra) {
        require(params.reference != 0 && params.reference == last_reconstruction_,
            "an inter picture must reference the previous picture", VA_STATUS_ERROR_UNIMPLEMENTED);
    }

    // A requested IDR has two possible levers. A kernel that carries the
    // REQUEST_SYNC_FRAME capability exposes V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME
    // and that is the direct request. Kernel branch 7.2.6 does not, and there
    // the intra period is the only one: zero asks the firmware for an intra
    // picture and putting the period back restores the GOP. Either way the
    // lever is pulled for every requested picture, including one that would
    // be an IDR anyway: skipping those made the driver's idea of the schedule
    // drift from the firmware's and silently dropped requests. A stream that
    // is intra-only needs no lever at all.
    const bool force_idr = params.keyframe && params.gop_size != 0;

    // The device layer takes microseconds and the encoder echoes the value
    // into the access unit. Advance by one frame duration per picture rather
    // than recomputing from the current picture's rate: FFmpeg sends
    // VAEncMiscParameterFrameRate only with IDR pictures (the global
    // parameters go with IDR in vaapi_encode.c), so a per-picture rate jumped
    // forward on every IDR and back on the picture after it. The firmware
    // stops returning access units once a timestamp goes backwards, which
    // wedged the session right after each GOP boundary at every rate but the
    // 30 fps fallback. The session rate is fixed in configure(), so this sum
    // only ever rises.
    const uint64_t frame_us = std::max<uint64_t>(1, 1000000ull * rate_den_ / rate_num_);
    timestamp_us_ += frame_us;
    const uint64_t timestamp_us = timestamp_us_;

    const Clock::time_point limit = deadline();
    take_output_slot(limit);
    const unsigned index = free_output_.front();
    free_output_.pop_front();
    // Both intra-period writes happen after take_output_slot(), which can
    // throw when no input slot comes back: pulling the lever before it would
    // leave the session coding intra-only after a stall.
    if (force_idr && !sync_frame_control_)
        set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, 0, "gop size");
    // The intra period goes back to the session's value before the picture is
    // queued, not after its completion: the firmware schedules the next GOP
    // boundary from the period it sees, so restoring afterwards made the
    // picture after a requested one an IDR as well.
    if (force_idr && !sync_frame_control_)
        set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop_control(params.gop_size), "gop size");
    // The direct request. The kernel takes it as a one-shot on the next
    // picture, so it needs no restore.
    if (force_idr && sync_frame_control_)
        set_control(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, "force key frame");
    // bytesused must be non-zero: vb2 rejects a zero-length raw buffer and
    // poisons the instance.
    device_->queue_buffer_dmabuf(Queue::Output, index, dmabuf_fd, dmabuf_length, dmabuf_length, timestamp_us, 0);

    Pending submitted;
    submitted.ticket.timestamp_us = timestamp_us;
    submitted.params = params;
    submitted.force_idr = force_idr;
    issued_us_ = timestamp_us;
    // The next inter picture may predict from this one.
    last_reconstruction_ = params.reconstruction;
    pending_.push_back(std::move(submitted));
    return pending_.back().ticket;
}

bool EncoderSession::complete_one()
{
    // Called with mutex_ held. A completion belongs to the oldest picture
    // still waiting: the firmware answers OUTPUT in the order it consumed it,
    // which is the order submit() queued them. The finished picture leaves
    // pending_ and waits in `completed_` for whichever thread holds its
    // ticket, so a later completion can never overwrite it.
    if (!healthy_)
        throw Error(VA_STATUS_ERROR_ENCODING_ERROR, "encoder session failed");
    const std::optional<Dequeued> done = device_->dequeue_buffer(Queue::Capture);
    if (!done)
        return false;
    // A bare LAST is the drain marker: it carries no payload, so it belongs
    // to no picture. Reading it as one would take the next ticket's access
    // unit and give that ticket an empty buffer.
    if (done->last() && !done->bytesused) {
        trace_("encode drain marker");
        return false;
    }
    // A completion that cannot be attributed to the oldest picture leaves
    // pending_ misaligned: the next picture's bytes would be stored under
    // this ticket. Every one of these failures stops the session instead, so
    // the outstanding tickets fail rather than receive a shifted stream.
    if (done->error()) {
        healthy_ = false;
        trace_("encode error completion index=%u flags=%#x", done->index, done->flags);
        throw Error(VA_STATUS_ERROR_ENCODING_ERROR, "encoder returned an error buffer");
    }
    if (done->index >= capture_.size()) {
        healthy_ = false;
        throw Error(VA_STATUS_ERROR_ENCODING_ERROR, "encoder returned an unknown buffer");
    }
    const CaptureSlot& slot = capture_[done->index];
    if (done->data_offset > done->bytesused || done->bytesused > slot.length) {
        healthy_ = false;
        throw Error(VA_STATUS_ERROR_ENCODING_ERROR, "encoder payload exceeds its buffer");
    }
    if (pending_.empty()) {
        healthy_ = false;
        throw Error(VA_STATUS_ERROR_ENCODING_ERROR, "encoder completed a picture that was never submitted");
    }
    // The firmware echoes the submitted timestamp into the access unit. A
    // mismatch means this completion is not the oldest picture's, and storing
    // it would shift every later ticket; a zero means the kernel did not echo
    // one, and the submission order is all there is to go on.
    if (done->timestamp_us != 0 && done->timestamp_us != pending_.front().ticket.timestamp_us) {
        healthy_ = false;
        trace_("encode completion timestamp=%llu expected=%llu",
            static_cast<unsigned long long>(done->timestamp_us),
            static_cast<unsigned long long>(pending_.front().ticket.timestamp_us));
        throw Error(VA_STATUS_ERROR_ENCODING_ERROR, "encoder completed a picture out of order");
    }

    Pending picture = std::move(pending_.front());
    pending_.pop_front();
    EncodeResult result;
    result.bytes.assign(slot.mapping + done->data_offset, slot.mapping + done->bytesused);
    // A requested IDR came from the intra period, not from the firmware's own
    // sync frame, so it may carry no SPS/PPS: a stream cut there would fail to
    // decode. Prefix the ones the last access unit carried.
    if (picture.force_idr && prepare_)
        result.bytes = prepare_(result.bytes, parameter_sets_);
    // No picture type here: the SM8550 encoder firmware does not send frame
    // info, so V4L2_BUF_FLAG_KEYFRAME is always clear. The bitstream is the
    // evidence (an IDR NAL at every GOP boundary).
    trace_("encode frame index=%u bytes=%zu idr=%d promoted=%d", frames_++, result.bytes.size(),
        picture.params.keyframe ? 1 : 0, picture.params.promoted ? 1 : 0);
    completed_.emplace(picture.ticket.timestamp_us, std::move(result));

    if (!done->last())
        device_->queue_buffer(Queue::Capture, slot.index, slot.length, 0, 0);
    if (auto output = device_->dequeue_buffer(Queue::Output))
        free_output_.push_back(output->index);
    if (done->last())
        drained_ = true;
    cold_ = false;
    return true;
}

EncodeResult EncoderSession::wait(EncodeTicket ticket)
{
    return wait_timeout(ticket, VA_TIMEOUT_INFINITE);
}

EncodeResult EncoderSession::wait_timeout(EncodeTicket ticket, uint64_t timeout_ns)
{
    const Clock::time_point limit = wait_deadline(timeout_ns);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A picture that finished before the session failed still owns
            // its bytes, so the completed table is consulted first: failing
            // this ticket would throw away an access unit that is already
            // here. The same applies after a drain that left a later picture
            // unresolved, which is the other place healthy_ goes false.
            auto it = completed_.find(ticket.timestamp_us);
            if (it != completed_.end()) {
                EncodeResult result = std::move(it->second);
                completed_.erase(it);
                claim(ticket);
                return result;
            }
            // A ticket that was already answered is not an error: FFmpeg
            // syncs the input surface and the coded buffer, and the second
            // call has nothing left to wait for.
            if (claimed(ticket))
                return EncodeResult {};
            require(healthy_, "encoder is in a failed state", VA_STATUS_ERROR_ENCODING_ERROR);
            require(!finished_, "encoder finished with a picture outstanding", VA_STATUS_ERROR_OPERATION_FAILED);
            require(ticket_known(ticket), "ticket does not belong to this session", VA_STATUS_ERROR_INVALID_PARAMETER);
            complete_one();
            it = completed_.find(ticket.timestamp_us);
            if (it != completed_.end()) {
                EncodeResult result = std::move(it->second);
                completed_.erase(it);
                claim(ticket);
                return result;
            }
        }
        require(Clock::now() < limit, "encoder produced no access unit", VA_STATUS_ERROR_TIMEDOUT);
        device_->poll(POLLIN | POLLPRI, kPollMs);
    }
}

void EncoderSession::claim(EncodeTicket ticket)
{
    claimed_us_ = std::max(claimed_us_, ticket.timestamp_us);
}

bool EncoderSession::claimed(EncodeTicket ticket) const
{
    return ticket.timestamp_us != 0 && ticket.timestamp_us <= claimed_us_;
}

bool EncoderSession::ticket_known(EncodeTicket ticket) const
{
    for (const Pending& picture : pending_)
        if (picture.ticket.timestamp_us == ticket.timestamp_us)
            return true;
    return false;
}

EncodeResult EncoderSession::encode(int dmabuf_fd, unsigned dmabuf_length, const EncodeParams& params)
{
    return wait(submit(dmabuf_fd, dmabuf_length, params));
}

void EncoderSession::finish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || finished_)
        return;
    finished_ = true;

    // STOP puts the drain marker on the raw port; the firmware returns the
    // pictures it still holds and marks the last one. Every submitted
    // picture still owes its ticket holder an access unit, so the drain
    // resolves them rather than discarding: a client that submitted and
    // never waited gets its bytes, and one that waits after the context is
    // gone finds them in completed_ instead of a timeout.
    try {
        device_->encoder_command(V4L2_ENC_CMD_STOP);
    } catch (const std::exception& error) {
        trace_("encode drain stop failed: %s", error.what());
    }
    try {
        const Clock::time_point limit = deadline();
        while (!pending_.empty()) {
            if (complete_one())
                continue;
            if (Clock::now() >= limit) {
                trace_("encode drain result=timeout outstanding=%zu", pending_.size());
                break;
            }
            // Complete outside the lock, the way wait() does: the firmware
            // needs time, and holding mutex_ across poll() would block the
            // thread that submitted.
            mutex_.unlock();
            try {
                device_->poll(POLLIN | POLLPRI, kPollMs);
            } catch (...) {
                // The enclosing lock_guard still owns mutex_: re-lock before
                // unwinding, or its destructor would unlock an unlocked mutex.
                mutex_.lock();
                throw;
            }
            mutex_.lock();
        }
        if (pending_.empty() && !drained_)
            trace_("encode drain result=complete");
    } catch (const std::exception& error) {
        trace_("encode drain failed: %s", error.what());
    }
    for (const CaptureSlot& slot : capture_)
        device_->unmap_buffer(slot.mapping, slot.length);
    capture_.clear();
    free_output_.clear();
    for (Queue queue : { Queue::Capture, Queue::Output }) {
        try {
            device_->stream(queue, false);
        } catch (const std::exception& error) {
            trace_("encode streamoff failed: %s", error.what());
        }
    }
    try {
        device_->request_buffers(Queue::Capture, 0);
    } catch (const std::exception& error) {
        trace_("encode release failed: %s", error.what());
    }
    try {
        device_->request_buffers(Queue::Output, 0, V4L2_MEMORY_DMABUF);
    } catch (const std::exception& error) {
        trace_("encode release failed: %s", error.what());
    }
    // A picture the drain could not resolve has no bytes to hand anyone; its
    // ticket is answered with the finished state rather than a stale success.
    if (!pending_.empty()) {
        healthy_ = false;
        pending_.clear();
    }
}

} // namespace iris
