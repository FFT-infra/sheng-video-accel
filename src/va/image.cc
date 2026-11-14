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

bool rgb_fourcc(uint32_t fourcc)
{
    return fourcc == VA_FOURCC_BGRA || fourcc == VA_FOURCC_RGBA;
}

// I420 is the same 4:2:0 picture as NV12 with the chroma pair stored as two
// separate planes instead of interleaved bytes. The surfaces the driver
// decodes into are always NV12, so an I420 image is a client-side layout the
// two copy paths translate, not a second surface format.
bool planar_chroma(uint32_t fourcc)
{
    return fourcc == VA_FOURCC_I420;
}

unsigned bytes_of(uint32_t fourcc)
{
    if (fourcc == VA_FOURCC_P010)
        return 2;
    return rgb_fourcc(fourcc) ? 4 : 1;
}

// An image the copy paths can move between a surface and a client buffer:
// the surface's own format, or I420 for the same 8-bit 4:2:0 picture.
bool compatible(uint32_t image_fourcc, uint32_t surface_fourcc)
{
    return image_fourcc == surface_fourcc
        || (planar_chroma(image_fourcc) && surface_fourcc == VA_FOURCC_NV12);
}

// The packed BGRA/RGBA surface the processor composes from is one plane of
// 32-bit words; VA names the bytes in memory order.
void fill_masks(VAImageFormat& format)
{
    if (format.fourcc == VA_FOURCC_BGRA) {
        format.red_mask = 0x00ff0000;
        format.green_mask = 0x0000ff00;
        format.blue_mask = 0x000000ff;
    } else if (format.fourcc == VA_FOURCC_RGBA) {
        format.red_mask = 0x000000ff;
        format.green_mask = 0x0000ff00;
        format.blue_mask = 0x00ff0000;
    }
    if (rgb_fourcc(format.fourcc))
        format.alpha_mask = 0xff000000;
}

// Packed layout for a VAImage of the given size.
void fill_image(VAImage& image, uint32_t fourcc, unsigned width, unsigned height)
{
    const unsigned bytes = bytes_of(fourcc);
    image.format.fourcc = fourcc;
    image.format.byte_order = VA_LSB_FIRST;
    image.format.bits_per_pixel = rgb_fourcc(fourcc) ? 32 : (bytes == 2 ? 24 : 12);
    fill_masks(image.format);
    image.width = static_cast<uint16_t>(width);
    image.height = static_cast<uint16_t>(height);
    if (rgb_fourcc(fourcc)) {
        image.num_planes = 1;
        image.pitches[0] = width * bytes;
        image.offsets[0] = 0;
        image.data_size = image.pitches[0] * height;
        image.num_palette_entries = 0;
        image.entry_bytes = 0;
        return;
    }
    if (planar_chroma(fourcc)) {
        // Three planes, each at its own pitch: the luma plane, then U and V
        // as one byte per 2x2 block.
        image.num_planes = 3;
        image.pitches[0] = (width + 1) & ~1u;
        image.pitches[1] = image.pitches[2] = image.pitches[0] / 2;
        image.offsets[0] = 0;
        image.offsets[1] = image.pitches[0] * height;
        image.offsets[2] = image.offsets[1] + image.pitches[1] * ((height + 1) / 2);
        image.data_size = image.offsets[2] + image.pitches[2] * ((height + 1) / 2);
        image.num_palette_entries = 0;
        image.entry_bytes = 0;
        return;
    }
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
    const unsigned bytes = bytes_of(fourcc);
    image.format.fourcc = fourcc;
    image.format.byte_order = VA_LSB_FIRST;
    image.format.bits_per_pixel = rgb_fourcc(fourcc) ? 32 : (bytes == 2 ? 24 : 12);
    fill_masks(image.format);
    image.width = static_cast<uint16_t>(width);
    image.height = static_cast<uint16_t>(height);
    image.pitches[0] = layout.stride;
    image.offsets[0] = layout.data_offset;
    image.data_size = layout.size;
    image.num_palette_entries = 0;
    image.entry_bytes = 0;
    if (rgb_fourcc(fourcc)) {
        image.num_planes = 1;
        return;
    }
    image.num_planes = 2;
    image.pitches[1] = layout.stride;
    image.offsets[1] = layout.chroma_offset();
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
    iris::require(compatible(image.format.fourcc, layout.fourcc), "image format does not match the surface",
        VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
    const unsigned bytes = bytes_of(layout.fourcc);
    const unsigned width = std::min<unsigned>(image.width, layout.width);
    const unsigned height = std::min<unsigned>(image.height, layout.height);
    (void)d;
    iris::dma_buf_cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    for (unsigned row = 0; row < height; ++row)
        std::memcpy(destination + image.offsets[0] + static_cast<size_t>(row) * image.pitches[0],
            source + layout.data_offset + static_cast<size_t>(row) * layout.stride, width * bytes);
    if (rgb_fourcc(layout.fourcc)) {
        iris::dma_buf_cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        return;
    }
    const unsigned chroma_width = (width + 1) & ~1u;
    const uint8_t* uv = source + layout.chroma_offset();
    if (planar_chroma(image.format.fourcc)) {
        // De-interleave the surface's (u, v) pairs into two planes.
        for (unsigned row = 0; row < (height + 1) / 2; ++row) {
            const uint8_t* line = uv + static_cast<size_t>(row) * layout.stride;
            uint8_t* u = destination + image.offsets[1] + static_cast<size_t>(row) * image.pitches[1];
            uint8_t* v = destination + image.offsets[2] + static_cast<size_t>(row) * image.pitches[2];
            for (unsigned col = 0; col < chroma_width / 2; ++col) {
                u[col] = line[col * 2];
                v[col] = line[col * 2 + 1];
            }
        }
    } else {
        for (unsigned row = 0; row < (height + 1) / 2; ++row)
            std::memcpy(destination + image.offsets[1] + static_cast<size_t>(row) * image.pitches[1],
                uv + static_cast<size_t>(row) * layout.stride, chroma_width * bytes);
    }
    iris::dma_buf_cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}

} // namespace

VAStatus queryImageFormats(VADriverContextP ctx, VAImageFormat* formats, int* count)
{
    auto& d = data(ctx);
    // count is in/out: on entry it is the caller's list size, and the driver
    // may write at most that many entries. FFmpeg sizes its array from
    // vaMaxNumImageFormats (kMaxImageFormats) and hands that number back, so
    // writing more entries than the driver advertised overflows the client's
    // heap instead of returning an error.
    VAImageFormat list[kMaxImageFormats] = {};
    int n = 0;
    list[n].fourcc = VA_FOURCC_NV12;
    list[n].byte_order = VA_LSB_FIRST;
    list[n].bits_per_pixel = 12;
    ++n;
    // The same picture with the chroma pair split into two planes; the copy
    // paths translate it to and from the surface's NV12.
    list[n].fourcc = VA_FOURCC_I420;
    list[n].byte_order = VA_LSB_FIRST;
    list[n].bits_per_pixel = 12;
    ++n;
    if (d.capabilities.p010) {
        list[n].fourcc = VA_FOURCC_P010;
        list[n].byte_order = VA_LSB_FIRST;
        list[n].bits_per_pixel = 24;
        ++n;
    }
    // The processor's packed overlay sources; a client uploads one and asks
    // for it to be composited onto a YUV frame.
    for (uint32_t fourcc : { VA_FOURCC_BGRA, VA_FOURCC_RGBA }) {
        list[n].fourcc = fourcc;
        list[n].byte_order = VA_LSB_FIRST;
        list[n].bits_per_pixel = 32;
        fill_masks(list[n]);
        ++n;
    }
    if (formats != nullptr && count != nullptr && *count > 0)
        std::copy_n(list, std::min(n, *count), formats);
    if (count != nullptr)
        *count = n;
    return VA_STATUS_SUCCESS;
}

VAStatus createImage(VADriverContextP ctx, VAImageFormat* format, int width, int height, VAImage* image)
{
    return guarded(ctx, "createImage", [&] {
        auto& d = data(ctx);
        iris::require(format
                && (format->fourcc == VA_FOURCC_NV12 || format->fourcc == VA_FOURCC_P010
                    || planar_chroma(format->fourcc) || rgb_fourcc(format->fourcc)),
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
        if (producing(d, *s))
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
            // A surface derived before it ever held a decoded frame is the
            // client's own memory: an upload maps this view and writes the
            // frame into it. Reading it back must not demand a decode
            // completion that will never arrive.
            if (!s->target.frame)
                s->client_pixels = true;
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
        if (producing(d, *s))
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

// Client pixels in, surface memory out. No scaling and no bit-depth
// conversion: the image's format must match the surface's, so what lands is
// exactly the client's bytes. This is the path a client that creates its own
// VAImage takes to fill a surface (FFmpeg falls back to it when the surface
// is not derivable); without it an upload has no route into the driver.
VAStatus putImage(VADriverContextP ctx, VASurfaceID id, VAImageID image_id, int sx, int sy, unsigned sw,
    unsigned sh, int dx, int dy, unsigned dw, unsigned dh)
{
    return guarded(ctx, "putImage", [&] {
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
        iris::require(sw == dw && sh == dh, "vaPutImage cannot scale", VA_STATUS_ERROR_UNIMPLEMENTED);
        iris::require(!b->alias, "vaPutImage reads a client image, not a derived view",
            VA_STATUS_ERROR_INVALID_IMAGE);
        iris::require(sx >= 0 && sy >= 0 && dx >= 0 && dy >= 0 && sw && sh
                && static_cast<uint64_t>(sx) + sw <= image.width
                && static_cast<uint64_t>(sy) + sh <= image.height
                && static_cast<uint64_t>(dx) + dw <= s->width && static_cast<uint64_t>(dy) + dh <= s->height,
            "vaPutImage region outside the image or the surface", VA_STATUS_ERROR_INVALID_PARAMETER);
        if (!rgb_fourcc(image.format.fourcc))
            iris::require(!((sx | sy | dx | dy | sw | sh) & 1u),
                "vaPutImage regions are even for 4:2:0", VA_STATUS_ERROR_INVALID_PARAMETER);
        iris::require(compatible(image.format.fourcc, s->fourcc), "image format does not match the surface",
            VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
        if (producing(d, *s))
            syncSurface(ctx, id);

        uint8_t* destination = nullptr;
        iris::Layout layout;
        int fd = -1;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            if (s->target.frame) {
                destination = static_cast<uint8_t*>(s->target.frame->map());
                layout = s->target.frame->layout();
                fd = s->target.frame->fd();
            } else {
                iris::StableBuffer& stable = ensure_stable(d, *s);
                destination = static_cast<uint8_t*>(stable.map());
                layout = stable.layout();
                fd = stable.fd();
            }
        }
        iris::require(destination != nullptr && compatible(image.format.fourcc, layout.fourcc),
            "surface memory does not match the image", VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);

        const unsigned bytes = bytes_of(layout.fourcc);
        const uint8_t* source = b->data.data();
        iris::dma_buf_cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        for (unsigned row = 0; row < sh; ++row)
            std::memcpy(destination + layout.data_offset + static_cast<size_t>(dy + row) * layout.stride
                    + static_cast<size_t>(dx) * bytes,
                source + image.offsets[0] + static_cast<size_t>(sy + row) * image.pitches[0]
                    + static_cast<size_t>(sx) * bytes,
                static_cast<size_t>(sw) * bytes);
        if (rgb_fourcc(layout.fourcc)) {
            iris::dma_buf_cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            s->client_pixels = true;
            s->failed = false;
            return VA_STATUS_SUCCESS;
        }
        const unsigned chroma_width = sw & ~1u;
        uint8_t* uv = destination + layout.chroma_offset();
        if (planar_chroma(image.format.fourcc)) {
            // Interleave the client's two planes into the surface's pairs.
            for (unsigned row = 0; row < sh / 2; ++row) {
                uint8_t* line = uv + static_cast<size_t>(dy / 2 + row) * layout.stride
                    + static_cast<size_t>(dx);
                const uint8_t* u = source + image.offsets[1]
                    + static_cast<size_t>(sy / 2 + row) * image.pitches[1] + static_cast<size_t>(sx) / 2;
                const uint8_t* v = source + image.offsets[2]
                    + static_cast<size_t>(sy / 2 + row) * image.pitches[2] + static_cast<size_t>(sx) / 2;
                for (unsigned col = 0; col < chroma_width / 2; ++col) {
                    line[col * 2] = u[col];
                    line[col * 2 + 1] = v[col];
                }
            }
        } else {
            for (unsigned row = 0; row < sh / 2; ++row)
                std::memcpy(uv + static_cast<size_t>(dy / 2 + row) * layout.stride
                        + static_cast<size_t>(dx) * bytes,
                    source + image.offsets[1] + static_cast<size_t>(sy / 2 + row) * image.pitches[1]
                        + static_cast<size_t>(sx) * bytes,
                    static_cast<size_t>(sw) * bytes);
        }
        iris::dma_buf_cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        s->client_pixels = true;
        s->failed = false;
        return VA_STATUS_SUCCESS;
    });
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
