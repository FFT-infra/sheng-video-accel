// SPDX-License-Identifier: MIT

#include "slots.h"

#include "../util/error.h"

#include <atomic>

extern "C" {
#include <sys/mman.h>
#include <unistd.h>
}

namespace iris {

uint64_t Frame::next_id()
{
    static std::atomic<uint64_t> counter { 1 };
    return counter.fetch_add(1);
}

Frame::~Frame()
{
    if (mapping_)
        munmap(mapping_, length_);
    if (fd_ >= 0)
        close(fd_);
}

uint8_t* Frame::map()
{
    require(fd_ >= 0, "an Import slot has no memory of its own");
    if (!mapping_) {
        void* mapping = mmap(nullptr, length_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        require(mapping != MAP_FAILED, "mmap CAPTURE DMA-BUF");
        mapping_ = mapping;
    }
    return static_cast<uint8_t*>(mapping_);
}

SlotPool::~SlotPool()
{
    for (auto& slot : output_)
        device_.unmap_buffer(slot.mapping, slot.length);
    if (scratch_fd_ >= 0)
        close(scratch_fd_);
}

void SlotPool::allocate_capture(unsigned count, const Layout& layout, CaptureMemory memory)
{
    require(capture_.empty(), "CAPTURE pool already allocated");
    const uint32_t v4l2_memory = memory == CaptureMemory::Import ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    const unsigned granted = device_.request_buffers(Queue::Capture, count, v4l2_memory);
    require(granted > 0 && granted <= 64, "REQBUFS CAPTURE returned an unusable count",
        VA_STATUS_ERROR_ALLOCATION_FAILED);
    capture_layout_ = layout;
    capture_memory_ = memory;
    capture_.reserve(granted);
    for (unsigned i = 0; i < granted; ++i) {
        if (memory == CaptureMemory::Import) {
            auto frame = std::make_shared<Frame>(device_, i, -1, layout.size);
            frame->layout() = layout;
            capture_.push_back(std::move(frame));
            continue;
        }
        const BufferInfo info = device_.query_buffer(Queue::Capture, i);
        const int fd = device_.export_buffer(Queue::Capture, i);
        auto frame = std::make_shared<Frame>(device_, i, fd, info.length);
        frame->layout() = layout;
        frame->layout().size = info.length;
        capture_.push_back(std::move(frame));
    }
}

void SlotPool::allocate_output(unsigned count)
{
    require(output_.empty(), "OUTPUT pool already allocated");
    const unsigned granted = device_.request_buffers(Queue::Output, count);
    require(granted > 0, "REQBUFS OUTPUT returned zero", VA_STATUS_ERROR_ALLOCATION_FAILED);
    output_.reserve(granted);
    for (unsigned i = 0; i < granted; ++i) {
        const BufferInfo info = device_.query_buffer(Queue::Output, i);
        OutputSlot slot;
        slot.index = i;
        slot.length = info.length;
        slot.mapping = device_.map_buffer(Queue::Output, info);
        output_.push_back(slot);
    }
}

void SlotPool::release_capture()
{
    // A slot a client still holds is not a reason to wait: each Frame owns
    // the DMA-BUF it was exported from, and vb2's dmabuf keeps a reference
    // on the vb2 buffer, so REQBUFS(0) detaches the buffer while the pages
    // stay alive for as long as the holder keeps the Frame. Verified on the
    // target: REQBUFS(0) succeeds with an exported fd open and the old
    // mapping reads back unchanged after the new pool is allocated.
    capture_.clear();
    device_.request_buffers(Queue::Capture, 0,
        capture_memory_ == CaptureMemory::Import ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP);
    capture_memory_ = CaptureMemory::Mmap;
    if (scratch_fd_ >= 0) {
        close(scratch_fd_);
        scratch_fd_ = -1;
    }
}

void SlotPool::release_output()
{
    for (auto& slot : output_) {
        device_.unmap_buffer(slot.mapping, slot.length);
        slot.mapping = nullptr;
    }
    output_.clear();
    device_.request_buffers(Queue::Output, 0);
}

unsigned SlotPool::requeue_capture()
{
    if (capture_memory_ == CaptureMemory::Import)
        return 0;
    unsigned queued = 0;
    for (auto& frame : capture_) {
        if (frame->queued_ || frame.use_count() != 1)
            continue;
        device_.queue_buffer(Queue::Capture, frame->index_, 0, 0, 0);
        frame->queued_ = true;
        ++queued;
    }
    return queued;
}

std::optional<unsigned> SlotPool::queue_capture_import(int fd, unsigned length, uint64_t token)
{
    require(capture_memory_ == CaptureMemory::Import, "queue_capture_import on an Mmap pool");
    Frame* chosen = nullptr;
    for (auto& frame : capture_) {
        if (frame->queued_)
            continue;
        if (frame->import_fd_ == fd) {
            chosen = frame.get();
            break;
        }
        if (!chosen)
            chosen = frame.get();
    }
    if (!chosen)
        return std::nullopt;
    device_.queue_buffer_dmabuf(Queue::Capture, chosen->index_, fd, length, 0, 0, 0);
    chosen->queued_ = true;
    chosen->import_fd_ = fd;
    chosen->import_token_ = token;
    return chosen->index_;
}

uint64_t SlotPool::take_import_token(unsigned index)
{
    require(index < capture_.size(), "CAPTURE index out of range", VA_STATUS_ERROR_DECODING_ERROR);
    const uint64_t token = capture_[index]->import_token_;
    capture_[index]->import_token_ = 0;
    return token;
}

unsigned SlotPool::capture_queued_for(uint64_t token) const
{
    unsigned count = 0;
    for (const auto& frame : capture_)
        count += (frame->queued_ && frame->import_token_ == token) ? 1 : 0;
    return count;
}

void SlotPool::queue_capture_scratch()
{
    require(capture_memory_ == CaptureMemory::Import, "scratch on an Mmap pool");
    if (scratch_fd_ < 0) {
        scratch_fd_ = device_.allocate_dmabuf(capture_layout_.size);
        require(scratch_fd_ >= 0, "allocate scratch CAPTURE buffer", VA_STATUS_ERROR_ALLOCATION_FAILED);
    }
    for (auto& frame : capture_) {
        if (frame->queued_)
            continue;
        device_.queue_buffer_dmabuf(Queue::Capture, frame->index_, scratch_fd_, capture_layout_.size, 0, 0, 0);
        frame->queued_ = true;
        frame->import_fd_ = scratch_fd_;
        frame->import_token_ = 0;
        return;
    }
    require(false, "no free CAPTURE slot for the scratch buffer", VA_STATUS_ERROR_HW_BUSY);
}

FrameRef SlotPool::take_capture(unsigned index)
{
    require(index < capture_.size(), "CAPTURE DQBUF index out of range", VA_STATUS_ERROR_DECODING_ERROR);
    auto& frame = capture_[index];
    frame->queued_ = false;
    return frame;
}

unsigned SlotPool::capture_queued() const
{
    unsigned count = 0;
    for (const auto& frame : capture_)
        count += frame->queued_ ? 1 : 0;
    return count;
}

unsigned SlotPool::capture_held() const
{
    unsigned count = 0;
    for (const auto& frame : capture_)
        count += frame.use_count() > 1 ? 1 : 0;
    return count;
}

OutputSlot* SlotPool::free_output()
{
    for (auto& slot : output_)
        if (!slot.queued)
            return &slot;
    return nullptr;
}

unsigned SlotPool::output_queued() const
{
    unsigned count = 0;
    for (const auto& slot : output_)
        count += slot.queued ? 1 : 0;
    return count;
}

void SlotPool::mark_all_capture_returned()
{
    for (auto& frame : capture_) {
        frame->queued_ = false;
        frame->import_token_ = 0;
    }
}

void SlotPool::mark_all_output_returned()
{
    for (auto& slot : output_) {
        slot.queued = false;
        slot.token = 0;
    }
}

} // namespace iris
