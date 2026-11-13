// SPDX-License-Identifier: MIT

#include "driver.h"

#include "../util/error.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <unistd.h>
#include <va/va_drmcommon.h>
}

namespace va {

VAStatus createBuffer(VADriverContextP ctx, VAContextID, VABufferType type, unsigned size, unsigned count,
    void* initial, VABufferID* id)
{
    switch (type) {
    case VAPictureParameterBufferType:
    case VAIQMatrixBufferType:
    case VASliceParameterBufferType:
    case VASliceDataBufferType:
    case VAImageBufferType:
    // Encode parameters. The firmware derives what it needs from these
    // before the first picture; the glue only copies them for the codec
    // translator.
    case VAEncSequenceParameterBufferType:
    case VAEncPictureParameterBufferType:
    case VAEncSliceParameterBufferType:
    case VAEncMiscParameterBufferType:
    case VAEncPackedHeaderParameterBufferType:
    case VAEncPackedHeaderDataBufferType:
    // The coded buffer's capacity is `size`; the bytes land in `payload`
    // when vaEndPicture returns, and mapBuffer hands out the segment.
    case VAEncCodedBufferType:
    // VideoProc: the pipeline parameters, and filter parameter buffers a
    // client may create even though no filter is implemented.
    case VAProcPipelineParameterBufferType:
    case VAProcFilterParameterBufferType:
        break;
    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
    auto& d = data(ctx);
    auto b = std::make_shared<Buffer>();
    b->type = type;
    b->size = size;
    b->count = count;
    if (type != VAEncCodedBufferType)
        b->data.resize(static_cast<size_t>(size) * count);
    if (initial && !b->data.empty())
        std::memcpy(b->data.data(), initial, b->data.size());
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    *id = d.next_id++;
    d.buffers[*id] = b;
    return VA_STATUS_SUCCESS;
}

VAStatus destroyBuffer(VADriverContextP ctx, VABufferID id)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (it->second->acquired_fd >= 0)
        close(it->second->acquired_fd);
    d.buffers.erase(it);
    return VA_STATUS_SUCCESS;
}

VAStatus claim_coded(DriverData& d, const std::shared_ptr<Buffer>& b, uint64_t timeout_ns)
{
    std::optional<iris::EncodeTicket> ticket;
    std::shared_ptr<iris::EncoderSession> session;
    {
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        // Nothing outstanding means somebody already claimed this picture —
        // vaSyncSurface on the input surface, or an earlier syncBuffer or
        // mapBuffer — and the bytes are on the buffer. Success is the
        // answer, not "unknown buffer".
        if (b->type != VAEncCodedBufferType || !b->ticket)
            return VA_STATUS_SUCCESS;
        ticket = b->ticket;
        session = b->coded_session;
    }
    iris::require(bool(session), "coded buffer lost its encode session", VA_STATUS_ERROR_INVALID_CONTEXT);
    const iris::EncodeResult result = session->wait_timeout(*ticket, timeout_ns);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    // An empty result is the session saying this ticket was already answered,
    // which happens when another thread claimed it between the read above and
    // this wait. The bytes are on the buffer already; overwriting them with
    // nothing is the bug this function exists to remove.
    if (result.bytes.empty())
        return VA_STATUS_SUCCESS;
    iris::require(result.bytes.size() <= b->size, "coded buffer is too small", VA_STATUS_ERROR_ENCODING_ERROR);
    b->payload = result.bytes;
    b->segment.size = static_cast<uint32_t>(b->payload.size());
    b->segment.bit_offset = 0;
    b->segment.status = 0;
    b->segment.reserved = 0;
    b->segment.buf = b->payload.empty() ? nullptr : b->payload.data();
    b->segment.next = nullptr;
    b->ticket.reset();
    b->coded_session.reset();
    return VA_STATUS_SUCCESS;
}

VAStatus mapBuffer(VADriverContextP ctx, VABufferID id, void** mapped)
{
    auto& d = data(ctx);
    std::shared_ptr<Buffer> b;
    {
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        b = buffer(d, id);
    }
    // A coded buffer whose picture has not been claimed yet. The client
    // either called vaSyncBuffer or vaSyncSurface already (then nothing is
    // outstanding and this returns at once) or is using the older contract
    // where reading the buffer implies waiting.
    if (b->type == VAEncCodedBufferType) {
        const VAStatus status = claim_coded(d, b, VA_TIMEOUT_INFINITE);
        if (status != VA_STATUS_SUCCESS)
            return status;
    }
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    auto& mapped_buffer = *it->second;
    if (mapped_buffer.type == VAEncCodedBufferType) {
        // vaMapBuffer on a coded buffer returns a linked list of
        // VACodedBufferSegment, terminated by next == NULL. One segment is
        // the whole access unit.
        mapped_buffer.segment.size = static_cast<uint32_t>(mapped_buffer.payload.size());
        mapped_buffer.segment.bit_offset = 0;
        mapped_buffer.segment.status = 0;
        mapped_buffer.segment.reserved = 0;
        mapped_buffer.segment.buf = mapped_buffer.payload.empty() ? nullptr : mapped_buffer.payload.data();
        mapped_buffer.segment.next = nullptr;
        *mapped = &mapped_buffer.segment;
        return VA_STATUS_SUCCESS;
    }
    // A derived image is a view over the surface's DMA-BUF, not a copy.
    if (mapped_buffer.alias) {
        if (mapped_buffer.alias_fd >= 0)
            iris::dma_buf_cpu_sync(
                mapped_buffer.alias_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE);
        *mapped = mapped_buffer.alias;
        return VA_STATUS_SUCCESS;
    }
    *mapped = mapped_buffer.data.data();
    return VA_STATUS_SUCCESS;
}

VAStatus unmapBuffer(VADriverContextP ctx, VABufferID id)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    // The client wrote through a mapped derived image: end the CPU access
    // window so the VPU sees those bytes when the surface is submitted.
    const auto& b = *it->second;
    if (b.alias && b.alias_fd >= 0)
        iris::dma_buf_cpu_sync(b.alias_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    return VA_STATUS_SUCCESS;
}

VAStatus bufferSetNumElements(VADriverContextP ctx, VABufferID id, unsigned count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    it->second->count = count;
    it->second->data.resize(static_cast<size_t>(it->second->size) * count);
    return VA_STATUS_SUCCESS;
}

VAStatus bufferInfo(VADriverContextP ctx, VABufferID id, VABufferType* type, unsigned* size, unsigned* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    *type = it->second->type;
    *size = it->second->size;
    *count = it->second->count;
    return VA_STATUS_SUCCESS;
}

// Handles for derived images: the DMA-BUF behind the surface. Acquiring a
// handle before the surface's first decode marks it persistent, like an
// export does.
VAStatus acquireBufferHandle(VADriverContextP ctx, VABufferID id, VABufferInfo* info)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    auto& b = *it->second;
    if (b.type != VAImageBufferType || b.derived_surface == VA_INVALID_ID)
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    if (info->mem_type && !(info->mem_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME))
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    auto sit = d.surfaces.find(b.derived_surface);
    if (sit == d.surfaces.end())
        return VA_STATUS_ERROR_INVALID_SURFACE;
    auto& s = *sit->second;
    int fd = -1;
    unsigned size = 0;
    if (s.target.frame) {
        fd = s.target.frame->fd();
        size = s.target.frame->length();
    } else {
        auto& stable = ensure_stable(d, s);
        fd = stable.fd();
        size = stable.size();
        if (!s.target.completed)
            s.persistent_export = true;
    }
    // A second vaAcquireBufferHandle for the same buffer hands back the
    // handle the first one made. Duplicating again would leak the old fd and
    // leave the surface's count one higher than the releases can settle, so
    // vaDestroySurfaces would report SURFACE_BUSY for the rest of the
    // process.
    if (b.acquired_fd < 0) {
        b.acquired_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (b.acquired_fd < 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        ++s.acquired_handles;
    }
    *info = {};
    info->handle = static_cast<uintptr_t>(b.acquired_fd);
    info->type = b.type;
    info->mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    info->mem_size = size;
    return VA_STATUS_SUCCESS;
}

VAStatus releaseBufferHandle(VADriverContextP ctx, VABufferID id)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.buffers.find(id);
    if (it == d.buffers.end())
        return VA_STATUS_ERROR_INVALID_BUFFER;
    auto& b = *it->second;
    if (b.acquired_fd >= 0) {
        close(b.acquired_fd);
        b.acquired_fd = -1;
        auto sit = d.surfaces.find(b.derived_surface);
        if (sit != d.surfaces.end() && sit->second->acquired_handles > 0)
            --sit->second->acquired_handles;
    }
    return VA_STATUS_SUCCESS;
}

} // namespace va
