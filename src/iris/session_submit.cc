// SPDX-License-Identifier: MIT
//
// Submission and target bookkeeping: taking an access unit into the OUTPUT
// queue, handing client buffers to the firmware one picture at a time, and
// invalidating a target when its client moves on.

#include "session.h"

#include "session_internal.h"
#include "../util/error.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

namespace iris {

// ---------------------------------------------------------------------------
// Submission

void Session::submit(const uint8_t* data, size_t size, const TargetPtr& target)
{
    std::lock_guard<std::mutex> lock(mutex_);
    require(state_ != SessionState::Failed, "decoder session failed", VA_STATUS_ERROR_DECODING_ERROR);
    require(state_ != SessionState::Finished, "decoder session finished", VA_STATUS_ERROR_OPERATION_FAILED);
    require(size > 0 && size <= config_.au_capacity, "access unit does not fit the OUTPUT slot",
        VA_STATUS_ERROR_NOT_ENOUGH_BUFFER);
    require(target->completions >= 1, "a picture produces at least one completion",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    // A target that still owns a token from an earlier picture is being
    // reused without release(); treat it as released.
    if (target->pending)
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

    if (state_ == SessionState::Configured && target->fourcc != 0)
        config_.capture_fourcc = target->fourcc;
    std::memcpy(slot->mapping, data, size);
    if (dump_) {
        std::fwrite(data, 1, size, static_cast<FILE*>(dump_));
        std::fflush(static_cast<FILE*>(dump_));
    }
    const uint64_t token = next_token_++;
    device_->queue_buffer(Queue::Output, slot->index, static_cast<unsigned>(size), token, 0);
    slot->queued = true;
    slot->token = token;
    target->token = token;
    target->pending = true;
    target->completed = false;
    target->error = false;
    target->invisible = 0;
    target->frame.reset();
    pending_[token] = target;
    ++stats_.submitted;
    if (pool_.capture_memory() == CaptureMemory::Import) {
        // The bitstream may run ahead; the client's buffer goes in only
        // when the firmware has nothing else to write into. The fd is
        // duplicated so the entry survives a released (or destroyed)
        // surface: its picture still needs a buffer.
        const int fd = target->destination ? fcntl(target->destination->fd(), F_DUPFD_CLOEXEC, 0) : -1;
        import_backlog_.push_back(
            { token, fd, target->completions, 0, target->destination ? target->destination->layout() : Layout {} });
        queue_import_next();
    }
    trace_("submit token=%llu bytes=%zu output=%u", static_cast<unsigned long long>(token), size, slot->index);

    if (state_ == SessionState::Configured) {
        // The first AU raises SOURCE_CHANGE; CAPTURE cannot be configured
        // before that. Bring-up may take firmware load time.
        const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(detail::kSourceChangeWaitMs);
        while (state_ == SessionState::Configured) {
            require(!expired(start_deadline), "no SOURCE_CHANGE after the first access unit",
                VA_STATUS_ERROR_TIMEDOUT);
            pump(20);
        }
    }
}

// ---------------------------------------------------------------------------
// Import window

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
    TargetPtr target = it == pending_.end() ? nullptr : it->second;
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
        wait_dma_buf(entry.fd, true, detail::kImportFenceMs);
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
// Tokens and release

void Session::forget(std::map<uint64_t, TargetPtr>::iterator it)
{
    retained_.erase(it->first);
    pending_.erase(it);
}

void Session::forget_token(uint64_t token)
{
    if (auto it = pending_.find(token); it != pending_.end())
        forget(it);
}

void Session::release_locked(const TargetPtr& target)
{
    if (target->pending) {
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
        if (pool_.capture_memory() == CaptureMemory::Import && target->publish == Publish::Import) {
            // A copy, not the surface's own target: the surface may be
            // reused or destroyed while this picture is still with the
            // firmware, and the completion must not touch it.
            auto kept = std::make_shared<Target>(*target);
            kept->detached = true;
            // The buffer outlives the surface only as this entry's duplicate
            // fd. Null the pointer so no later path can read a freed
            // StableBuffer; every Import decision uses the entry's copy of
            // the layout instead.
            kept->destination = nullptr;
            kept->pending = true;
            retained_[kept->token] = kept;
            pending_[kept->token] = kept;
            ++retained_total_;
            trace_("target detached token=%llu", static_cast<unsigned long long>(kept->token));
        } else {
            forget_token(target->token);
        }
        target->pending = false;
    }
    target->frame.reset();
    target->completed = false;
    target->error = false;
    target->token = 0;
}

void Session::release(const TargetPtr& target)
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

} // namespace iris
