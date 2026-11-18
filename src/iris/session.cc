// SPDX-License-Identifier: MIT

#include "session.h"

#include "session_internal.h"

#include "../util/error.h"
#include "../util/prime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

extern "C" {
#include <linux/v4l2-controls.h>
#include <poll.h>
}

namespace iris {

namespace {

constexpr unsigned kMinCapturePool = 32;
constexpr unsigned kMaxCapturePool = 64;
constexpr int kLastWaitMs = 2000;
constexpr int kOutputDrainMs = 500;

// The device's contiguous capture memory is a fixed carve-out on SM8550, and
// it holds the CAPTURE pool alongside the firmware's DPB and scratch buffers.
// A 7680x4320 NV12 frame is 47.5 MiB, so a 32-frame pool asks for 1.5 GiB and
// STREAMON fails with ENOMEM; the same node allocates 8 frames and decodes the
// four codecs byte-exactly. Cap the pool by that budget instead of letting a
// large geometry turn a fixed count into an unbounded allocation.
constexpr uint64_t kCaptureMemoryBudget = 384ull << 20;

// The same pool when the kernel allocates CAPTURE from system memory and maps
// it through the IOMMU (capture_sg). 720 MiB is the 15 8K frames a Direct
// target needs: the firmware minimum of seven plus the eight-picture headroom.
// A larger pool fragmented the memory the HEVC bitstream buffer asks for.
constexpr uint64_t kSystemCaptureMemoryBudget = 720ull << 20;

// Decoded pictures a client can hold between completion and consumption.
// The driver cannot see that queue: FFmpeg's hwdownload filter backlog held
// eight 8K pictures between the last capture and the sync that waited for the
// ninth. A Direct target pins one CAPTURE frame for every one of them, so a
// pool with less headroom than this over the firmware's minimum can be pinned
// completely while access units are still queued, and neither side can move.
// At 8K the budget leaves exactly the firmware minimum plus one.
constexpr unsigned kDirectHeadroom = 8;

// Compressed AU capacity when the caller has no better estimate: Chromium
// and GStreamer both size the OUTPUT plane from the coded area with a floor.
unsigned default_au_capacity(unsigned width, unsigned height)
{
    const uint64_t area = static_cast<uint64_t>(width) * height;
    // 4K IDR frames at high bit rates exceed 1 MiB; 8K stays under 8 MiB.
    if (area > 1920u * 1088u)
        return static_cast<unsigned>(std::min<uint64_t>(8u << 20, std::max<uint64_t>(2u << 20, area)));
    return 2u << 20;
}

unsigned capture_pool_count(unsigned wanted, uint64_t frame_size, int firmware_min, uint64_t budget)
{
    if (frame_size == 0)
        return wanted;
    // The firmware's minimum is not negotiable: it needs that many slots to
    // run at all, even when the budget would prefer fewer.
    const unsigned floor = firmware_min > 0 ? static_cast<unsigned>(firmware_min) : 1u;
    const uint64_t affordable = std::max<uint64_t>(floor, budget / frame_size);
    return std::min(wanted, static_cast<unsigned>(std::min<uint64_t>(affordable, kMaxCapturePool)));
}

} // namespace

Session::Session(std::unique_ptr<Device> device, const SessionConfig& config, const Options& options,
    CopyEngine* copier, const Trace& trace)
    : device_(std::move(device))
    , config_(config)
    , options_(options)
    , copier_(copier)
    , trace_(trace)
    , pool_(*device_)
{
    require(config_.width && config_.height && config_.codec_pixelformat, "session geometry",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    visible_ = Selection { 0, 0, config_.width, config_.height };
    if (config_.au_capacity == 0)
        config_.au_capacity = default_au_capacity(config_.width, config_.height);
    if (!options_.dump_dir.empty()) {
        std::string path = options_.dump_dir + "/iris-session-" + std::to_string(reinterpret_cast<uintptr_t>(this))
            + ".bin";
        dump_ = std::fopen(path.c_str(), "wb");
    }
    try {
        configure_output();
    } catch (...) {
        if (dump_)
            std::fclose(static_cast<FILE*>(dump_));
        throw;
    }
}

Session::~Session()
{
    // Callers are expected to finish() first; a destructor cannot wait on
    // hardware. Stop streaming so the kernel returns every buffer, then let
    // the pool close fds. Frames still held by targets outlive the session
    // (they hold their own fd) but must not be requeued: they never are,
    // because the pool is gone.
    try {
        if (state_ != SessionState::Failed) {
            device_->stream(Queue::Capture, false);
            device_->stream(Queue::Output, false);
        }
    } catch (...) {
    }
    for (auto& [token, target] : pending_) {
        target->pending = false;
        target->error = true;
    }
    pending_.clear();
    retained_.clear();
    // A session destroyed without finish() still owns one duplicated client
    // fd per unresolved Import entry.
    clear_import_backlog();
    if (dump_)
        std::fclose(static_cast<FILE*>(dump_));
}

int Session::sync_timeout_ms() const
{
    if (cold_ && !options_.sync_timeout_overridden)
        return options_.cold_start_timeout_ms;
    return options_.sync_timeout_ms;
}

bool Session::expired(std::chrono::steady_clock::time_point deadline) const
{
    return std::chrono::steady_clock::now() >= deadline;
}

bool Session::supports_import() const
{
    // The codec qualification comes from the capability table, never from a
    // local guess; see data/iris-codec-capabilities.json ("publish_import")
    // and the note there for why VP9 is a fallback.
    if (!config_.import_allowed || mode_ != SessionMode::DecodeOrder || import_rejected_)
        return false;
    return pool_.capture_count() == 0 || pool_.capture_memory() == CaptureMemory::Import;
}



bool Session::import_compatible(const Layout& client, const Layout& capture)
{
    // The firmware writes with the CAPTURE pitch and puts chroma after
    // storage_height luma rows; the client's exported offsets are fixed, so
    // both must agree exactly and the buffer must hold the whole plane set.
    if (capture.compressed()) {
        if (client.fourcc != capture.fourcc || client.modifier != capture.modifier
            || client.plane_count != capture.plane_count || client.size < capture.size)
            return false;
        for (unsigned i = 0; i < capture.plane_count; ++i)
            if (client.planes[i].offset != capture.planes[i].offset
                || client.planes[i].pitch != capture.planes[i].pitch)
                return false;
        return true;
    }
    return client.fourcc == capture.fourcc && client.stride == capture.stride
        && client.chroma_offset() == capture.chroma_offset() && client.size >= capture.size;
}

// ---------------------------------------------------------------------------
// Bring-up

void Session::configure_output()
{
    device_->subscribe(V4L2_EVENT_SOURCE_CHANGE);
    FormatInfo request;
    request.pixelformat = config_.codec_pixelformat;
    request.width = config_.width;
    request.height = config_.height;
    request.sizeimage = config_.au_capacity;
    const FormatInfo granted = device_->set_format(Queue::Output, request);
    require(granted.pixelformat == config_.codec_pixelformat, "decoder rejected the codec",
        VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    config_.au_capacity = granted.sizeimage ? granted.sizeimage : config_.au_capacity;

    // Decode-order output if the kernel offers it (docs/architecture.md §2).
    // Both controls must be set before OUTPUT streams; the driver rejects
    // changes afterwards.
    if (device_->set_control(V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY, 0)
        && device_->set_control(V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE, 1))
        mode_ = SessionMode::DecodeOrder;
    else
        mode_ = SessionMode::DisplayOrder;

    pool_.allocate_output(config_.output_slots);
    device_->stream(Queue::Output, true);
    state_ = SessionState::Configured;
    trace_("session open node=%s mode=%s codec=%c%c%c%c %ux%u au=%u output=%u", device_->path().c_str(),
        mode_ == SessionMode::DecodeOrder ? "decode-order" : "display-order", config_.codec_pixelformat & 0xff,
        (config_.codec_pixelformat >> 8) & 0xff, (config_.codec_pixelformat >> 16) & 0xff,
        (config_.codec_pixelformat >> 24) & 0xff, config_.width, config_.height, config_.au_capacity,
        pool_.output_count());
}

void Session::configure_capture()
{
    // Only meaningful after SOURCE_CHANGE: the decoder has parsed the
    // stream and knows the coded geometry.
    //
    // At a resolution change the driver has already derived the new coded
    // size, stride and crop from the bitstream (iris_hfi_gen2_handle_src_change
    // -> iris_hfi_gen2_read_input_subcr_params) and hands them to the firmware
    // on the resume. Asking for the format again would replace them with this
    // client's guess before that resume, and the firmware then decodes at a
    // geometry the stream never asked for: the rest of the segment is consumed
    // and never produces a picture. So the format is set only when the driver's
    // answer is not the one the client needs (a P010 surface against an NV12
    // default, an Import layout). G_FMT first, exactly as the Capture Setup
    // sequence in dev-decoder.rst prescribes.
    const bool ubwc = config_.capture_modifier == DRM_FORMAT_MOD_QCOM_COMPRESSED;
    const uint32_t pixelformat = ubwc ? V4L2_PIX_FMT_QC08C
                                      : (config_.capture_fourcc == VA_FOURCC_P010 ? V4L2_PIX_FMT_P010
                                                                                  : V4L2_PIX_FMT_NV12);
    FormatInfo current = device_->get_format(Queue::Capture);
    FormatInfo granted = current;
    if (current.pixelformat != pixelformat) {
        current.pixelformat = pixelformat;
        granted = device_->set_format(Queue::Capture, current);
    }
    if (granted.pixelformat != pixelformat || granted.num_planes != 1) {
        trace_("capture format rejected wanted=%c%c%c%c granted=%c%c%c%c planes=%u %ux%u", pixelformat & 0xff,
            (pixelformat >> 8) & 0xff, (pixelformat >> 16) & 0xff, (pixelformat >> 24) & 0xff,
            granted.pixelformat & 0xff, (granted.pixelformat >> 8) & 0xff, (granted.pixelformat >> 16) & 0xff,
            (granted.pixelformat >> 24) & 0xff, granted.num_planes, granted.width, granted.height);
        throw Error(VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT, "decoder CAPTURE layout unsupported");
    }

    // The visible rectangle. The kernel reports the firmware's crop through
    // every selection target; when it is empty (this Iris driver returns a
    // zero rectangle before the first frame) fall back to what the client
    // declared, bounded by the coded size. The client's geometry is the SPS
    // crop as far as it knows, so it is the right default.
    if (auto compose = device_->get_compose(Queue::Capture); compose && compose->width && compose->height
        && compose->width <= granted.width && compose->height <= granted.height)
        visible_ = *compose;
    else
        visible_ = Selection { 0, 0, std::min(config_.width, granted.width), std::min(config_.height, granted.height) };
    config_.width = visible_.width;
    config_.height = visible_.height;

    Layout layout;
    layout.fourcc = config_.capture_fourcc;
    layout.width = granted.width;
    layout.height = granted.height;
    layout.data_offset = 0;
    layout.size = granted.sizeimage;
    if (ubwc) {
        // The kernel's Q08C allocation: four 4 KiB-rounded planes. The
        // kernel's sizeimage is the one the pool must hold; a disagreement
        // means the two layouts were derived differently.
        const PlaneLayout ubwc = iris::ubwc_layout(granted.width, granted.height);
        require(ubwc.size == granted.sizeimage, "the UBWC layout disagrees with the node's sizeimage",
            VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        layout.modifier = ubwc.modifier;
        layout.plane_count = ubwc.plane_count;
        for (unsigned i = 0; i < ubwc.plane_count; ++i)
            layout.planes[i] = { ubwc.planes[i].offset, ubwc.planes[i].pitch, ubwc.planes[i].rows };
    } else {
        layout.stride = granted.bytesperline;
        // iris_buffer.c: luma scanlines aligned to 32, chroma directly after.
        layout.storage_height = (granted.height + 31) & ~31u;
    }

    // Import memory when the first picture asks for it and the contract
    // holds (docs/architecture.md §3.3): decode-order output, and the
    // client's buffer laid out exactly as the firmware will write it.
    CaptureMemory memory = CaptureMemory::Mmap;
    if (!pending_.empty() && pending_.begin()->second->publish == Publish::Import) {
        const Target& first = *pending_.begin()->second;
        const char* why = nullptr;
        if (!config_.import_allowed)
            why = "codec not qualified in the capability table";
        else if (mode_ != SessionMode::DecodeOrder)
            why = "display-order output";
        else if (!first.destination)
            why = "no client buffer";
        else if (!import_compatible(first.destination->layout(), layout))
            why = "client layout differs from the CAPTURE layout";
        if (why) {
            import_rejected_ = true;
            trace_("import rejected: %s (client stride=%u chroma=%u size=%u; capture stride=%u chroma=%u size=%u)", why,
                first.destination ? first.destination->layout().stride : 0,
                first.destination ? first.destination->layout().chroma_offset() : 0,
                first.destination ? first.destination->size() : 0, layout.stride, layout.chroma_offset(),
                layout.size);
        } else {
            memory = CaptureMemory::Import;
        }
    }

    // Pool sizing: the client's surface count plus in-flight headroom, never
    // below the firmware's minimum, never below 32 (the old vb2 ceiling that
    // every hardware pass ran with), capped by the kernel's maximum. At large
    // geometries the memory budget wins over the fixed count: 32 8K frames
    // exceed the device's contiguous carve-out and STREAMON fails outright.
    const auto minimum = device_->get_control(V4L2_CID_MIN_BUFFERS_FOR_CAPTURE);
    unsigned wanted = std::clamp(config_.surface_count + 4, kMinCapturePool, kMaxCapturePool);
    if (minimum && *minimum > 0)
        wanted = std::max(wanted, static_cast<unsigned>(*minimum));
    const uint64_t budget = device_->capture_from_system() ? kSystemCaptureMemoryBudget : kCaptureMemoryBudget;
    wanted = capture_pool_count(wanted, layout.size, minimum ? *minimum : 0, budget);
    direct_min_pool_ = (minimum && *minimum > 0 ? static_cast<unsigned>(*minimum) : 0) + kDirectHeadroom;
    pool_.allocate_capture(wanted, layout, memory);
    trace_("capture configure %ux%u stride=%u size=%u visible=%ux%u pool=%u memory=%s firmware-min=%d",
        granted.width, granted.height, granted.bytesperline, granted.sizeimage, visible_.width, visible_.height,
        pool_.capture_count(), memory == CaptureMemory::Import ? "import" : "mmap", minimum ? *minimum : -1);
    enter_streaming();
}

void Session::enter_streaming()
{
    // CAPTURE STREAMON has to precede every CAPTURE QBUF on this path, both
    // memories alike. The kernel keeps a DRC boundary sub-state
    // (DRC | DRC_LAST) after the firmware PSC_LAST and clears it inside
    // iris_process_streamon_output. A CAPTURE QBUF that reaches
    // iris_vb2_buf_queue while those bits are still set is not handed to the
    // firmware: the driver completes it on the spot as an empty
    // V4L2_BUF_FLAG_LAST buffer, which makes vb2 set q->last_buffer_dequeued
    // and turns every later CAPTURE DQBUF into -EPIPE. Queueing before
    // STREAMON therefore loses the whole new segment; queueing after it does
    // not. v4l2av1dec survives the same window only by luck of timing.
    device_->stream(Queue::Capture, true);
    if (pool_.capture_memory() == CaptureMemory::Import) {
        // `lent` counts slots in the pool that exists now, and this is a new
        // one: after a resolution change the slots an old entry was queued
        // into are gone, so its count would otherwise say "already lent" for
        // buffers the firmware never got.
        for (ImportEntry& entry : import_backlog_)
            entry.lent = 0;
        // Pictures already submitted (the first AU, or the one that
        // announced a resolution change) wait in the backlog; the first
        // gets its buffers now, the rest follow one picture at a time.
        for (const auto& [token, target] : pending_) {
            const bool queued = std::any_of(import_backlog_.begin(), import_backlog_.end(),
                [&](const ImportEntry& e) { return e.token == token; });
            if (!queued && !target->detached && target->destination)
                import_backlog_.push_back({ token, fcntl(target->destination->fd(), F_DUPFD_CLOEXEC, 0),
                    target->completions, 0, target->destination->layout() });
        }
        queue_import_next();
    } else {
        pool_.requeue_capture();
    }
    source_change_ = false;
    saw_last_ = false;
    state_ = SessionState::Streaming;
}

// ---------------------------------------------------------------------------
// Drain, restart, resolution change, finish

void Session::drain(const char* reason, bool wait_output)
{
    require(state_ == SessionState::Streaming, "drain outside Streaming");
    ++stats_.drains;
    trace_("drain begin reason=%s pending=%zu", reason, pending_.size());
    state_ = SessionState::Draining;
    saw_last_ = false;
    require(device_->decoder_command(V4L2_DEC_CMD_STOP), "DECODER_CMD_STOP busy", VA_STATUS_ERROR_HW_BUSY);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLastWaitMs);
    while (!saw_last_) {
        if (expired(deadline)) {
            fail("no LAST after STOP");
            throw Error(VA_STATUS_ERROR_DECODING_ERROR, "decoder did not drain");
        }
        if (pool_.capture_memory() == CaptureMemory::Import) {
            // Pictures still owed get their buffers one at a time. LAST
            // arrives on a buffer of its own, so whenever the firmware has
            // nothing to write into (backlog empty, or its head is the
            // picture of a new geometry that waits for the rebuilt pool)
            // lend it a scratch one.
            queue_import_next();
            if (pool_.capture_queued() == 0)
                pool_.queue_capture_scratch();
        } else {
            pool_.requeue_capture();
        }
        device_->poll(POLLIN | POLLPRI, 5);
        handle_events();
        handle_output_returns();
        handle_capture_returns();
    }
    // Iris recycles the final OUTPUT a few ticks after LAST; START with a
    // pre-STOP OUTPUT still queued returns EBUSY.
    if (wait_output) {
        const auto output_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kOutputDrainMs);
        while (pool_.output_queued() > 0 && !expired(output_deadline)) {
            device_->poll(POLLOUT, 5);
            handle_output_returns();
        }
    }
    fail_unrecoverable_tokens();
    state_ = SessionState::RestartPending;
    ++progress_;
    trace_("drain complete last=1 output_queued=%u", pool_.output_queued());
}

void Session::restart()
{
    require(state_ == SessionState::RestartPending, "restart outside RestartPending");
    pool_.requeue_capture();
    // EBUSY here means an OUTPUT is still owned by the firmware; give it a
    // short grace instead of declaring the instance dead.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kOutputDrainMs);
    while (!device_->decoder_command(V4L2_DEC_CMD_START)) {
        if (expired(deadline)) {
            fail("DECODER_CMD_START stayed busy");
            throw Error(VA_STATUS_ERROR_HW_BUSY, "decoder restart busy");
        }
        device_->poll(POLLOUT, 5);
        handle_output_returns();
    }
    saw_last_ = false;
    state_ = SessionState::Streaming;
    ++progress_;
    trace_("restart");
}

void Session::reconfigure()
{
    // Mid-stream resolution change. The kernel contract: SOURCE_CHANGE is an
    // implicit drain (dev-decoder.rst), so the last picture from before the
    // change comes back with LAST and the decoder is already stopped. Waiting
    // for that LAST is this side's whole drain; a DECODER_CMD_STOP on top of
    // it starts a second drain the firmware has nothing left to finish, and
    // no second LAST ever arrives. Then STREAMOFF CAPTURE, REQBUFS(0), re-read
    // the format and rebuild. OUTPUT keeps streaming. Held frames from the
    // old geometry stay valid for their holders (they own the fd); the pool
    // must be empty of holds before REQBUFS(0), so callers are expected to
    // have released targets they no longer display. If one is still held, the
    // change waits.
    ++stats_.reconfigures;
    trace_("session reconfigure %ux%u ->", config_.width, config_.height);
    await_drc_last();
    fail_unrecoverable_tokens();
    state_ = SessionState::Reconfiguring;
    device_->stream(Queue::Capture, false);
    pool_.mark_all_capture_returned();
    // The copy engine may cache imports of the old slots; those imports
    // hold DMA-BUF references that would block REQBUFS(0).
    if (copier_)
        for (unsigned i = 0; i < pool_.capture_count(); ++i)
            copier_->forget(pool_.take_capture(i)->id());
    continue_reconfigure();
}

void Session::await_drc_last()
{
    // The kernel marks the boundary on a CAPTURE buffer, which has to be
    // queued for it to come back; the marker may carry no pixels and is not
    // attributed to any picture.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLastWaitMs);
    ++stats_.drains;
    trace_("drc drain begin pending=%zu", pending_.size());
    while (!saw_last_) {
        if (expired(deadline)) {
            fail("no LAST after SOURCE_CHANGE");
            throw Error(VA_STATUS_ERROR_DECODING_ERROR, "decoder did not drain");
        }
        if (pool_.capture_memory() == CaptureMemory::Import) {
            // An import pool only queues buffers the client exported, one at
            // a time and only for pictures it still owes. The boundary marker
            // is not a picture; lend it a scratch buffer when nothing is
            // queued, exactly as the explicit drain does.
            queue_import_next();
            if (pool_.capture_queued() == 0)
                pool_.queue_capture_scratch();
        } else {
            pool_.requeue_capture();
        }
        device_->poll(POLLIN | POLLPRI, 5);
        handle_output_returns();
        handle_capture_returns();
    }
    trace_("drc drain complete last=1");
}

void Session::continue_reconfigure()
{
    require(state_ == SessionState::Reconfiguring, "continue_reconfigure outside Reconfiguring");
    // A Direct client may still hold frames of the old geometry for display.
    // It must not be asked to release them first: the usual cause of the
    // change is that client's own submit() of the new-resolution picture, so
    // waiting would wait on the call that is waiting here. Each held slot
    // owns the DMA-BUF it came from and vb2 keeps the memory alive after
    // REQBUFS(0) (orphaning), so the pool is dropped under the holders and
    // their frames keep reading back (verified on the target, see
    // docs/architecture.md §4).
    pool_.release_capture();
    // Per the V4L2 stateful decoder contract, CAPTURE STREAMON after the
    // drain resumes decoding at the new geometry; no START is issued and
    // OUTPUT keeps streaming with the new-resolution access unit queued.
    source_change_ = true;
    state_ = SessionState::Configured;
    configure_capture();
    ++progress_;
    trace_("session reconfigure -> %ux%u", config_.width, config_.height);
}

void Session::fail_unrecoverable_tokens()
{
    // After a drain the firmware has returned every picture it decoded. A
    // token whose OUTPUT slot is still queued was never consumed (the
    // new-resolution access unit, or input behind a STOP) and decodes after
    // the restart; every other pending token is lost.
    for (auto it = pending_.begin(); it != pending_.end();) {
        bool still_queued = false;
        for (const auto& slot : pool_.outputs())
            if (slot.queued && slot.token == it->first) {
                still_queued = true;
                break;
            }
        if (still_queued) {
            ++it;
            continue;
        }
        it->second->pending = false;
        it->second->error = true;
        ++stats_.errors;
        trace_("drain lost token=%llu", static_cast<unsigned long long>(it->first));
        retained_.erase(it->first);
        it = pending_.erase(it);
    }
}

void Session::finish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == SessionState::Failed || state_ == SessionState::Finished)
        return;
    if (state_ == SessionState::Configured) {
        // Nothing ever completed; no CAPTURE queue to drain.
        state_ = SessionState::Finished;
        return;
    }
    try {
        if (state_ == SessionState::Streaming)
            drain("finish", false);
    } catch (const Error&) {
        // fail() already ran.
    }
    // An entry the drain never resolved still holds a duplicate of the
    // client's fd. Nothing retires it after this point, so close it here.
    clear_import_backlog();
    state_ = state_ == SessionState::Failed ? SessionState::Failed : SessionState::Finished;
    trace_("session finish submitted=%llu completed=%llu errors=%llu drops=%llu timeouts=%llu drains=%llu "
           "misplaced=%llu retained=%llu",
        static_cast<unsigned long long>(stats_.submitted), static_cast<unsigned long long>(stats_.completed),
        static_cast<unsigned long long>(stats_.errors), static_cast<unsigned long long>(stats_.drops),
        static_cast<unsigned long long>(stats_.timeouts), static_cast<unsigned long long>(stats_.drains),
        static_cast<unsigned long long>(stats_.misplaced), static_cast<unsigned long long>(retained_total_));
}

void Session::fail(const char* what)
{
    if (state_ == SessionState::Failed)
        return;
    state_ = SessionState::Failed;
    trace_("session failed: %s", what);
    fail_all_pending();
}

void Session::fail_all_pending()
{
    for (auto& [token, target] : pending_) {
        target->pending = false;
        target->error = true;
    }
    pending_.clear();
    retained_.clear();
    clear_import_backlog();
}

} // namespace iris
