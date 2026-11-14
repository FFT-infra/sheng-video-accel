// SPDX-License-Identifier: MIT

#include "publish.h"

#include "../util/error.h"

#include <cstring>
#include <mutex>
#include <utility>

extern "C" {
#include <linux/dma-buf.h>
}

namespace iris {

namespace {

void validate(const Layout& layout, unsigned width, unsigned height, unsigned bytes, unsigned size)
{
    const uint64_t chroma_rows = (height + 1) / 2;
    const uint64_t row_bytes = static_cast<uint64_t>((width + 1) & ~1u) * bytes;
    const uint64_t chroma_offset = layout.chroma_offset();
    require(width && height && width <= layout.width && height <= layout.height && height <= layout.storage_height
            && row_bytes <= layout.stride && chroma_offset + (chroma_rows - 1) * layout.stride + row_bytes <= size,
        "frame copy exceeds buffer layout", VA_STATUS_ERROR_INVALID_SURFACE);
}

class CpuCopy final : public CopyEngine {
public:
    const char* name() const override { return "cpu"; }
    void forget(uint64_t) override { }

    void copy(StableBuffer& destination, Frame& source) override
    {
        const Layout& from = source.layout();
        const Layout& to = destination.layout();
        require(from.fourcc == to.fourcc, "frame copy fourcc mismatch", VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
        const unsigned bytes = from.fourcc == VA_FOURCC_P010 ? 2 : 1;
        const unsigned width = to.width, height = to.height;
        validate(from, width, height, bytes, source.length());
        validate(to, width, height, bytes, destination.size());

        uint8_t* src = source.map();
        uint8_t* dst = destination.map();
        wait_dma_buf(destination.fd(), true, 5000);
        dma_buf_cpu_sync(source.fd(), DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        dma_buf_cpu_sync(destination.fd(), DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        for (unsigned plane = 0; plane < 2; ++plane) {
            const size_t src_offset = plane ? from.chroma_offset() : from.data_offset;
            const size_t dst_offset = plane ? to.chroma_offset() : to.data_offset;
            const unsigned rows = plane ? (height + 1) / 2 : height;
            const unsigned row_bytes = (plane ? ((width + 1) & ~1u) : width) * bytes;
            for (unsigned row = 0; row < rows; ++row)
                std::memcpy(dst + dst_offset + static_cast<size_t>(row) * to.stride,
                    src + src_offset + static_cast<size_t>(row) * from.stride, row_bytes);
        }
        dma_buf_cpu_sync(destination.fd(), DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        dma_buf_cpu_sync(source.fd(), DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    }
};

class DeferredCopy final : public CopyEngine {
public:
    explicit DeferredCopy(std::function<std::unique_ptr<CopyEngine>()> factory)
        : factory_(std::move(factory))
    {
    }

    // Before the first copy this reports "deferred": the session traces the
    // name after a copy, so a started session never sees this value.
    const char* name() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return engine_ ? engine_->name() : "deferred";
    }

    void copy(StableBuffer& destination, Frame& source) override { engine().copy(destination, source); }

    // A forget with no backend has nothing to release. Building one here
    // would undo the deferral for a client that only ever publishes Direct.
    void forget(uint64_t id) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (engine_)
            engine_->forget(id);
    }

private:
    CopyEngine& engine()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!engine_)
            engine_ = factory_();
        return *engine_;
    }

    std::function<std::unique_ptr<CopyEngine>()> factory_;
    mutable std::mutex mutex_;
    std::unique_ptr<CopyEngine> engine_;
};

} // namespace

std::unique_ptr<CopyEngine> make_cpu_copy() { return std::make_unique<CpuCopy>(); }

std::unique_ptr<CopyEngine> make_deferred_copy(int render_fd, CopyMode mode)
{
    return make_deferred_copy([render_fd, mode] {
        std::unique_ptr<CopyEngine> engine;
        if (mode == CopyMode::Gpu)
            engine = make_gpu_copy(render_fd);
        if (!engine)
            engine = make_cpu_copy();
        return engine;
    });
}

std::unique_ptr<CopyEngine> make_deferred_copy(std::function<std::unique_ptr<CopyEngine>()> factory)
{
    return std::make_unique<DeferredCopy>(std::move(factory));
}

} // namespace iris

#ifndef IRIS_HAVE_GPU_COPY
namespace iris {
std::unique_ptr<CopyEngine> make_gpu_copy(int) { return nullptr; }
}
#endif
