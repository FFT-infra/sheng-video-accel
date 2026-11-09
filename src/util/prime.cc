// SPDX-License-Identifier: MIT

#include "prime.h"

#include "error.h"

extern "C" {
#include <libdrm/drm_fourcc.h>
}

namespace iris {

namespace {

struct Shape {
    uint32_t composed; // one layer carrying both planes (DRM_PRIME_2)
    uint32_t luma; // separate-plane layers (DRM_PRIME_2)
    uint32_t chroma;
    unsigned bytes_per_sample;
    unsigned pitch_alignment;
};

Shape shape(uint32_t fourcc)
{
    if (fourcc == VA_FOURCC_NV12)
        return { DRM_FORMAT_NV12, DRM_FORMAT_R8, DRM_FORMAT_GR88, 1, 128 };
    require(fourcc == VA_FOURCC_P010, "imported surfaces are NV12 or P010", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    return { DRM_FORMAT_P010, DRM_FORMAT_R16, DRM_FORMAT_GR1616, 2, 256 };
}

// The pitch, row count and plane bytes both codecs' CAPTURE buffers use; the
// checks every import shares. `luma` and `chroma` are the client's plane
// pitches and byte offsets.
void check_planes(const PlaneLayout& layout, const unsigned (&pitches)[2], const unsigned (&offsets)[2])
{
    require(pitches[0] == layout.stride && pitches[1] == layout.stride && offsets[0] == 0
            && offsets[1] == layout.chroma_offset,
        "the imported buffer must use the CAPTURE layout: luma at offset 0, chroma after the aligned rows, one pitch",
        VA_STATUS_ERROR_INVALID_PARAMETER);
}

void check_size(const PlaneLayout& layout, uint64_t claimed, uint64_t allocation)
{
    require(claimed >= layout.size, "the imported descriptor is smaller than its planes",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    require(!allocation || (allocation >= layout.size && allocation >= claimed),
        "the imported DMA-BUF is smaller than its descriptor claims", VA_STATUS_ERROR_INVALID_PARAMETER);
    require(layout.size <= 0xffffffffu, "imported surface is too large", VA_STATUS_ERROR_INVALID_PARAMETER);
}

} // namespace

PlaneLayout plane_layout(uint32_t fourcc, unsigned width, unsigned height)
{
    // The Iris CAPTURE layout (iris_buffer.c, iris_yuv_buffer_size_nv12 /
    // _p010): luma pitch aligned to 128 (256 for P010), luma scanlines to
    // 32, chroma pitch equal to luma, chroma scanlines to 16, chroma
    // directly after the luma plane.
    const Shape s = shape(fourcc);
    PlaneLayout layout;
    layout.stride = (width * s.bytes_per_sample + s.pitch_alignment - 1) & ~(s.pitch_alignment - 1);
    layout.storage_height = (height + 31) & ~31u;
    layout.chroma_offset = layout.stride * layout.storage_height;
    const unsigned chroma_rows = (((height + 1) / 2) + 15) & ~15u;
    const uint64_t total = static_cast<uint64_t>(layout.chroma_offset) + static_cast<uint64_t>(layout.stride) * chroma_rows;
    require(width && height && total <= 0xffffffffu, "surface geometry", VA_STATUS_ERROR_INVALID_PARAMETER);
    layout.size = static_cast<unsigned>(total);
    return layout;
}

PlaneLayout validate_prime2(const VADRMPRIMESurfaceDescriptor& descriptor, unsigned width, unsigned height,
    uint32_t fourcc, uint64_t allocation)
{
    const Shape s = shape(fourcc);
    require(descriptor.fourcc == fourcc && descriptor.width == width && descriptor.height == height,
        "the imported descriptor does not match the requested format and size", VA_STATUS_ERROR_INVALID_PARAMETER);
    require(descriptor.num_objects == 1 && descriptor.objects[0].fd >= 0,
        "import requires exactly one DMA-BUF object", VA_STATUS_ERROR_INVALID_PARAMETER);
    require(descriptor.objects[0].drm_format_modifier == DRM_FORMAT_MOD_LINEAR,
        "import requires a linear DMA-BUF", VA_STATUS_ERROR_INVALID_PARAMETER);

    unsigned pitches[2] = {};
    unsigned offsets[2] = {};
    if (descriptor.num_layers == 1) {
        const auto& layer = descriptor.layers[0];
        require(layer.drm_format == s.composed && layer.num_planes == 2 && !layer.object_index[0]
                && !layer.object_index[1],
            "the imported layer must carry both planes of one NV12/P010 object", VA_STATUS_ERROR_INVALID_PARAMETER);
        for (unsigned i = 0; i < 2; ++i) {
            pitches[i] = layer.pitch[i];
            offsets[i] = layer.offset[i];
        }
    } else {
        require(descriptor.num_layers == 2 && descriptor.layers[0].drm_format == s.luma
                && descriptor.layers[1].drm_format == s.chroma,
            "the imported layers must be the luma and chroma planes of one object",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        for (unsigned i = 0; i < 2; ++i) {
            require(descriptor.layers[i].num_planes == 1 && descriptor.layers[i].object_index[0] == 0,
                "each imported layer must name the single object", VA_STATUS_ERROR_INVALID_PARAMETER);
            pitches[i] = descriptor.layers[i].pitch[0];
            offsets[i] = descriptor.layers[i].offset[0];
        }
    }

    const PlaneLayout layout = plane_layout(fourcc, width, height);
    check_planes(layout, pitches, offsets);
    check_size(layout, descriptor.objects[0].size, allocation);
    return layout;
}

PlaneLayout validate_prime1(const VASurfaceAttribExternalBuffers& buffers, unsigned width, unsigned height,
    uint32_t fourcc, uint64_t allocation)
{
    require(buffers.pixel_format == fourcc && buffers.width == width && buffers.height == height,
        "the imported descriptor does not match the requested format and size", VA_STATUS_ERROR_INVALID_PARAMETER);
    require(buffers.num_buffers == 1 && buffers.buffers && buffers.buffers[0],
        "import requires exactly one DMA-BUF", VA_STATUS_ERROR_INVALID_PARAMETER);
    require(!(buffers.flags & VA_SURFACE_EXTBUF_DESC_ENABLE_TILING),
        "import requires a linear DMA-BUF", VA_STATUS_ERROR_INVALID_PARAMETER);
    // PRIME_1 names the luma plane and, when the client fills the second
    // entry, the chroma plane; both have to be where the CAPTURE layout puts
    // them.
    const PlaneLayout layout = plane_layout(fourcc, width, height);
    require(buffers.pitches[0] == layout.stride && buffers.offsets[0] == 0
            && (buffers.num_planes < 2 || buffers.offsets[1] == layout.chroma_offset),
        "the imported buffer must use the CAPTURE layout: luma at offset 0, chroma after the aligned rows, one pitch",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    check_size(layout, buffers.data_size, allocation);
    return layout;
}

} // namespace iris
