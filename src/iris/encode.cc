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

// The firmware's period with B pictures is an IDR plus a whole number of
// (P, B, ...) groups: 1 + n * (b_frames + 1). A period that leaves a partial
// group puts the firmware's IDR on a picture the client placed inside a pair,
// and the stream never recovers after it: with one B per P, an even period
// broke at the first IDR (each decoded picture ~23 dB against the source)
// while an odd one stayed at 45 dB across every boundary. Round the client's
// request down to the nearest whole group; a client that asks for 60 gets a
// 59-picture GOP.
uint32_t aligned_intra_period(const EncodeParams& params)
{
    if (params.b_frames == 0 || params.gop_size == 0)
        return params.gop_size;
    const uint32_t group = params.b_frames + 1;
    return 1 + ((params.gop_size - 1) / group) * group;
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
    // B pictures are opt-in and H.264 only. The firmware holds the newest
    // queued picture until another arrives, so a client that syncs the
    // picture it just issued must keep one more picture in flight than the
    // default; a client that does not stalls at the first pair boundary for
    // its sync timeout. FFmpeg reaches that shape with -async_depth 3. Its
    // HEVC path requests B pictures as soon as the config advertises a
    // future reference, and this kernel and firmware answer that request
    // with an error completion after nine pictures, so HEVC is refused here
    // rather than left to produce a truncated stream.
    require(!params.b_frames || options_.b_frames, "B pictures are not enabled",
        VA_STATUS_ERROR_UNIMPLEMENTED);
    require(!params.b_frames || h264, "B pictures are not supported for HEVC",
        VA_STATUS_ERROR_UNIMPLEMENTED);
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

    require(params.bit_depth == 8 || params.bit_depth == 10, "encode bit depth must be 8 or 10",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    require(params.bit_depth == 8 || hevc, "10-bit encode is HEVC only",
        VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    // The raw port carries one format per context: P010 for the 10-bit
    // path, NV12 otherwise.
    const bool ten_bit = params.bit_depth > 8;
    const uint32_t raw_fourcc = ten_bit ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
    const FormatInfo output = device_->set_format(
        Queue::Output, FormatInfo { raw_fourcc, config_.width, config_.height, 1, 0, 0 });
    require(output.pixelformat == raw_fourcc, ten_bit ? "encoder did not accept P010" : "encoder did not accept NV12",
        VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    const Layout input_layout
        = stable_layout(ten_bit ? VA_FOURCC_P010 : VA_FOURCC_NV12, config_.width, config_.height);
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
        // Rate control off: the per-picture-type QP controls decide every
        // picture, including the B control the firmware does not derive.
        set_frame_qp(params.qp);
    } else {
        set_control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, bitrate_mode(params.rate_mode), "bitrate mode");
        if (params.bitrate)
            set_control(V4L2_CID_MPEG_VIDEO_BITRATE, static_cast<int32_t>(params.bitrate), "bitrate");
        // CBR always has a peak (its target when the client gave none). VBR
        // has one only when the client's maximum is above its target, and
        // leaving it unwritten would hand the firmware a default instead of
        // the ceiling the client asked for.
        const uint32_t peak = params.rate_mode == RateMode::Cbr && !params.peak_bitrate
            ? params.bitrate
            : params.peak_bitrate;
        if (peak)
            set_control(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, static_cast<int32_t>(peak), "peak bitrate");
    }
    // The firmware schedules the GOP from this one number: it decides which
    // pictures are B and in what order they are coded. It has no per-buffer
    // picture-type control, so the client's slice and reference lists are
    // checked for consistency but do not reach the kernel.
    set_control(V4L2_CID_MPEG_VIDEO_B_FRAMES, static_cast<int32_t>(params.b_frames), "b frames");
    set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop_control(aligned_intra_period(params)), "gop size");
    // Rolling intra refresh. The firmware acts on the period only under CBR:
    // measured byte-identical to a stream without it under VBR and CQP
    // (TEST-RESULTS.md, "Encoder capabilities"), so a client asking for it in
    // either mode is refused rather than handed a stream that ignores the
    // request.
    if (params.intra_refresh_period) {
        require(params.rate_mode == RateMode::Cbr,
            "rolling intra refresh needs CBR; the firmware ignores it otherwise",
            VA_STATUS_ERROR_UNIMPLEMENTED);
        set_control(V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE,
            V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE_CYCLIC, "intra refresh type");
        set_control(V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD,
            static_cast<int32_t>(params.intra_refresh_period), "intra refresh period");
        trace_("encode intra-refresh period=%u", params.intra_refresh_period);
    }
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
    // The ROI control is a dynamic s32 array the module registers with the
    // 0017 patch. Its dims bound the map; a context whose grid is larger than
    // that is not encodable with ROI, and the picture is refused rather than
    // written and failed by the kernel.
    if (options_.roi)
        if (const auto roi = device_->query_control(kRoiDeltaQpControl))
            roi_capacity_ = roi->dims[0];

    // CAPTURE carries the frame rate the firmware rates against; OUTPUT
    // carries the operating rate, which only affects clocking. One rate is
    // fixed for the session because S_PARM cannot change while streaming;
    // the per-picture timestamps advance on the same value.
    prepare_ = std::move(prepare);
    rate_num_ = params.frame_rate_num ? params.frame_rate_num : 30;
    rate_den_ = params.frame_rate_den ? params.frame_rate_den : 1;
    frame_us_ = std::max<uint64_t>(1, 1000000ull * rate_den_ / rate_num_);
    device_->set_frame_rate(Queue::Capture, rate_num_, rate_den_);
    device_->set_frame_rate(Queue::Output, rate_num_, rate_den_);

    // Multi-slice. The kernel turns the macroblock budget into the firmware's
    // slice count, and it accepts one only under CBR or with rate control
    // off, at 60 fps or less, within the codec's slice geometry. Both are
    // per-context controls, so the count is fixed here; apply_dynamic()
    // refuses a later picture that changes it.
    if (params.slices > 1) {
        // The kernel's frame rate is the denominator over the numerator,
        // truncated, which is what its own slice check compares.
        const unsigned fps = rate_num_ ? rate_den_ / rate_num_ : 0;
        require(params.rate_mode != RateMode::Vbr,
            "multi-slice encode needs CBR or CQP; the firmware refuses it under VBR",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        require(fps <= 60, "multi-slice encode is limited to 60 fps", VA_STATUS_ERROR_INVALID_PARAMETER);
        require(config_.width >= (hevc ? 384u : 192u) && config_.height >= 128
                && config_.width <= 4096 && config_.height <= 2160,
            "multi-slice encode is limited to 4096x2160 and the codec's smallest slice shape",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        const unsigned slice_mb = std::max<unsigned>(1, params.slice_mb);
        set_control(V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE, V4L2_MPEG_VIDEO_MULTI_SLICE_MODE_MAX_MB,
            "multi-slice mode");
        set_control(V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_MB, static_cast<int32_t>(slice_mb), "multi-slice mb");
        trace_("encode slices=%u mode=max_mb mb=%u", params.slices, slice_mb);
    }

    // Temporal layers. The firmware derives the layer membership from the
    // count, so that is all the node takes; H.264 has an explicit enable and
    // HEVC's layer count is its own switch. Per-layer bitrates are not
    // written: the advertised attribute says per-layer rate control is off,
    // and the firmware ignores them while a total bitrate is set.
    if (params.temporal_layers > 1) {
        require(params.temporal_layers <= 6, "temporal layers must be between 1 and 6",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        if (h264) {
            set_control(V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_TYPE,
                V4L2_MPEG_VIDEO_H264_HIERARCHICAL_CODING_P, "hierarchical coding type");
            set_control(V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER,
                static_cast<int32_t>(params.temporal_layers - 1), "hierarchical coding layers");
            set_control(V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING, 1, "hierarchical coding");
        } else {
            set_control(V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_TYPE,
                V4L2_MPEG_VIDEO_HEVC_HIERARCHICAL_CODING_P, "hierarchical coding type");
            set_control(V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_LAYER,
                static_cast<int32_t>(params.temporal_layers - 1), "hierarchical coding layers");
        }
        trace_("encode temporal layers=%u", params.temporal_layers);
    }

    // Iris sizes coded buffers from the current rate-control controls. A
    // format set before them keeps the buffered-RC size, and queueing then
    // fails EINVAL; re-derive it now that the controls are final and before
    // any buffer exists.
    const FormatInfo coded = device_->set_format(
        Queue::Capture, FormatInfo { config_.codec, config_.width, config_.height, 1, 0, 0 });
    require(coded.pixelformat == config_.codec, "encoder lost the codec after configuration",
        VA_STATUS_ERROR_UNSUPPORTED_PROFILE);

    // The kernel arms one ROI metadata buffer per control write and attaches
    // it to the next queued picture. It builds the buffer and subscribes the
    // firmware's metadata delivery while the session is starting, so the
    // array must be in place before the queues stream; a client writes its
    // first map with the picture it belongs to, which is later. Arm an
    // all-zero map now, once, and every per-picture write replaces it.
    if (roi_capacity_ > 0 && roi_cells() <= roi_capacity_) {
        const std::vector<int32_t> zeros(roi_cells(), 0);
        if (!device_->set_control_array(kRoiDeltaQpControl, zeros.data(), zeros.size()))
            roi_capacity_ = 0;
    }

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
    trace_("encode open node=%s codec=%.4s raw=%.4s depth=%u %ux%u crop=%ux%u mode=%s gop=%u rate=%u/%u",
        device_->path().c_str(), reinterpret_cast<const char*>(&config_.codec),
        reinterpret_cast<const char*>(&raw_fourcc), params.bit_depth, output.width, output.height,
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
    // The B control has a default of its own (QP 20 on this kernel) and is
    // not derived from the other two. Left alone, a CQP session with B
    // pictures codes them at that default whatever the client asked for: at
    // QP 26 that measured 15.5 Mbit/s against 4.6, and at QP 30 19.2 against
    // 2.2. Inert while B pictures are off.
    if (h264)
        set_control(V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP, qp, "b-frame qp");
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
    // The B count is a per-context control too, and changing it mid-GOP
    // would make the firmware's schedule disagree with the pictures already
    // coded against the old one.
    require(params.b_frames == applied_.b_frames,
        "changing the B picture count requires a new context", VA_STATUS_ERROR_UNIMPLEMENTED);
    // The slice budget is written into a per-context control too, and a
    // change would split the pictures already coded against the old one
    // differently from the rest of the stream.
    require(params.slices == applied_.slices && params.slice_mb == applied_.slice_mb,
        "changing the slice count requires a new context", VA_STATUS_ERROR_UNIMPLEMENTED);
    // The raw port format is fixed when the queue starts: the kernel has no
    // usable transition between NV12 and P010 on a streaming output queue.
    require(params.bit_depth == applied_.bit_depth,
        "changing the encode bit depth requires a new context", VA_STATUS_ERROR_UNIMPLEMENTED);
    // The layer count is a per-context control too: the pictures already
    // coded belong to the old hierarchy.
    require(params.temporal_layers == applied_.temporal_layers,
        "changing the temporal layer count requires a new context", VA_STATUS_ERROR_UNIMPLEMENTED);
    // The refresh period is per-context in V4L2. The kernel marks it dynamic
    // only for a random period, and this driver writes the cyclic one, so a
    // change needs a new context rather than a write the kernel refuses.
    require(params.intra_refresh_period == applied_.intra_refresh_period,
        "changing the intra refresh period requires a new context", VA_STATUS_ERROR_UNIMPLEMENTED);
    if (params.rate_mode == RateMode::Cqp && params.qp != applied_.qp) {
        set_frame_qp(params.qp);
        applied_.qp = params.qp;
    }
    if (params.bitrate && params.bitrate != applied_.bitrate) {
        set_control(V4L2_CID_MPEG_VIDEO_BITRATE, static_cast<int32_t>(params.bitrate), "bitrate");
        applied_.bitrate = params.bitrate;
    }
    const uint32_t peak = params.rate_mode == RateMode::Cbr && !params.peak_bitrate
        ? params.bitrate
        : params.peak_bitrate;
    if (peak && peak != applied_.peak_bitrate) {
        set_control(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, static_cast<int32_t>(peak), "peak bitrate");
        applied_.peak_bitrate = peak;
    }
    if (params.gop_size != applied_.gop_size) {
        set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop_control(aligned_intra_period(params)), "gop size");
        applied_.gop_size = params.gop_size;
    }
}

size_t EncoderSession::roi_cells() const
{
    const unsigned lcu = config_.codec == V4L2_PIX_FMT_HEVC ? 32 : 16;
    return static_cast<size_t>((config_.width + lcu - 1) / lcu) * ((config_.height + lcu - 1) / lcu);
}

void EncoderSession::apply_roi(const EncodeParams& params)
{
    if (params.roi_map.empty() && !roi_used_)
        return;
    require(roi_capacity_ > 0, "ROI needs the kernel ROI control (packaging/kernel/0017-*)",
        VA_STATUS_ERROR_UNIMPLEMENTED);
    const size_t cells = roi_cells();
    require(cells <= roi_capacity_, "the ROI map is larger than the kernel control takes",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    require(params.roi_map.empty() || params.roi_map.size() == cells,
        "the ROI map does not match the encoder's macroblock grid", VA_STATUS_ERROR_INVALID_PARAMETER);
    // A picture with no regions still gets a write once the stream has used
    // ROI: the kernel arms one metadata buffer per write and attaches it to
    // the next raw buffer, so skipping the write would let the firmware reuse
    // the previous picture's map.
    const std::vector<int32_t> zeros = params.roi_map.empty() ? std::vector<int32_t>(cells, 0)
                                                              : std::vector<int32_t>();
    const int32_t* map = params.roi_map.empty() ? zeros.data() : params.roi_map.data();
    if (!device_->set_control_array(kRoiDeltaQpControl, map, cells))
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED, "encoder ROI control disappeared");
    roi_used_ = true;
    if (params.roi_regions)
        trace_("encode roi regions=%u cells=%zu", params.roi_regions, cells);
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
        // complete_one() pumps OUTPUT even when no coded frame is ready, so a
        // consumed input may have refilled the slot without a completion.
        if (!free_output_.empty())
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
    require(!drain_sent_, "the encoder has been drained", VA_STATUS_ERROR_OPERATION_FAILED);
    require(healthy_, "encoder is in a failed state", VA_STATUS_ERROR_ENCODING_ERROR);

    // The encoder node has no source-change and no way to code against a
    // reference it was not given, so the stream has to start on an intra
    // picture and every inter picture has to predict from the one just
    // coded. Both are checked here rather than in the codec layer because
    // only the session knows what it already submitted.
    require(params.intra || last_reconstruction_ != 0, "the encoder must start with an intra picture",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    // Without B pictures the driver is the only thing keeping the reference
    // structure legal, and it holds the one reference it just coded. With
    // them the firmware schedules the GOP, a B picture's past reference is
    // two pictures back, and the client's lists describe a structure the
    // node never receives; the codec layer still requires a valid past
    // reference (and a future one for B), but not this identity.
    if (!params.intra && params.b_frames == 0) {
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
    timestamp_us_ += frame_us_;
    const uint64_t timestamp_us = timestamp_us_;
    issued_us_ = timestamp_us;

    // The client's order is relative to the newest IDR, so put it on a
    // session-wide scale before comparing pictures with it. A keyframe is the
    // next picture in display order after everything already submitted, which
    // is exactly the anchor the client's count restarts from.
    if (params.keyframe)
        poc_base_ = last_display_ + 1 - params.display_order;
    const int64_t display = params.display_order + poc_base_;
    if (display > last_display_)
        last_display_ = display;

    Pending submitted;
    submitted.ticket.timestamp_us = timestamp_us;
    submitted.params = params;
    submitted.params.display_order = static_cast<int32_t>(display);
    submitted.force_idr = force_idr;
    submitted.dmabuf_fd = dmabuf_fd;
    submitted.dmabuf_length = dmabuf_length;
    submitted.submitted_at = Clock::now();

    if (applied_.b_frames == 0) {
        queue_pending(submitted);
        pending_.push_back(std::move(submitted));
    } else if (!held_) {
        // Nothing older can follow the first picture of a session.
        held_ = std::move(submitted);
    } else if (submitted.params.display_order < held_->params.display_order) {
        // A B picture for a frame the future reference skipped over: it is
        // the older of the two and goes first.
        queue_pending(submitted);
        pending_.push_back(std::move(submitted));
        queue_pending(*held_);
        pending_.push_back(std::move(*held_));
        held_.reset();
    } else {
        queue_pending(*held_);
        pending_.push_back(std::move(*held_));
        held_ = std::move(submitted);
    }
    // The next inter picture may predict from this one.
    last_reconstruction_ = params.reconstruction;
    return EncodeTicket { timestamp_us };
}

void EncoderSession::queue_pending(Pending& picture)
{
    // Called with mutex_ held, in queue order. Every per-picture control is
    // written here rather than in submit(): with B pictures the queue order
    // is not the submission order, and a QP or a rate written at submission
    // time would land on the wrong picture.
    apply_dynamic(picture.params);
    // The ROI map belongs to the raw buffer queued below: the kernel arms one
    // metadata buffer per write and attaches it to the next QBUF. Written
    // before the slot is taken so a refused map does not consume one.
    apply_roi(picture.params);
    const Clock::time_point limit = deadline();
    take_output_slot(limit);
    const unsigned index = free_output_.front();
    free_output_.pop_front();
    const bool force_idr = picture.force_idr;
    const EncodeParams& params = picture.params;
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
        set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop_control(aligned_intra_period(params)), "gop size");
    // The direct request. The kernel takes it as a one-shot on the next
    // picture, so it needs no restore.
    if (force_idr && sync_frame_control_)
        set_control(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, "force key frame");
    queue_us_ += frame_us_;
    picture.queue_us = queue_us_;
    queued_tickets_[picture.queue_us] = picture.ticket.timestamp_us;
    // bytesused must be non-zero: vb2 rejects a zero-length raw buffer and
    // poisons the instance.
    device_->queue_buffer_dmabuf(Queue::Output, index, picture.dmabuf_fd, picture.dmabuf_length,
        picture.dmabuf_length, picture.queue_us, 0);
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
    // The firmware returns an input buffer when it has read the picture,
    // which is before the coded frame exists. Harvest those first: the slot
    // becomes usable again immediately, and vaSyncSurface on the input
    // surface is answered without waiting for the access unit.
    pump_output();
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
    // The firmware echoes the submitted timestamp into the access unit.
    // Without B pictures it answers in submission order, so the echo names
    // the oldest pending picture. With them it answers in display order: the
    // future reference was submitted ahead of its B picture, and the B comes
    // back first. Match the echo rather than the queue position. A zero echo
    // means the kernel carries no timestamp and submission order is all
    // there is to go on, which is the in-order case.
    auto picture_it = pending_.begin();
    if (done->timestamp_us != 0) {
        picture_it = std::find_if(pending_.begin(), pending_.end(),
            [&](const Pending& p) { return p.queue_us == done->timestamp_us; });
        if (picture_it == pending_.end()) {
            healthy_ = false;
            trace_("encode completion timestamp=%llu matches no pending picture",
                static_cast<unsigned long long>(done->timestamp_us));
            throw Error(VA_STATUS_ERROR_ENCODING_ERROR, "encoder completed a picture that was never queued");
        }
    }

    Pending picture = std::move(*picture_it);
    pending_.erase(picture_it);
    EncodeResult result;
    result.bytes.assign(slot.mapping + done->data_offset, slot.mapping + done->bytesused);
    // A requested IDR came from the intra period, not from the firmware's own
    // sync frame, so it may carry no SPS/PPS: a stream cut there would fail to
    // decode. Prefix the ones the last access unit carried.
    if (picture.force_idr && prepare_)
        result.bytes = prepare_(result.bytes, parameter_sets_);
    // The client's raw packed headers (SEI) travel in front of the coded
    // picture. The firmware wrote the parameter sets itself, so inserting
    // them before the first slice puts them after those sets, where a
    // decoder expects a prefix SEI.
    if (!picture.params.prefix_nals.empty()) {
        const codec::Codec codec = config_.codec == V4L2_PIX_FMT_H264 ? codec::Codec::H264 : codec::Codec::Hevc;
        size_t bytes = 0;
        for (const std::vector<uint8_t>& nal : picture.params.prefix_nals) {
            result.bytes = codec::insert_before_vcl(result.bytes, nal, codec);
            bytes += nal.size();
        }
        trace_("encode packed header bytes=%zu count=%zu", bytes, picture.params.prefix_nals.size());
    }
    // No picture type here: the SM8550 encoder firmware does not send frame
    // info, so V4L2_BUF_FLAG_KEYFRAME is always clear. The bitstream is the
    // evidence (an IDR NAL at every GOP boundary). The latency is measured
    // from the client's submission, so it includes any queueing the client
    // asked for; `first` marks the session's first picture, whose firmware
    // bring-up is not representative.
    const unsigned index = frames_++;
    const long long latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - picture.submitted_at)
                                     .count();
    trace_("encode frame index=%u bytes=%zu idr=%d promoted=%d latency_us=%lld first=%d", index,
        result.bytes.size(), picture.params.keyframe ? 1 : 0, picture.params.promoted ? 1 : 0, latency_us,
        index == 0 ? 1 : 0);
    completed_.emplace(picture.ticket.timestamp_us, std::move(result));

    if (!done->last())
        device_->queue_buffer(Queue::Capture, slot.index, slot.length, 0, 0);
    pump_output();
    if (done->last())
        drained_ = true;
    cold_ = false;
    return true;
}

void EncoderSession::flush_successor(EncodeTicket ticket)
{
    // Called with mutex_ held. A wait never returns empty-handed, so a picture
    // the session is holding back has to be queued in two cases. The client
    // may be waiting for that picture itself: FFmpeg syncs the access unit of
    // the picture it just issued, and a held picture would then wait for a
    // submit that never comes. Or it may be waiting for the picture's
    // display-order predecessor, which the firmware cannot code without the
    // successor in its queue. With one B picture per P those two are the only
    // pairings that occur.
    if (!held_)
        return;
    if (held_->ticket.timestamp_us == ticket.timestamp_us) {
        queue_pending(*held_);
        pending_.push_back(std::move(*held_));
        held_.reset();
        return;
    }
    const auto it = std::find_if(pending_.begin(), pending_.end(),
        [&](const Pending& picture) { return picture.ticket.timestamp_us == ticket.timestamp_us; });
    if (it == pending_.end() || it->params.display_order + 1 != held_->params.display_order)
        return;
    queue_pending(*held_);
    pending_.push_back(std::move(*held_));
    held_.reset();
}

bool EncoderSession::drain_locked(Clock::time_point limit)
{
    // Called with mutex_ held. STOP puts the drain marker on the raw port and
    // the firmware returns the pictures it still holds, the last one marked.
    // It is not repeatable: after it the firmware codes nothing else.
    //
    // A picture still waiting in held_ has not been queued at all; it is the
    // last one submitted, so it is also the last in display order. Queue it
    // before the marker or the firmware never sees it.
    if (held_) {
        queue_pending(*held_);
        pending_.push_back(std::move(*held_));
        held_.reset();
    }
    if (!drain_sent_) {
        // The marker is one-shot and the firmware codes nothing after it, so
        // the flag follows a STOP that the driver accepted. Marking it
        // before the ioctl left a failed STOP (EBUSY while buffers were
        // still queued) looking like a stream that had already drained, and
        // every later wait sat on pictures that would never arrive.
        try {
            device_->encoder_command(V4L2_ENC_CMD_STOP);
            drain_sent_ = true;
        } catch (const std::exception& error) {
            trace_("encode drain stop failed: %s", error.what());
        }
    }
    bool arrived = false;
    while (!pending_.empty()) {
        if (complete_one()) {
            arrived = true;
            continue;
        }
        if (Clock::now() >= limit)
            break;
        // Complete outside the lock, the way wait() does: the firmware needs
        // time, and holding mutex_ across poll() would block the thread that
        // submitted.
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
    return arrived;
}

void EncoderSession::pump_output()
{
    // Called with mutex_ held. A freed input slot is usable again even while
    // its picture is still being coded, which is exactly what the kernel
    // promises: OUTPUT DQBUF returns ownership of the raw buffer.
    while (auto output = device_->dequeue_buffer(Queue::Output)) {
        free_output_.push_back(output->index);
        if (!output->timestamp_us)
            continue;
        // The kernel echoes the queue stamp, which is not the ticket when B
        // pictures reorder the queue. The picture is still in pending_ at
        // this point: the output is released before its access unit exists.
        uint64_t ticket = 0;
        for (const Pending& picture : pending_)
            if (picture.queue_us == output->timestamp_us)
                ticket = picture.ticket.timestamp_us;
        if (!ticket) {
            const auto it = queued_tickets_.find(output->timestamp_us);
            if (it != queued_tickets_.end())
                ticket = it->second;
        }
        if (ticket)
            input_released_.insert(ticket);
        // A client that never syncs its input surfaces never drains this
        // set. Keep it bounded; an answer dropped here is recovered from
        // completed_/claimed, or by waiting for the access unit.
        while (input_released_.size() > 64)
            input_released_.erase(input_released_.begin());
    }
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
            // The firmware codes in display order and holds the newest queued
            // picture until its successor arrives. When the client is waiting
            // for the earlier half of a pair, that successor is the one
            // sitting in held_: neither completes until it is queued.
            flush_successor(ticket);
        }
        if (Clock::now() >= limit) {
            // With B pictures the firmware keeps the last picture until it is
            // told there is no more input. VA has no end-of-stream call, and
            // a VAAPI client waits for its tail before it destroys the
            // context, so a wait that outlives the session bound is the only
            // signal the driver gets. Drain and take what arrives; a session
            // that drains this way refuses further pictures.
            bool arrived = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // A zero timeout is a poll, not a verdict. Draining here
                // would end the stream on the first vaSyncBuffer(..., 0).
                if (timeout_ns != 0 && applied_.b_frames != 0 && !drain_sent_)
                    arrived = drain_locked(Clock::now() + std::chrono::milliseconds(sync_timeout_ms()));
            }
            if (arrived)
                continue;
            throw Error(VA_STATUS_ERROR_TIMEDOUT, "encoder produced no access unit");
        }
        device_->poll(POLLIN | POLLPRI, kPollMs);
    }
}

void EncoderSession::wait_input(EncodeTicket ticket)
{
    const Clock::time_point limit = deadline();
    // Idempotent: a client that syncs the same input surface twice must not
    // block the second time. The set is pruned at each claim and capped in
    // pump_output, so keeping the answer costs bounded memory.
    auto settled = [&] {
        return input_released_.count(ticket.timestamp_us) != 0 || completed_.count(ticket.timestamp_us) != 0
            || claimed(ticket);
    };
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (settled())
                return;
            require(healthy_, "encoder is in a failed state", VA_STATUS_ERROR_ENCODING_ERROR);
            require(!finished_, "encoder finished with a picture outstanding", VA_STATUS_ERROR_OPERATION_FAILED);
            require(ticket_known(ticket), "ticket does not belong to this session", VA_STATUS_ERROR_INVALID_PARAMETER);
            pump_output();
            if (settled())
                return;
            // No input completion yet: one coded completion is the wait the
            // client would otherwise do through the coded buffer.
            complete_one();
            flush_successor(ticket);
            if (settled())
                return;
        }
        require(Clock::now() < limit, "encoder did not release the input buffer", VA_STATUS_ERROR_TIMEDOUT);
        device_->poll(POLLIN | POLLPRI, kPollMs);
    }
}

void EncoderSession::claim(EncodeTicket ticket)
{
    // A claim that leaves an older picture pending is a B picture that came
    // back before its future reference; the watermark would then mark that
    // reference answered and wait() would hand its holder an empty result.
    const bool jumped = std::any_of(pending_.begin(), pending_.end(),
        [&](const Pending& p) { return p.ticket.timestamp_us < ticket.timestamp_us; });
    if (jumped) {
        claimed_out_of_order_.insert(ticket.timestamp_us);
        while (claimed_out_of_order_.size() > 64)
            claimed_out_of_order_.erase(claimed_out_of_order_.begin());
    } else {
        claimed_us_ = std::max(claimed_us_, ticket.timestamp_us);
    }
    // Nothing at or below the newest claimed ticket can still be asked about
    // by an input sync that has not already been answered.
    while (!input_released_.empty() && *input_released_.begin() <= claimed_us_)
        input_released_.erase(input_released_.begin());
    // The queue stamp of a claimed picture is dead: its input release, if any,
    // already landed in input_released_ before the claim.
    for (auto it = queued_tickets_.begin(); it != queued_tickets_.end();) {
        if (it->second <= claimed_us_ || claimed_out_of_order_.count(it->second))
            it = queued_tickets_.erase(it);
        else
            ++it;
    }
}

bool EncoderSession::claimed(EncodeTicket ticket) const
{
    return ticket.timestamp_us != 0
        && (ticket.timestamp_us <= claimed_us_
            || claimed_out_of_order_.count(ticket.timestamp_us) != 0);
}

bool EncoderSession::ticket_known(EncodeTicket ticket) const
{
    if (held_ && held_->ticket.timestamp_us == ticket.timestamp_us)
        return true;
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
        drain_locked(deadline());
        if (!pending_.empty())
            trace_("encode drain result=timeout outstanding=%zu", pending_.size());
        else if (!drained_)
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
