// SPDX-License-Identifier: MIT
//
// Memory for surfaces whose DMA-BUF must outlive any single CAPTURE slot:
// persistent exports (Chrome's pre-exported pool) and vaGetImage targets.
// Allocation order: MSM GEM cached-coherent, MSM GEM write-combined, system
// DMA heap. The fd, pitch and offsets never change for the buffer's life.

#pragma once

#include "slots.h"

#include <cstdint>
#include <memory>

namespace iris {

enum class MemoryOrigin { MsmCoherent, MsmWriteCombined, DmaHeap, Imported };

class StableBuffer {
public:
    StableBuffer(int fd, unsigned size, MemoryOrigin origin, const Layout& layout)
        : fd_(fd)
        , size_(size)
        , origin_(origin)
        , layout_(layout)
    {
    }
    ~StableBuffer();
    StableBuffer(const StableBuffer&) = delete;
    StableBuffer& operator=(const StableBuffer&) = delete;

    int fd() const { return fd_; }
    unsigned size() const { return size_; }
    MemoryOrigin origin() const { return origin_; }
    const Layout& layout() const { return layout_; }
    // Process-unique identity, never reused. Copy engines key cached GPU
    // views on this so a recycled pointer or fd cannot alias a freed buffer.
    uint64_t id() const { return id_; }
    uint8_t* map();

private:
    uint64_t id_ = next_id();
    static uint64_t next_id();
    int fd_;
    unsigned size_;
    MemoryOrigin origin_;
    Layout layout_;
    void* mapping_ = nullptr;
};

// Compute the linear layout used for every stable buffer. It is the Iris
// CAPTURE layout for the same geometry (128-byte pitch, 32-row luma,
// 16-row chroma, 4 KiB total), so an Import session can hand the buffer to
// the decoder unchanged; it also satisfies freedreno's linear import rules.
Layout stable_layout(uint32_t fourcc, unsigned width, unsigned height);

// One buffer from the system DMA heap; -1 on failure.
int allocate_dma_heap(unsigned size);

// render_fd < 0 skips the GEM attempts and goes straight to the DMA heap.
std::unique_ptr<StableBuffer> allocate_stable(int render_fd, const Layout& layout);

// DMA_BUF_IOCTL_SYNC around CPU access. Returns false if the kernel does
// not implement sync for this buffer (system heap on some kernels), which
// is not an error for the caller.
bool dma_buf_cpu_sync(int fd, uint64_t flags);

// Wait for the buffer's implicit fence so a compositor still sampling the
// previous frame is not overwritten under it. POLLOUT for write access.
void wait_dma_buf(int fd, bool write, int timeout_ms);

} // namespace iris
