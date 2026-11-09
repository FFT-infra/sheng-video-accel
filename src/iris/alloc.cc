// SPDX-License-Identifier: MIT

#include "alloc.h"

#include "../util/error.h"
#include "../util/prime.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>

extern "C" {
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
}

// Implemented in msm_alloc.c: libdrm's msm_drm.h uses the C++ operator
// keyword "or" as an identifier and cannot be included from C++.
extern "C" int iris_msm_allocate(int render_fd, unsigned size, int* coherent);

extern "C" {
#include <va/va.h>
}

namespace iris {

uint64_t StableBuffer::next_id()
{
    static std::atomic<uint64_t> counter { 1 };
    return counter.fetch_add(1);
}

StableBuffer::~StableBuffer()
{
    if (mapping_)
        munmap(mapping_, size_);
    if (fd_ >= 0)
        close(fd_);
}

uint8_t* StableBuffer::map()
{
    if (!mapping_) {
        void* mapping = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        require(mapping != MAP_FAILED, "mmap stable DMA-BUF");
        mapping_ = mapping;
    }
    return static_cast<uint8_t*>(mapping_);
}

Layout stable_layout(uint32_t fourcc, unsigned width, unsigned height)
{
    // The Iris CAPTURE layout, with the total rounded up to 4 KiB. A stable
    // buffer with this layout can be queued into CAPTURE as the client's
    // own memory (Import publish); it also satisfies freedreno's linear
    // import rules for the GPU copy.
    const PlaneLayout planes = plane_layout(fourcc, width, height);
    Layout layout;
    layout.fourcc = fourcc;
    layout.width = width;
    layout.height = height;
    layout.stride = planes.stride;
    layout.storage_height = planes.storage_height;
    layout.data_offset = 0;
    const uint64_t total = (static_cast<uint64_t>(planes.size) + 4095) & ~static_cast<uint64_t>(4095);
    require(total <= 0xffffffffu, "stable buffer geometry", VA_STATUS_ERROR_INVALID_PARAMETER);
    layout.size = static_cast<unsigned>(total);
    return layout;
}

namespace {

int allocate_msm(int render_fd, unsigned size, MemoryOrigin* origin)
{
    int coherent = 0;
    const int fd = iris_msm_allocate(render_fd, size, &coherent);
    if (fd < 0)
        return -1;
    *origin = coherent ? MemoryOrigin::MsmCoherent : MemoryOrigin::MsmWriteCombined;
    return fd;
}

} // namespace

int allocate_dma_heap(unsigned size)
{
    const int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap < 0)
        return -1;
    dma_heap_allocation_data request = {};
    request.len = size;
    request.fd_flags = O_RDWR | O_CLOEXEC;
    const int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &request);
    close(heap);
    return result < 0 ? -1 : static_cast<int>(request.fd);
}

std::unique_ptr<StableBuffer> allocate_stable(int render_fd, const Layout& layout)
{
    MemoryOrigin origin = MemoryOrigin::DmaHeap;
    int fd = render_fd >= 0 ? allocate_msm(render_fd, layout.size, &origin) : -1;
    if (fd < 0) {
        fd = allocate_dma_heap(layout.size);
        origin = MemoryOrigin::DmaHeap;
    }
    require(fd >= 0, "allocate stable surface memory", VA_STATUS_ERROR_ALLOCATION_FAILED);
    auto buffer = std::make_unique<StableBuffer>(fd, layout.size, origin, layout);
    // Start every stable buffer black rather than with whatever the
    // allocator left in it; the green flash from the stream-switch reports
    // was uninitialised chroma.
    uint8_t* mapping = buffer->map();
    dma_buf_cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    std::memset(mapping, 0, layout.stride * layout.storage_height);
    std::memset(mapping + layout.chroma_offset(), 0x80, layout.size - layout.chroma_offset());
    dma_buf_cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
    return buffer;
}

bool dma_buf_cpu_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync request = {};
    request.flags = flags;
    int result;
    do {
        result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &request);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

void wait_dma_buf(int fd, bool write, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    pollfd fence = { fd, static_cast<short>(write ? POLLOUT : POLLIN), 0 };
    for (;;) {
        const int result = ::poll(&fence, 1, 100);
        if (result < 0 && errno == EINTR)
            continue;
        require(result >= 0 && !(fence.revents & (POLLERR | POLLNVAL | POLLHUP)), "DMA-BUF fence wait");
        if (result > 0 && (fence.revents & fence.events))
            return;
        require(std::chrono::steady_clock::now() < deadline, "DMA-BUF fence timeout", VA_STATUS_ERROR_TIMEDOUT);
    }
}

} // namespace iris
