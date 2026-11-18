// SPDX-License-Identifier: MIT
//
// Completing a picture: servicing the two queues, matching a returned buffer
// to the target that owns it, publishing it through that target's policy,
// and waiting for one.

#include "session.h"

#include "../util/error.h"

#include <algorithm>
#include <cstdio>

extern "C" {
#include <linux/v4l2-controls.h>
#include <poll.h>
}

namespace iris {

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

// ---------------------------------------------------------------------------
// Completion

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
                TargetPtr target = hidden->second;
                forget(hidden);
                frame->layout().data_offset = returned->data_offset;
                // A hidden picture carries no output plane in the firmware's
                // accounting (bytes=0) but it did decode it, and the stream
                // displays it later through show_existing_frame; publish
                // what it decoded all the same.
                publish(*target, frame, *returned, "capture noshow");
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
            TargetPtr target = it->second;
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
            if (noshow_shape && (target->completions - target->invisible) > 1) {
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
                ++target->invisible;
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
                target->pending = false;
                target->error = true;
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
            target->pending = false;
            if (noshow_shape && (target->hidden || target->invisible > 0)) {
                // A hidden client picture completes with no pixels. The
                // second NOSHOW half of an install unit is that picture even
                // when its own show_frame=0 made the target hidden; the
                // counter above already consumed the internal install half.
                // Publishing without pixels is what a hidden surface needs.
                frame->layout().data_offset = returned->data_offset;
                publish(*target, frame, *returned, "capture noshow");
                cold_ = false;
                ++stats_.completed;
                ++progress_;
                continue;
            }
            target->error = true;
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
        TargetPtr target = it->second;
        forget(it);
        if (pool_.capture_memory() == CaptureMemory::Import && slot_token != returned->timestamp_us) {
            // The firmware wrote this picture into a buffer queued for a
            // different token. Neither surface can be trusted: the one this
            // token belongs to got nothing, the one the buffer belongs to
            // got foreign pixels. Fail both; never publish either.
            target->pending = false;
            target->error = true;
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
        publish(*target, frame, *returned);
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
        // fill it the Copy way. supports_import() tells the caller. UBWC has
        // no linear copy: the slot's bytes are compressed and the client's
        // buffer is a different allocation, so degrading would hand out
        // garbage. A layout mismatch is the session's only way here.
        if (frame->layout().compressed()) {
            trace_("%s token=%llu index=%u flags=%#x publish=ubwc-copy-refused", event ? event : "capture",
                static_cast<unsigned long long>(completion.timestamp_us), completion.index, completion.flags);
            target.pending = false;
            target.error = true;
            ++stats_.errors;
            throw Error(VA_STATUS_ERROR_UNIMPLEMENTED, "a UBWC picture cannot fall back to Copy");
        }
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

void Session::wait(const TargetPtr& target)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!target->pending) {
        require(!target->error, "target decode failed", VA_STATUS_ERROR_DECODING_ERROR);
        return;
    }
    require(state_ != SessionState::Failed, "decoder session failed", VA_STATUS_ERROR_DECODING_ERROR);
    pump(0);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(sync_timeout_ms());
    uint64_t seen_progress = progress_;
    bool drained = false;
    while (target->pending) {
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
                    static_cast<unsigned long long>(target->token), pending_.size(), pool_.capture_queued(),
                    pool_.capture_held());
                drain("timeout", true);
                restart();
                drained = true;
                deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.sync_timeout_ms);
                continue;
            }
            trace_("sync timeout token=%llu pending=%zu queued=%u held=%u action=fail",
                static_cast<unsigned long long>(target->token), pending_.size(), pool_.capture_queued(),
                pool_.capture_held());
            // Invalidate the token so a late completion is dropped rather
            // than written into a surface the client has moved on from.
            forget_token(target->token);
            target->pending = false;
            target->error = true;
            throw Error(VA_STATUS_ERROR_TIMEDOUT, "surface sync timeout");
        }
        lock.unlock();
        device_->poll(POLLIN | POLLPRI, 10);
        lock.lock();
        pump(0);
    }
    require(!target->error, "target decode failed", VA_STATUS_ERROR_DECODING_ERROR);
}

} // namespace iris
