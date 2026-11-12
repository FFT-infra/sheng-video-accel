// SPDX-License-Identifier: MIT

#include "session.h"

#include "../util/error.h"

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
constexpr int kSourceChangeWaitMs = 5000;
// Bound on the client buffer's implicit fence before it is handed to the
// firmware: the compositor may still be sampling the previous picture.
constexpr int kImportFenceMs = 2000;

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
    const uint32_t pixelformat = config_.capture_fourcc == VA_FOURCC_P010 ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
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
    layout.stride = granted.bytesperline;
    // iris_buffer.c: luma scanlines aligned to 32, chroma directly after.
    layout.storage_height = (granted.height + 31) & ~31u;
    layout.data_offset = 0;
    layout.size = granted.sizeimage;

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
    // every hardware pass ran with), capped by the kernel's maximum.
    const auto minimum = device_->get_control(V4L2_CID_MIN_BUFFERS_FOR_CAPTURE);
    unsigned wanted = std::clamp(config_.surface_count + 4, kMinCapturePool, kMaxCapturePool);
    if (minimum && *minimum > 0)
        wanted = std::max(wanted, static_cast<unsigned>(*minimum));
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

void Session::queue_import_next()
{
    if (pool_.capture_memory() != CaptureMemory::Import || import_backlog_.empty())
        return;
    ImportEntry& entry = import_backlog_.front();
    const unsigned queued_for_this = pool_.capture_queued_for(entry.token);
    // The window is one picture, not one buffer. A picture the firmware
    // decodes as several (a VP9 alt-ref) gets a buffer for each of its
    // completions, and only that picture's buffers are ever with it. Moving
    // on to the next picture before this one is answered is what gave the
    // firmware a choice, and a choice is what the misplaced count measures.
    if (queued_for_this == 0 && entry.lent >= entry.completions) {
        // Every buffer this picture needed has been lent and has come back:
        // it is answered, or it was abandoned and its completion dropped.
        const int fd = entry.fd;
        import_backlog_.pop_front();
        if (fd >= 0)
            close(fd);
        queue_import_next();
        return;
    }
    if (entry.lent >= entry.completions)
        return; // fully lent; the firmware still holds one of them
    auto it = pending_.find(entry.token);
    Target* target = it == pending_.end() ? nullptr : it->second;
    // A retained target has no destination to check: its surface, and with
    // it the buffer, is gone, and the entry holds the duplicate of that fd.
    if (target && !target->detached && (target->publish == Publish::Direct || !target->destination)) {
        // The client never gave this picture a buffer; nothing to decode
        // into. (An Import session requires every surface exported.) This
        // check has to come before the fd test below: a Direct target is
        // exactly the one whose fd is -1, and popping the entry silently
        // left its target pending forever.
        const int fd = entry.fd;
        import_backlog_.pop_front();
        if (fd >= 0)
            close(fd);
        forget(it);
        target->pending = false;
        target->error = true;
        ++stats_.errors;
        trace_("import token=%llu failed: no client buffer", static_cast<unsigned long long>(entry.token));
        queue_import_next();
        return;
    }
    if (entry.fd < 0) {
        // No client buffer and no target left to fail: the surface was
        // released or destroyed. Nothing to queue, now or later.
        import_backlog_.pop_front();
        queue_import_next();
        return;
    }
    if (target && !import_compatible(entry.client_layout, pool_.capture_layout()))
        // The first picture of a resolution change: its buffer fits the
        // geometry the firmware is about to announce, not the current one.
        // Leave it at the head; enter_streaming() queues it after the pool
        // is rebuilt. If no change follows, the picture completes into
        // whatever is queued next and is reported misplaced.
        return;
    // The compositor may still be sampling the previous picture from this
    // buffer; the firmware must not write under it. The fence is waited for
    // once per picture: every half of a superframe lands in the same client
    // buffer, so the wait is on the buffer, not on the completion.
    if (entry.lent == 0) {
        const auto fence_start = std::chrono::steady_clock::now();
        wait_dma_buf(entry.fd, true, kImportFenceMs);
        const auto fence_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - fence_start)
                                  .count();
        if (fence_ms >= 20)
            trace_("import fence token=%llu fd=%d waited=%lldms", static_cast<unsigned long long>(entry.token),
                entry.fd, static_cast<long long>(fence_ms));
    }
    // Every completion of this picture gets a slot now, not one per
    // returned completion: the firmware does not hand the first half back
    // before it has somewhere to put the second, and waiting for a return
    // that needs a buffer is the stall this replaces. The slots all carry
    // the same client buffer, so the firmware still has no choice between
    // different pictures' memory — which is what the window rule protects.
    while (entry.lent < entry.completions) {
        const auto slot = pool_.queue_capture_import(entry.fd, pool_.capture_layout().size, entry.token);
        require(slot.has_value(), "no free CAPTURE slot with nothing in flight");
        ++entry.lent;
        trace_("import token=%llu index=%u fd=%d backlog=%zu abandoned=%d lent=%u/%u",
            static_cast<unsigned long long>(entry.token), *slot, entry.fd, import_backlog_.size(),
            target == nullptr ? 1 : 0, entry.lent, entry.completions);
    }
}

void Session::clear_import_backlog()
{
    for (const ImportEntry& entry : import_backlog_)
        if (entry.fd >= 0)
            close(entry.fd);
    import_backlog_.clear();
}

// ---------------------------------------------------------------------------
// Submission

void Session::submit(const uint8_t* data, size_t size, Target& target)
{
    std::lock_guard<std::mutex> lock(mutex_);
    require(state_ != SessionState::Failed, "decoder session failed", VA_STATUS_ERROR_DECODING_ERROR);
    require(state_ != SessionState::Finished, "decoder session finished", VA_STATUS_ERROR_OPERATION_FAILED);
    require(size > 0 && size <= config_.au_capacity, "access unit does not fit the OUTPUT slot",
        VA_STATUS_ERROR_NOT_ENOUGH_BUFFER);
    require(target.completions >= 1, "a picture produces at least one completion",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    // A target that still owns a token from an earlier picture is being
    // reused without release(); treat it as released.
    if (target.pending)
        release_locked(target);

    pump(0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(sync_timeout_ms());
    // A resolution change is waiting for the client to release frames it
    // still holds from the old geometry. Nothing can be accepted until the
    // CAPTURE pool is rebuilt; the client must release (or sync and drop)
    // before it submits more.
    while (state_ == SessionState::Reconfiguring) {
        require(!expired(deadline), "resolution change blocked by held CAPTURE frames; release them first",
            VA_STATUS_ERROR_SURFACE_BUSY);
        pump(20);
    }
    // In display-order mode the firmware may hold every OUTPUT slot behind a
    // reorder window; the drain in wait() is what frees them. Here we can
    // only wait for the normal return of a slot.
    OutputSlot* slot = nullptr;
    while (!(slot = pool_.free_output())) {
        require(state_ == SessionState::Streaming || state_ == SessionState::Configured,
            "decoder not accepting input while draining", VA_STATUS_ERROR_HW_BUSY);
        require(!expired(deadline), "OUTPUT queue full", VA_STATUS_ERROR_TIMEDOUT);
        pump(20);
    }

    if (state_ == SessionState::Configured && target.fourcc != 0)
        config_.capture_fourcc = target.fourcc;
    std::memcpy(slot->mapping, data, size);
    if (dump_) {
        std::fwrite(data, 1, size, static_cast<FILE*>(dump_));
        std::fflush(static_cast<FILE*>(dump_));
    }
    const uint64_t token = next_token_++;
    device_->queue_buffer(Queue::Output, slot->index, static_cast<unsigned>(size), token, 0);
    slot->queued = true;
    slot->token = token;
    target.token = token;
    target.pending = true;
    target.completed = false;
    target.error = false;
    target.invisible = 0;
    target.frame.reset();
    pending_[token] = &target;
    ++stats_.submitted;
    if (pool_.capture_memory() == CaptureMemory::Import) {
        // The bitstream may run ahead; the client's buffer goes in only
        // when the firmware has nothing else to write into. The fd is
        // duplicated so the entry survives a released (or destroyed)
        // surface: its picture still needs a buffer.
        const int fd = target.destination ? fcntl(target.destination->fd(), F_DUPFD_CLOEXEC, 0) : -1;
        import_backlog_.push_back(
            { token, fd, target.completions, 0, target.destination ? target.destination->layout() : Layout {} });
        queue_import_next();
    }
    trace_("submit token=%llu bytes=%zu output=%u", static_cast<unsigned long long>(token), size, slot->index);

    if (state_ == SessionState::Configured) {
        // The first AU raises SOURCE_CHANGE; CAPTURE cannot be configured
        // before that. Bring-up may take firmware load time.
        const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSourceChangeWaitMs);
        while (state_ == SessionState::Configured) {
            require(!expired(start_deadline), "no SOURCE_CHANGE after the first access unit",
                VA_STATUS_ERROR_TIMEDOUT);
            pump(20);
        }
    }
}

// ---------------------------------------------------------------------------
// Servicing

void Session::service()
{
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || state_ == SessionState::Failed)
        return;
    try {
        pump(0);
    } catch (const Error&) {
        // fail() already recorded it; the next submit/wait reports.
    }
}

void Session::pump(int timeout_ms)
{
    if (state_ == SessionState::Failed)
        return;
    pool_.requeue_capture();
    if (timeout_ms > 0) {
        // Some Iris kernels do not wake POLLIN for a completed CAPTURE
        // buffer while OUTPUT has writable space; the wait is a bounded
        // sleep and the non-blocking DQBUF below is authoritative.
        device_->poll(POLLIN | POLLPRI | POLLOUT, std::min(timeout_ms, 5));
    }
    try {
        handle_events();
        if (source_change_ && state_ == SessionState::Configured)
            configure_capture();
        handle_output_returns();
        if (state_ == SessionState::Streaming || state_ == SessionState::Draining)
            handle_capture_returns();
        if (source_change_ && state_ == SessionState::Streaming)
            reconfigure();
        else if (state_ == SessionState::Reconfiguring)
            continue_reconfigure();
        if (state_ != SessionState::Reconfiguring)
            pool_.requeue_capture();
    } catch (const Error& error) {
        fail(error.what());
        throw;
    }
}

void Session::handle_events()
{
    for (;;) {
        const Event event = device_->dequeue_event();
        if (event == Event::None)
            return;
        if (event == Event::SourceChange) {
            source_change_ = true;
            trace_("event source-change state=%d", static_cast<int>(state_));
        }
    }
}

void Session::handle_output_returns()
{
    while (auto returned = device_->dequeue_buffer(Queue::Output)) {
        OutputSlot& slot = pool_.output(returned->index);
        slot.queued = false;
        if (returned->error()) {
            // The firmware rejected the bitstream in this slot. The
            // corresponding CAPTURE may never come; fail the target now.
            auto it = pending_.find(slot.token);
            if (it != pending_.end()) {
                Target& target = *it->second;
                target.pending = false;
                target.error = true;
                forget(it);
                ++stats_.errors;
                trace_("output error token=%llu output=%u", static_cast<unsigned long long>(slot.token),
                    slot.index);
            }
        }
        slot.token = 0;
    }
}

bool Session::handle_capture_returns()
{
    bool last = false;
    while (auto returned = device_->dequeue_buffer(Queue::Capture)) {
        FrameRef frame = pool_.take_capture(returned->index);
        const uint64_t slot_token = pool_.take_import_token(returned->index);
        const bool payload = returned->bytesused > returned->data_offset;
        if (state_ == SessionState::Streaming)
            queue_import_next();
        if (returned->last()) {
            last = true;
            saw_last_ = true;
        }
        constexpr uint32_t picture
            = V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME;
        const bool noshow_shape = !(returned->flags & picture) && !payload;
        if (returned->error() && returned->timestamp_us == 0) {
            // A picture the firmware decoded but does not show (an AV1
            // alt-ref) comes back with V4L2_BUF_FLAG_ERROR, no picture-type
            // bit and no timestamp. It is not a failure: the frame is
            // decoded, and the stream displays it later with
            // show_existing_frame, which VA never routes here, so the
            // client still downloads this surface. The CAPTURE returns
            // follow decode order, so it belongs to the oldest target
            // submitted as hidden. Anything else is a real failure marker
            // with no token it could be trusted against.
            if (noshow_shape && returned->last()) {
                // A resolution-change boundary can use an unnamed NOSHOW
                // slot as its empty LAST marker. It belongs to no picture.
                trace_("capture index=%u flags=%#x publish=invisible", returned->index, returned->flags);
                continue;
            }
            auto hidden = pending_.end();
            if (noshow_shape) {
                for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                    if (it->second->hidden
                        && (it->second->completions - it->second->invisible) == 1) {
                        hidden = it;
                        break;
                    }
                }
            }
            if (hidden != pending_.end()) {
                Target& target = *hidden->second;
                forget(hidden);
                frame->layout().data_offset = returned->data_offset;
                // A hidden picture carries no output plane in the firmware's
                // accounting (bytes=0) but it did decode it, and the stream
                // displays it later through show_existing_frame; publish
                // what it decoded all the same.
                publish(target, frame, *returned, "capture noshow");
                cold_ = false;
                ++stats_.completed;
                ++progress_;
                continue;
            }
            ++stats_.errors;
            trace_("capture error index=%u flags=%#x", returned->index, returned->flags);
            continue;
        }
        if (returned->error()) {
            // A kernel carrying packaging/kernel/0004-* keeps the firmware's
            // timestamp on an error completion, which names the picture. A
            // hidden picture still has no picture-type bit and no payload and
            // is published; anything else failed to decode and fails its own
            // target here, without waiting for a drain. This runs before the
            // payload check below because an error completion carries none.
            auto it = pending_.find(returned->timestamp_us);
            if (it == pending_.end()) {
                if (noshow_shape) {
                    // The invisible half of a two-completion unit whose target
                    // was released before it returned. It never carried pixels
                    // and was never a decode failure. A visible payload
                    // completion reports the one drop below; when both halves
                    // are NOSHOW, neither half has pixels to abandon, so
                    // drops stays 0 and the pair is two invisible records.
                    // Counting this half as a drop would charge the client
                    // twice for one picture and register a false decode error.
                    // Use publish=invisible for the trace parser: a normal
                    // two-completion unit has one such record, this fully
                    // abandoned NOSHOW pair has two.
                    trace_("capture token=%llu index=%u flags=%#x publish=invisible",
                        static_cast<unsigned long long>(returned->timestamp_us), returned->index,
                        returned->flags);
                } else {
                    ++stats_.errors;
                    trace_("capture error index=%u flags=%#x token=%llu publish=drop", returned->index,
                        returned->flags, static_cast<unsigned long long>(returned->timestamp_us));
                }
                continue;
            }
            Target& target = *it->second;
            if (noshow_shape && returned->last()) {
                // A resolution-change boundary marks the end of the old
                // segment with an empty LAST buffer. On this firmware the
                // borrowed slot can also carry ERROR and a NOSHOW shape; it
                // names no picture and must not fail the token the timestamp
                // happens to point at. The following restart decodes the
                // still-queued new-resolution access unit.
                trace_("capture token=%llu index=%u flags=%#x publish=invisible",
                    static_cast<unsigned long long>(returned->timestamp_us), returned->index, returned->flags);
                continue;
            }
            if (noshow_shape && (target.completions - target.invisible) > 1) {
                // A two-completion access unit starts with an invisible
                // install/alt-ref half: ERROR, no picture-type bit and no
                // payload, under the same token the client's picture will
                // carry. Count it and keep the target pending. The next
                // completion belongs to the client even when it is NOSHOW
                // too (a show_frame=0 shown half); without this counter the
                // shape alone makes both halves invisible and the client
                // waits forever. A picture the firmware really rejected
                // carries a picture-type bit (0x4049 for the truncated HEVC
                // in TEST-RESULTS) and falls through to the failure below.
                ++target.invisible;
                trace_("capture token=%llu index=%u flags=%#x publish=invisible",
                    static_cast<unsigned long long>(returned->timestamp_us), returned->index, returned->flags);
                continue;
            }
            // An error completion that landed in another picture's buffer is
            // the same misplacement the payload path refuses: the timestamp
            // names the picture the firmware was coding, the slot names the
            // buffer that was written. Failing only the timestamp's surface
            // left the buffer's owner pending and kept the misplaced count
            // at zero.
            if (pool_.capture_memory() == CaptureMemory::Import && slot_token != returned->timestamp_us) {
                target.pending = false;
                target.error = true;
                ++stats_.errors;
                ++stats_.misplaced;
                forget(it);
                if (auto owner = pending_.find(slot_token); owner != pending_.end()) {
                    owner->second->pending = false;
                    owner->second->error = true;
                    ++stats_.errors;
                    forget(owner);
                }
                trace_("capture error token=%llu index=%u flags=%#x publish=misplaced slot_token=%llu",
                    static_cast<unsigned long long>(returned->timestamp_us), returned->index, returned->flags,
                    static_cast<unsigned long long>(slot_token));
                continue;
            }
            forget(it);
            target.pending = false;
            if (noshow_shape && (target.hidden || target.invisible > 0)) {
                // A hidden client picture completes with no pixels. The
                // second NOSHOW half of an install unit is that picture even
                // when its own show_frame=0 made the target hidden; the
                // counter above already consumed the internal install half.
                // Publishing without pixels is what a hidden surface needs.
                frame->layout().data_offset = returned->data_offset;
                publish(target, frame, *returned, "capture noshow");
                cold_ = false;
                ++stats_.completed;
                ++progress_;
                continue;
            }
            target.error = true;
            ++stats_.errors;
            trace_("capture token=%llu index=%u flags=%#x publish=error",
                static_cast<unsigned long long>(returned->timestamp_us), returned->index, returned->flags);
            continue;
        }
        if (!payload) {
            trace_("capture empty index=%u flags=%#x last=%d", returned->index, returned->flags, last ? 1 : 0);
            if (last)
                trace_("capture last");
            continue;
        }
        auto it = pending_.find(returned->timestamp_us);
        if (it == pending_.end()) {
            // Released target, or a completion after a drain; never publish
            // pixels the caller did not ask for.
            ++stats_.drops;
            trace_("capture token=%llu index=%u flags=%#x publish=drop",
                static_cast<unsigned long long>(returned->timestamp_us), returned->index, returned->flags);
            continue;
        }
        Target& target = *it->second;
        forget(it);
        if (pool_.capture_memory() == CaptureMemory::Import && slot_token != returned->timestamp_us) {
            // The firmware wrote this picture into a buffer queued for a
            // different token. Neither surface can be trusted: the one this
            // token belongs to got nothing, the one the buffer belongs to
            // got foreign pixels. Fail both; never publish either.
            target.pending = false;
            target.error = true;
            ++stats_.errors;
            ++stats_.misplaced;
            if (auto owner = pending_.find(slot_token); owner != pending_.end()) {
                owner->second->pending = false;
                owner->second->error = true;
                ++stats_.errors;
                forget(owner);
            }
            trace_("capture token=%llu index=%u flags=%#x publish=misplaced slot_token=%llu",
                static_cast<unsigned long long>(returned->timestamp_us), returned->index, returned->flags,
                static_cast<unsigned long long>(slot_token));
            continue;
        }
        frame->layout().data_offset = returned->data_offset;
        publish(target, frame, *returned);
        cold_ = false;
        ++stats_.completed;
        ++progress_;
        if (last)
            trace_("capture last");
    }
    return last;
}

void Session::publish(Target& target, FrameRef frame, const Dequeued& completion, const char* event)
{
    const char* how = "direct";
    if (target.detached || pool_.capture_memory() == CaptureMemory::Import) {
        // The firmware wrote straight into the client's buffer; the slot
        // carried nothing of ours and is free once the ref drops.
        how = "import";
        target.publish = Publish::Import;
        target.frame.reset();
    } else if (target.publish == Publish::Import) {
        // Asked for Import, got an Mmap pool: the client buffer exists, so
        // fill it the Copy way. supports_import() tells the caller.
        target.publish = Publish::Copy;
    }
    if (target.publish == Publish::Copy) {
        require(target.destination != nullptr && copier_ != nullptr, "copy publish without a destination");
        try {
            copier_->copy(*target.destination, *frame);
        } catch (const Error&) {
            target.pending = false;
            target.error = true;
            ++stats_.errors;
            throw;
        }
        how = copier_->name();
        // The slot goes back to the firmware as soon as `frame` drops.
        target.frame.reset();
    } else if (target.publish == Publish::Direct) {
        target.frame = frame;
    }
    target.pending = false;
    target.completed = true;
    target.error = false;
    trace_("%s token=%llu index=%u flags=%#x publish=%s%s", event ? event : "capture",
        static_cast<unsigned long long>(completion.timestamp_us), completion.index, completion.flags,
        target.publish == Publish::Copy ? "copy-" : "", how);
}

// ---------------------------------------------------------------------------
// Waiting

void Session::wait(Target& target)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!target.pending) {
        require(!target.error, "target decode failed", VA_STATUS_ERROR_DECODING_ERROR);
        return;
    }
    require(state_ != SessionState::Failed, "decoder session failed", VA_STATUS_ERROR_DECODING_ERROR);
    pump(0);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(sync_timeout_ms());
    uint64_t seen_progress = progress_;
    bool drained = false;
    while (target.pending) {
        require(state_ != SessionState::Failed, "decoder session failed", VA_STATUS_ERROR_DECODING_ERROR);
        if (progress_ != seen_progress) {
            seen_progress = progress_;
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(sync_timeout_ms());
        }
        if (expired(deadline)) {
            ++stats_.timeouts;
            if (mode_ == SessionMode::DisplayOrder && !drained && state_ == SessionState::Streaming) {
                // Display-order firmware may be holding this picture behind
                // later input the client has not submitted yet. One drain
                // flushes the reorder window; after it, a still-pending
                // target is a real failure.
                trace_("sync timeout token=%llu pending=%zu queued=%u held=%u action=drain",
                    static_cast<unsigned long long>(target.token), pending_.size(), pool_.capture_queued(),
                    pool_.capture_held());
                drain("timeout", true);
                restart();
                drained = true;
                deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.sync_timeout_ms);
                continue;
            }
            trace_("sync timeout token=%llu pending=%zu queued=%u held=%u action=fail",
                static_cast<unsigned long long>(target.token), pending_.size(), pool_.capture_queued(),
                pool_.capture_held());
            // Invalidate the token so a late completion is dropped rather
            // than written into a surface the client has moved on from.
            forget_token(target.token);
            target.pending = false;
            target.error = true;
            throw Error(VA_STATUS_ERROR_TIMEDOUT, "surface sync timeout");
        }
        lock.unlock();
        device_->poll(POLLIN | POLLPRI, 10);
        lock.lock();
        pump(0);
    }
    require(!target.error, "target decode failed", VA_STATUS_ERROR_DECODING_ERROR);
}

// ---------------------------------------------------------------------------
// Release, drain, restart, reconfigure, finish

void Session::forget(std::map<uint64_t, Target*>::iterator it)
{
    retained_.erase(it->first);
    pending_.erase(it);
}

void Session::forget_token(uint64_t token)
{
    if (auto it = pending_.find(token); it != pending_.end())
        forget(it);
}

void Session::release_locked(Target& target)
{
    if (target.pending) {
        // The token leaves the map, but an Import backlog entry stays: the
        // picture is already in the OUTPUT queue and must be given a buffer
        // like every other picture, or every later completion lands in a
        // neighbour's buffer.
        //
        // Under Import the picture also already has the client's buffer in
        // the firmware's hands, so releasing the surface does not lose the
        // pixels: the client exported them and the backlog holds its own
        // duplicate of the fd. Keep a copy of the target, let the completion
        // publish normally (the Import path touches no destination), and the
        // ledger stays exact instead of calling a delivered picture a drop.
        // A Copy or Direct target cannot do that: its pixels land in the
        // surface's own buffer or frame, which is about to be destroyed.
        if (pool_.capture_memory() == CaptureMemory::Import && target.publish == Publish::Import) {
            Target& kept = retained_[target.token];
            kept = target;
            kept.detached = true;
            // The buffer outlives the surface only as this entry's duplicate
            // fd. Null the pointer so no later path can read a freed
            // StableBuffer; every Import decision uses the entry's copy of
            // the layout instead.
            kept.destination = nullptr;
            kept.pending = true;
            pending_[target.token] = &kept;
            ++retained_total_;
            trace_("target detached token=%llu", static_cast<unsigned long long>(target.token));
        } else {
            forget_token(target.token);
        }
        target.pending = false;
    }
    target.frame.reset();
    target.completed = false;
    target.error = false;
    target.token = 0;
}

void Session::release(Target& target)
{
    std::lock_guard<std::mutex> lock(mutex_);
    release_locked(target);
    if (state_ == SessionState::Reconfiguring) {
        try {
            continue_reconfigure();
        } catch (const Error&) {
            // fail() ran; the next submit reports it.
        }
    }
}

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
