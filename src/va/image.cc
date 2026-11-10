// SPDX-License-Identifier: MIT
//
// Images: vaGetImage copies the visible region of a surface's pixels into a
// packed client buffer; vaDeriveImage exposes the surface's own memory.
// Subpictures are not implemented.

#include "driver.h"

#include "../util/error.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <linux/dma-buf.h>
}

namespace va {

namespace {

VAStatus guarded(VADriverContextP ctx, const char* where, auto&& body)
{
    try {
        return body();
    } catch (const std::exception& error) {
        return report(ctx, where, error);
    }
}

// Packed NV12/P010 layout for a VAImage of the given size.
void fill_image(VAImage& image, uint32_t fourcc, unsigned width, unsigned height)
{
    const unsigned bytes = fourcc == VA_FOURCC_P010 ? 2 : 1;
    image.format.fourcc = fourcc;
    image.format.byte_order = VA_LSB_FIRST;
    image.format.bits_per_pixel = bytes == 2 ? 24 : 12;
    image.width = static_cast<uint16_t>(width);
    image.height = static_cast<uint16_t>(height);
    image.num_planes = 2;
    image.pitches[0] = width * bytes;
    image.pitches[1] = ((width + 1) & ~1u) * bytes;
    image.offsets[0] = 0;
    image.offsets[1] = image.pitches[0] * height;
    image.data_size = image.offsets[1] + image.pitches[1] * ((height + 1) / 2);
    image.num_palette_entries = 0;
    image.entry_bytes = 0;
}

// A derived image describes the surface's own memory: the pitches, offsets
// and size come from the DMA-BUF layout, not from a packed copy.
void view_image(VAImage& image, uint32_t fourcc, unsigned width, unsigned height, const iris::Layout& layout)
{
    const unsigned bytes = fourcc == VA_FOURCC_P010 ? 2 : 1;
    image.format.fourcc = fourcc;
    image.format.byte_order = VA_LSB_FIRST;
    image.format.bits_per_pixel = bytes == 2 ? 24 : 12;
    image.width = static_cast<uint16_t>(width);
    image.height = static_cast<uint16_t>(height);
    image.num_planes = 2;
    image.pitches[0] = layout.stride;
    image.pitches[1] = layout.stride;
    image.offsets[0] = layout.data_offset;
    image.offsets[1] = layout.chroma_offset();
    image.data_size = layout.size;
    image.num_palette_entries = 0;
    image.entry_bytes = 0;
}

// Copy the visible width x height from a surface's current pixels (held
// CAPTURE frame, or stable buffer) into a packed image buffer.
void copy_pixels(DriverData& d, Surface& s, const VAImage& image, uint8_t* destination)
{
    iris::require(s.has_pixels(), "surface has no decoded pixels", VA_STATUS_ERROR_OPERATION_FAILED);
    const uint8_t* source = nullptr;
    iris::Layout layout;
    int fd = -1;
    if (s.target.frame) {
        source = s.target.frame->map();
        layout = s.target.frame->layout();
        fd = s.target.frame->fd();
    } else {
        source = s.stable->map();
        layout = s.stable->layout();
        fd = s.stable->fd();
    }
    iris::require(layout.fourcc == image.format.fourcc, "image format does not match the surface",
        VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
    const unsigned bytes = layout.fourcc == VA_FOURCC_P010 ? 2 : 1;
    const unsigned width = std::min<unsigned>(image.width, layout.width);
    const unsigned height = std::min<unsigned>(image.height, layout.height);
    (void)d;
    iris::dma_buf_cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    for (unsigned row = 0; row < height; ++row)
        std::memcpy(destination + image.offsets[0] + static_cast<size_t>(row) * image.pitches[0],
            source + layout.data_offset + static_cast<size_t>(row) * layout.stride, width * bytes);
    for (unsigned row = 0; row < (height + 1) / 2; ++row)
        std::memcpy(destination + image.offsets[1] + static_cast<size_t>(row) * image.pitches[1],
            source + layout.chroma_offset() + static_cast<size_t>(row) * layout.stride, ((width + 1) & ~1u) * bytes);
    iris::dma_buf_cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

} // namespace

VAStatus queryImageFormats(VADriverContextP ctx, VAImageFormat* formats, int* count)
{
    auto& d = data(ctx);
    int n = 0;
    formats[n] = {};
    formats[n].fourcc = VA_FOURCC_NV12;
    formats[n].byte_order = VA_LSB_FIRST;
    formats[n].bits_per_pixel = 12;
    ++n;
    if (d.capabilities.p010) {
        formats[n] = {};
        formats[n].fourcc = VA_FOURCC_P010;
        formats[n].byte_order = VA_LSB_FIRST;
        formats[n].bits_per_pixel = 24;
        ++n;
    }
    *count = n;
    return VA_STATUS_SUCCESS;
}

VAStatus createImage(VADriverContextP ctx, VAImageFormat* format, int width, int height, VAImage* image)
{
    return guarded(ctx, "createImage", [&] {
        auto& d = data(ctx);
        iris::require(format && (format->fourcc == VA_FOURCC_NV12 || format->fourcc == VA_FOURCC_P010),
            "image format not supported", VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
        iris::require(width > 0 && height > 0, "image geometry", VA_STATUS_ERROR_INVALID_PARAMETER);
        *image = {};
        fill_image(*image, format->fourcc, static_cast<unsigned>(width), static_cast<unsigned>(height));
        VABufferID buffer_id;
        const VAStatus status = createBuffer(ctx, VA_INVALID_ID, VAImageBufferType, image->data_size, 1, nullptr, &buffer_id);
        if (status != VA_STATUS_SUCCESS)
            return status;
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        image->buf = buffer_id;
        image->image_id = d.next_id++;
        d.images[image->image_id] = *image;
        return VA_STATUS_SUCCESS;
    });
}

VAStatus destroyImage(VADriverContextP ctx, VAImageID id)
{
    auto& d = data(ctx);
    VABufferID buffer_id;
    VASurfaceID derived = VA_INVALID_ID;
    {
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        auto it = d.images.find(id);
        if (it == d.images.end())
            return VA_STATUS_ERROR_INVALID_IMAGE;
        buffer_id = it->second.buf;
        d.images.erase(it);
        auto bit = d.buffers.find(buffer_id);
        if (bit != d.buffers.end())
            derived = bit->second->derived_surface;
        if (derived != VA_INVALID_ID) {
            auto sit = d.surfaces.find(derived);
            if (sit != d.surfaces.end() && sit->second->derived_images > 0)
                --sit->second->derived_images;
        }
    }
    return destroyBuffer(ctx, buffer_id);
}

VAStatus deriveImage(VADriverContextP ctx, VASurfaceID id, VAImage* image)
{
    return guarded(ctx, "deriveImage", [&] {
        auto& d = data(ctx);
        std::shared_ptr<Surface> s;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            s = surface(d, id);
        }
        if (s->rendering())
            syncSurface(ctx, id);
        // A view, not a copy. FFmpeg's encode upload maps the derived image
        // and writes straight into it, then unmaps without ever calling
        // vaPutImage; anything else silently drops the frame.
        iris::Layout layout;
        int fd = -1;
        void* mapping = nullptr;
        if (s->target.frame) {
            layout = s->target.frame->layout();
            fd = s->target.frame->fd();
            mapping = s->target.frame->map();
        } else {
            iris::StableBuffer* stable = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(d.mutex);
                stable = &ensure_stable(d, *s);
            }
            layout = stable->layout();
            fd = stable->fd();
            mapping = stable->map();
        }
        iris::require(mapping != nullptr, "surface has no CPU mapping", VA_STATUS_ERROR_OPERATION_FAILED);

        VAImage view = {};
        view_image(view, s->fourcc, s->width, s->height, layout);
        auto b = std::make_shared<Buffer>();
        b->type = VAImageBufferType;
        b->size = layout.size;
        b->count = 1;
        b->alias = mapping;
        b->alias_surface = id;
        b->alias_fd = fd;
        b->alias_owner = s;
        b->alias_frame = s->target.frame;
        b->derived_surface = id;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            view.buf = d.next_id++;
            view.image_id = d.next_id++;
            d.buffers[view.buf] = b;
            d.images[view.image_id] = view;
            ++s->derived_images;
        }
        *image = view;
        return VA_STATUS_SUCCESS;
    });
}

VAStatus setImagePalette(VADriverContextP, VAImageID, unsigned char*) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

VAStatus getImage(VADriverContextP ctx, VASurfaceID id, int x, int y, unsigned width, unsigned height,
    VAImageID image_id)
{
    return guarded(ctx, "getImage", [&] {
        auto& d = data(ctx);
        std::shared_ptr<Surface> s;
        VAImage image;
        std::shared_ptr<Buffer> b;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            s = surface(d, id);
            auto it = d.images.find(image_id);
            iris::require(it != d.images.end(), "invalid image", VA_STATUS_ERROR_INVALID_IMAGE);
            image = it->second;
            b = buffer(d, image.buf);
        }
        iris::require(x == 0 && y == 0 && width == image.width && height == image.height,
            "partial vaGetImage is not supported", VA_STATUS_ERROR_UNIMPLEMENTED);
        if (s->rendering())
            syncSurface(ctx, id);
        // An image obtained from vaDeriveImage already is a view of these
        // pixels; copying a surface onto itself would need a direction the
        // VAImage does not carry.
        if (b->alias) {
            iris::require(b->alias_surface == id, "vaGetImage into an image derived from another surface",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            return VA_STATUS_SUCCESS;
        }
        copy_pixels(d, *s, image, b->data.data());
        return VA_STATUS_SUCCESS;
    });
}

VAStatus putImage(VADriverContextP, VASurfaceID, VAImageID, int, int, unsigned, unsigned, int, int, unsigned, unsigned)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus querySubpictureFormats(VADriverContextP, VAImageFormat*, unsigned*, unsigned* count)
{
    *count = 0;
    return VA_STATUS_SUCCESS;
}
VAStatus createSubpicture(VADriverContextP, VAImageID, VASubpictureID*) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus destroySubpicture(VADriverContextP, VASubpictureID) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus setSubpictureImage(VADriverContextP, VASubpictureID, VAImageID) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus setSubpictureChromakey(VADriverContextP, VASubpictureID, unsigned, unsigned, unsigned)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
VAStatus setSubpictureGlobalAlpha(VADriverContextP, VASubpictureID, float) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus associateSubpicture(VADriverContextP, VASubpictureID, VASurfaceID*, int, short, short, unsigned short,
    unsigned short, short, short, unsigned short, unsigned short, unsigned)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
VAStatus deassociateSubpicture(VADriverContextP, VASubpictureID, VASurfaceID*, int)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

} // namespace va
