// SPDX-License-Identifier: MIT

#include "fake_device.h"

#include "../../src/util/error.h"

#include <cstring>
#include <numeric>

namespace iris::test {

std::string FakeDevice::joined_log() const
{
    std::string out;
    for (const auto& line : log_) {
        out += line;
        out += '\n';
    }
    return out;
}

FormatInfo FakeDevice::get_format(Queue queue)
{
    note(std::string("G_FMT ") + (queue == Queue::Output ? "OUTPUT" : "CAPTURE"));
    return current_format(queue);
}

FormatInfo FakeDevice::current_format(Queue queue)
{
    FormatInfo info;
    if (queue == Queue::Output) {
        info.pixelformat = V4L2_PIX_FMT_H264;
        info.width = config_.coded_width;
        info.height = config_.coded_height;
        info.num_planes = 1;
        info.sizeimage = config_.au_capacity;
    } else {
        info.pixelformat = V4L2_PIX_FMT_NV12;
        info.width = config_.coded_width;
        info.height = config_.coded_height;
        info.num_planes = 1;
        info.bytesperline = config_.stride;
        info.sizeimage = config_.stride * config_.coded_height * 3 / 2;
    }
    return info;
}

FormatInfo FakeDevice::set_format(Queue queue, const FormatInfo& request)
{
    note(std::string("S_FMT ") + (queue == Queue::Output ? "OUTPUT" : "CAPTURE"));
    if (queue == Queue::Output) {
        require(!output_on_, "fake: S_FMT OUTPUT while streaming");
        output_format_set_ = true;
        FormatInfo granted = request;
        granted.num_planes = 1;
        if (granted.sizeimage == 0)
            granted.sizeimage = config_.au_capacity;
        config_.au_capacity = granted.sizeimage;
        return granted;
    }
    require(!capture_on_, "fake: S_FMT CAPTURE while streaming");
    FormatInfo granted = current_format(Queue::Capture);
    // The kernel substitutes a format it does not support with what is
    // already there, so a P010 request against this NV12 node is granted
    // NV12: the session must notice and stay on its G_FMT answer.
    granted.pixelformat = request.pixelformat == V4L2_PIX_FMT_P010 ? V4L2_PIX_FMT_NV12 : request.pixelformat;
    return granted;
}

std::optional<Selection> FakeDevice::get_compose(Queue queue)
{
    if (queue != Queue::Capture)
        return std::nullopt;
    return Selection { 0, 0, config_.visible_width, config_.visible_height };
}

std::optional<int32_t> FakeDevice::get_control(uint32_t id)
{
    if (id == V4L2_CID_MIN_BUFFERS_FOR_CAPTURE)
        return static_cast<int32_t>(config_.firmware_min_capture);
    auto it = controls_.find(id);
    if (it == controls_.end())
        return std::nullopt;
    return it->second;
}

bool FakeDevice::set_control(uint32_t id, int32_t value)
{
    if (id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY || id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE) {
        if (!config_.decode_order_control)
            return false;
        require(!output_on_, "fake: display-delay control changed while OUTPUT streams");
        if (id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY)
            require(value == 0, "fake: only display delay 0 is supported", VA_STATUS_ERROR_INVALID_PARAMETER);
        controls_[id] = value;
        note(id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY ? "S_CTRL DISPLAY_DELAY" : "S_CTRL DISPLAY_DELAY_ENABLE");
        return true;
    }
    return false;
}

unsigned FakeDevice::request_buffers(Queue queue, unsigned count, uint32_t memory)
{
    note("REQBUFS " + std::string(queue == Queue::Output ? "OUTPUT" : "CAPTURE") + " " + std::to_string(count)
        + (memory == V4L2_MEMORY_DMABUF ? " DMABUF" : ""));
    if (queue == Queue::Capture)
        capture_memory_ = memory;
    if (queue == Queue::Output) {
        require(output_format_set_, "fake: REQBUFS OUTPUT before S_FMT");
        output_count_ = count;
        output_memory_.assign(count, std::vector<uint8_t>(config_.au_capacity));
        output_queue_.clear();
        output_done_.clear();
        return count;
    }
    if (count == 0) {
        require(!capture_on_, "fake: REQBUFS CAPTURE 0 while streaming");
        capture_count_ = 0;
        capture_queue_.clear();
        capture_done_.clear();
        return 0;
    }
    capture_count_ = std::min(count, config_.capture_max);
    capture_queue_.clear();
    capture_done_.clear();
    capture_fds_.assign(capture_count_, -1);
    return capture_count_;
}

BufferInfo FakeDevice::query_buffer(Queue queue, unsigned index)
{
    if (queue == Queue::Output) {
        require(index < output_count_, "fake: QUERYBUF OUTPUT out of range");
        return BufferInfo { index, config_.au_capacity, index * config_.au_capacity, 0 };
    }
    require(index < capture_count_, "fake: QUERYBUF CAPTURE out of range");
    return BufferInfo { index, config_.stride * config_.coded_height * 3 / 2, 0, 0 };
}

int FakeDevice::export_buffer(Queue queue, unsigned index)
{
    require(queue == Queue::Capture && index < capture_count_, "fake: EXPBUF");
    require(capture_memory_ != V4L2_MEMORY_DMABUF, "fake: EXPBUF on a DMABUF queue");
    // An anonymous shared mapping stands in for the DMA-BUF; the Frame
    // closes it.
    const int fd = memfd_create("fake-capture", 0);
    require(fd >= 0, "fake: memfd");
    require(ftruncate(fd, config_.stride * config_.coded_height * 3 / 2) == 0, "fake: ftruncate");
    return fd;
}

void* FakeDevice::map_buffer(Queue queue, const BufferInfo& info)
{
    require(queue == Queue::Output && info.index < output_count_, "fake: map non-OUTPUT");
    return output_memory_[info.index].data();
}

void FakeDevice::unmap_buffer(void*, unsigned) { }

void FakeDevice::queue_buffer(Queue queue, unsigned index, unsigned bytesused, uint64_t timestamp_us, uint32_t)
{
    if (queue == Queue::Output) {
        require(index < output_count_, "fake: QBUF OUTPUT out of range");
        require(bytesused > 0 && bytesused <= config_.au_capacity, "fake: OUTPUT bytesused");
        require(timestamp_us != 0, "fake: OUTPUT with zero timestamp");
        for (const auto& queued : output_queue_)
            require(queued.index != index, "fake: OUTPUT index queued twice");
        require(!stopped_, "fake: QBUF OUTPUT after STOP without START");
        output_queue_.push_back({ index, timestamp_us });
        note("QBUF OUTPUT " + std::to_string(index));
        if (!source_change_pending_ && !capture_on_ && capture_count_ == 0 && decoded_since_stream_ == 0)
            source_change_pending_ = true;
        return;
    }
    require(index < capture_count_, "fake: QBUF CAPTURE out of range");
    require(capture_memory_ == V4L2_MEMORY_MMAP, "fake: MMAP QBUF on a DMABUF queue");
    for (unsigned queued : capture_queue_)
        require(queued != index, "fake: CAPTURE index queued twice");
    if (last_pending_) {
        emit_last(index);
        return;
    }
    capture_queue_.push_back(index);
}

void FakeDevice::emit_last(unsigned slot)
{
    last_pending_ = false;
    Dequeued last;
    last.index = slot;
    last.flags = V4L2_BUF_FLAG_LAST;
    last.bytesused = 0;
    capture_done_.push_back(last);
}

int FakeDevice::allocate_dmabuf(unsigned size)
{
    const int fd = memfd_create("fake-scratch", 0);
    require(fd >= 0 && ftruncate(fd, size) == 0, "fake: scratch memfd");
    note("ALLOC SCRATCH " + std::to_string(size));
    return fd;
}

void FakeDevice::queue_buffer_dmabuf(Queue queue, unsigned index, int dmabuf_fd, unsigned length, unsigned,
    uint64_t, uint32_t)
{
    require(queue == Queue::Capture, "fake: DMABUF QBUF only modelled for CAPTURE");
    require(capture_memory_ == V4L2_MEMORY_DMABUF, "fake: DMABUF QBUF on an MMAP queue");
    require(index < capture_count_, "fake: QBUF CAPTURE out of range");
    require(dmabuf_fd >= 0 && length >= config_.stride * config_.coded_height * 3 / 2,
        "fake: DMABUF too small for the CAPTURE format", VA_STATUS_ERROR_INVALID_PARAMETER);
    for (unsigned queued : capture_queue_)
        require(queued != index, "fake: CAPTURE index queued twice");
    // vb2 takes its own reference on QBUF, so a fake must too: the caller
    // closes its descriptor as soon as the QBUF returns.
    if (capture_fds_[index] >= 0)
        close(capture_fds_[index]);
    capture_fds_[index] = dup(dmabuf_fd);
    note("QBUF CAPTURE DMABUF " + std::to_string(index) + " fd=" + std::to_string(dmabuf_fd));
    if (drc_last_pending_) {
        drc_last_pending_ = false;
        drc_last_sent_ = true;
        emit_last(index);
        return;
    }
    if (last_pending_) {
        emit_last(index);
        return;
    }
    capture_queue_.push_back(index);
}

std::optional<Dequeued> FakeDevice::dequeue_buffer(Queue queue)
{
    if (queue == Queue::Output) {
        if (output_done_.empty())
            return std::nullopt;
        Dequeued result;
        result.index = output_done_.front();
        result.flags = output_done_flags_.front();
        output_done_.pop_front();
        output_done_flags_.pop_front();
        return result;
    }
    if (capture_eio_) {
        capture_eio_ = false;
        throw Error(VA_STATUS_ERROR_DECODING_ERROR, "DQBUF: Input/output error");
    }
    if (capture_done_.empty())
        return std::nullopt;
    Dequeued result = capture_done_.front();
    capture_done_.pop_front();
    return result;
}

void FakeDevice::stream(Queue queue, bool on)
{
    note(std::string(on ? "STREAMON " : "STREAMOFF ") + (queue == Queue::Output ? "OUTPUT" : "CAPTURE"));
    if (queue == Queue::Output) {
        output_on_ = on;
        if (!on) {
            output_queue_.clear();
            output_done_.clear();
            output_done_flags_.clear();
        }
        return;
    }
    if (on)
        require(output_on_, "fake: CAPTURE STREAMON before OUTPUT");
    capture_on_ = on;
    if (on) {
        drc_blocked_ = false;
        drc_last_sent_ = false;
        stopped_ = false;
    } else {
        capture_queue_.clear();
        capture_done_.clear();
        reorder_hold_.clear();
        decoded_since_stream_ = 0;
        last_pending_ = false;
    }
}

Event FakeDevice::dequeue_event()
{
    if (source_change_pending_) {
        source_change_pending_ = false;
        note("EVENT SOURCE_CHANGE");
        // A mid-stream source change is an implicit drain (dev-decoder.rst
        // "Dynamic Resolution Change"): the kernel finishes the pictures from
        // before the change and marks the boundary with LAST on a CAPTURE
        // buffer, without any decoder STOP. The bring-up source change is not
        // a drain and carries no LAST; the kernel models the two as
        // IRIS_INST_SUB_FIRST_IPSC and IRIS_INST_SUB_DRC_LAST. The marker may
        // be empty and belongs to no picture, so it borrows a free slot, or
        // the next one queued when an import pool has none.
        if (drc_change_ && !drc_last_sent_) {
            if (!capture_queue_.empty()) {
                const unsigned slot = capture_queue_.front();
                capture_queue_.pop_front();
                emit_last(slot);
                drc_last_sent_ = true;
            } else {
                drc_last_pending_ = true;
            }
        }
        drc_change_ = false;
        return Event::SourceChange;
    }
    return Event::None;
}

bool FakeDevice::decoder_command(uint32_t cmd)
{
    if (cmd == V4L2_DEC_CMD_STOP) {
        note("DECODER_CMD STOP");
        stopped_ = true;
        // Flush: complete everything queued, release the reorder hold, then
        // emit LAST on an empty buffer.
        while ((!output_queue_.empty() && !drc_blocked_) || !reorder_hold_.empty()) {
            if (capture_queue_.empty())
                break; // caller must requeue; LAST waits
            if (!reorder_hold_.empty()) {
                auto held = reorder_hold_.front();
                reorder_hold_.pop_front();
                const unsigned slot = capture_queue_.front();
                capture_queue_.pop_front();
                Dequeued done;
                done.index = slot;
                done.timestamp_us = held.timestamp;
                done.bytesused = config_.stride * config_.coded_height * 3 / 2;
                capture_done_.push_back(done);
                continue;
            }
            complete_one();
        }
        if (!capture_queue_.empty() && (output_queue_.empty() || drc_blocked_) && reorder_hold_.empty()) {
            const unsigned slot = capture_queue_.front();
            capture_queue_.pop_front();
            emit_last(slot);
        } else {
            last_pending_ = true;
        }
        return true;
    }
    if (cmd == V4L2_DEC_CMD_START) {
        if (start_busy_ > 0) {
            --start_busy_;
            note("DECODER_CMD START (EBUSY)");
            return false;
        }
        if (!output_queue_.empty()) {
            note("DECODER_CMD START (EBUSY: OUTPUT queued)");
            return false;
        }
        note("DECODER_CMD START");
        stopped_ = false;
        return true;
    }
    return true;
}

short FakeDevice::poll(short events, int)
{
    if (auto_tick_)
        tick(1);
    short revents = 0;
    if ((events & POLLIN) && !capture_done_.empty())
        revents |= POLLIN;
    if ((events & POLLPRI) && source_change_pending_)
        revents |= POLLPRI;
    if ((events & POLLOUT) && !output_done_.empty())
        revents |= POLLOUT;
    return revents;
}

void FakeDevice::complete_one()
{
    if (output_queue_.empty())
        return;
    const QueuedOutput au = output_queue_.front();
    output_queue_.pop_front();
    ++decoded_since_stream_;
    uint32_t output_flags = 0;
    if (output_errors_ > 0) {
        --output_errors_;
        output_flags = V4L2_BUF_FLAG_ERROR;
        output_done_.push_back(au.index);
        output_done_flags_.push_back(output_flags);
        return;
    }
    if (pending_drc_) {
        // The firmware parses the new-resolution AU, announces the change,
        // and leaves that OUTPUT buffer queued until CAPTURE is restarted
        // at the new geometry (dev-decoder.rst, dynamic resolution change).
        config_.coded_width = pending_drc_->first;
        config_.coded_height = pending_drc_->second;
        config_.visible_width = pending_drc_->first;
        config_.visible_height = pending_drc_->second;
        config_.stride = pending_drc_->first;
        pending_drc_.reset();
        source_change_pending_ = true;
        drc_change_ = true;
        output_queue_.push_front(au);
        --decoded_since_stream_;
        drc_blocked_ = true;
        return;
    }
    if (error_markers_ > 0 && !capture_queue_.empty()) {
        --error_markers_;
        const unsigned slot = capture_queue_.front();
        capture_queue_.pop_front();
        Dequeued marker;
        marker.index = slot;
        marker.flags = V4L2_BUF_FLAG_ERROR;
        marker.timestamp_us = 0;
        marker.bytesused = 0;
        capture_done_.push_back(marker);
    }
    if (timestamped_errors_ > 0 && !capture_queue_.empty()) {
        // A kernel with packaging/kernel/0004-* keeps the firmware timestamp
        // on an error completion, so the client can name the failed picture.
        --timestamped_errors_;
        const unsigned slot = capture_queue_.front();
        capture_queue_.pop_front();
        Dequeued corrupt;
        corrupt.index = slot;
        corrupt.flags = V4L2_BUF_FLAG_ERROR | timestamped_error_flags_;
        corrupt.timestamp_us = au.timestamp;
        corrupt.bytesused = 0;
        capture_done_.push_back(corrupt);
        output_done_.push_back(au.index);
        output_done_flags_.push_back(0);
        return;
    }
    if (config_.display_order_reorder && !stopped_) {
        // Hold this picture: display-order firmware withholds a decoded
        // picture until later input arrives. Release the oldest held one
        // once the window is deep enough.
        reorder_hold_.push_back(au);
        output_done_.push_back(au.index);
        output_done_flags_.push_back(0);
        if (reorder_hold_.size() <= config_.reorder_depth)
            return;
        const QueuedOutput release = reorder_hold_.front();
        reorder_hold_.pop_front();
        require(!capture_queue_.empty(), "fake: no free CAPTURE slot for completion");
        const unsigned slot = capture_queue_.front();
        capture_queue_.pop_front();
        Dequeued done;
        done.index = slot;
        done.timestamp_us = release.timestamp;
        done.bytesused = config_.stride * config_.coded_height * 3 / 2;
        capture_done_.push_back(done);
        return;
    }
    require(!capture_queue_.empty(), "fake: no free CAPTURE slot for completion");
    if (altref_superframe_ && capture_queue_.size() < 2) {
        // The firmware will not return the invisible half until it has a slot
        // for the visible one; the picture waits in the OUTPUT queue instead
        // of completing. This is the ~500 ms stall a one-buffer Import window
        // produced on the target, modelled rather than timed.
        output_queue_.push_front(au);
        --decoded_since_stream_;
        return;
    }
    unsigned slot = capture_queue_.front();
    if (misorder_ > 0 && capture_queue_.size() > 1) {
        // Model a firmware that does not fill CAPTURE in QBUF order.
        --misorder_;
        slot = capture_queue_[1];
        capture_queue_.erase(capture_queue_.begin() + 1);
    } else {
        capture_queue_.pop_front();
    }
    if (altref_superframe_) {
        // A VP9 alt-ref is one VA picture the firmware decodes as two: the
        // invisible frame completes first with the ERROR flag, no
        // picture-type bit and no payload, under the same token the visible
        // one will carry. Only the second completion has pixels.
        altref_superframe_ = false;
        Dequeued invisible;
        invisible.index = slot;
        invisible.flags = V4L2_BUF_FLAG_ERROR;
        invisible.timestamp_us = au.timestamp;
        invisible.bytesused = 0;
        capture_done_.push_back(invisible);
        require(!capture_queue_.empty(), "fake: no free CAPTURE slot for the visible frame");
        slot = capture_queue_.front();
        capture_queue_.pop_front();
    }
    Dequeued done;
    done.index = slot;
    done.timestamp_us = au.timestamp;
    if (token_skew_ > 0 && !output_queue_.empty()) {
        --token_skew_;
        done.timestamp_us = output_queue_.front().timestamp;
    }
    done.bytesused = config_.stride * config_.coded_height * 3 / 2;
    capture_done_.push_back(done);
    // CAPTURE returns before OUTPUT on Iris.
    output_done_.push_back(au.index);
    output_done_flags_.push_back(0);
    if (stopped_ && last_pending_ && (output_queue_.empty() || drc_blocked_) && !capture_queue_.empty()) {
        const unsigned next = capture_queue_.front();
        capture_queue_.pop_front();
        emit_last(next);
    }
}

void FakeDevice::tick(unsigned n)
{
    if (stalled_ || !capture_on_ || drc_blocked_)
        return;
    for (unsigned i = 0; i < n && !output_queue_.empty(); ++i)
        complete_one();
}

} // namespace iris::test
