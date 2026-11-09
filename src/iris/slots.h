// SPDX-License-Identifier: MIT
//
// CAPTURE and OUTPUT slot ownership. A Frame is one CAPTURE slot's DMA-BUF;
// its shared_ptr use count is the slot state (docs/architecture.md §3.2):
//   1 and queued  -> firmware owns it
//   1 and !queued -> free, requeued on the next service pass
//   >1            -> held by a Surface or a copy engine view
// There is no separate "held" flag to drift out of sync with reality.
//
// An Import pool (CaptureMemory::Import) owns no memory: every slot is a
// vb2 index that carries whichever client DMA-BUF was queued into it for one
// picture. Frames of such a pool have no fd of their own, are never
// exported or held, and record the token they were queued for so the
// completion can be checked against the buffer it landed in.

#pragma once

#include "device.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace iris {

enum class CaptureMemory { Mmap, Import };

struct Layout {
    uint32_t fourcc = 0; // VA fourcc (NV12 / P010)
    unsigned width = 0, height = 0; // visible
    unsigned stride = 0;
    unsigned storage_height = 0; // rows before the chroma plane
    unsigned data_offset = 0;
    unsigned size = 0;
    unsigned chroma_offset() const { return data_offset + stride * storage_height; }
};

class Frame {
public:
    Frame(Device& device, unsigned index, int fd, unsigned length)
        : device_(device)
        , index_(index)
        , fd_(fd)
        , length_(length)
    {
    }
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    unsigned index() const { return index_; }
    int fd() const { return fd_; }
    uint64_t id() const { return id_; }
    unsigned length() const { return length_; }
    bool queued() const { return queued_; }
    // Import pools: the token whose picture this slot was queued for, and
    // the client fd it carries. 0 / -1 when idle.
    uint64_t import_token() const { return import_token_; }
    int import_fd() const { return import_fd_; }
    const Layout& layout() const { return layout_; }
    Layout& layout() { return layout_; }
    // CPU view of the DMA-BUF, mapped on first use. Only the CPU copy
    // engine and vaGetImage need it.
    uint8_t* map();

private:
    friend class SlotPool;
    static uint64_t next_id();
    uint64_t id_ = next_id();
    Device& device_;
    unsigned index_;
    int fd_;
    unsigned length_;
    bool queued_ = false;
    uint64_t import_token_ = 0;
    int import_fd_ = -1;
    void* mapping_ = nullptr;
    Layout layout_;
};

using FrameRef = std::shared_ptr<Frame>;

// One OUTPUT slot: MMAP'd bitstream memory for exactly one access unit.
struct OutputSlot {
    unsigned index = 0;
    unsigned length = 0;
    void* mapping = nullptr;
    bool queued = false;
    uint64_t token = 0; // the AU token this slot carries while queued
};

class SlotPool {
public:
    explicit SlotPool(Device& device)
        : device_(device)
    {
    }
    ~SlotPool();
    SlotPool(const SlotPool&) = delete;
    SlotPool& operator=(const SlotPool&) = delete;

    // Allocate a CAPTURE pool. Mmap: every slot is exported once here and
    // the fd lives as long as the Frame. Import: REQBUFS(DMABUF), no memory,
    // slots are filled per picture by queue_capture_import().
    void allocate_capture(unsigned count, const Layout& layout, CaptureMemory memory = CaptureMemory::Mmap);
    CaptureMemory capture_memory() const { return capture_memory_; }
    void allocate_output(unsigned count);
    // REQBUFS(0) both queues; requires no Frame to be held (use count 1).
    void release_capture();
    void release_output();

    // QBUF every free CAPTURE slot. Returns the number queued. A no-op for
    // an Import pool: its slots are queued by queue_capture_import() only.
    unsigned requeue_capture();
    // Import pool: queue the client's DMA-BUF `fd` (of `length` bytes) into
    // a free slot for the picture `token`. Prefers the slot that last
    // carried this fd (vb2 keeps the attachment when the fd is unchanged).
    // Returns the slot index, or nullopt when every slot is in flight.
    std::optional<unsigned> queue_capture_import(int fd, unsigned length, uint64_t token);
    // Import pool: the token slot `index` was queued for (0 for scratch or
    // idle); clears it.
    uint64_t take_import_token(unsigned index);
    // Import pool: how many CAPTURE slots are queued for `token` right now.
    // A picture whose access unit the firmware decodes as several (a VP9
    // alt-ref) needs a slot for each of them, and this is how the session
    // tells "the firmware holds only this picture's buffers" from "it holds
    // someone else's too".
    unsigned capture_queued_for(uint64_t token) const;
    // Import pool: queue a driver-owned scratch buffer so the firmware has
    // somewhere to put LAST. Allocated once, from the system DMA heap.
    void queue_capture_scratch();
    // Called on CAPTURE DQBUF: mark the slot returned and hand out the ref.
    FrameRef take_capture(unsigned index);
    // The number of slots the firmware currently owns.
    unsigned capture_queued() const;
    unsigned capture_held() const;
    unsigned capture_count() const { return static_cast<unsigned>(capture_.size()); }
    const Layout& capture_layout() const { return capture_layout_; }

    // OUTPUT slot bookkeeping.
    OutputSlot* free_output();
    OutputSlot& output(unsigned index) { return output_.at(index); }
    unsigned output_queued() const;
    unsigned output_count() const { return static_cast<unsigned>(output_.size()); }
    std::vector<OutputSlot>& outputs() { return output_; }

    // After STREAMOFF the kernel has returned every queued buffer without a
    // DQBUF; reflect that.
    void mark_all_capture_returned();
    void mark_all_output_returned();

private:
    Device& device_;
    std::vector<FrameRef> capture_;
    std::vector<OutputSlot> output_;
    Layout capture_layout_;
    CaptureMemory capture_memory_ = CaptureMemory::Mmap;
    int scratch_fd_ = -1;
};

} // namespace iris
