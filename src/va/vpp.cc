// SPDX-License-Identifier: MIT
//
// The three queries on the VPP vtable. They describe what the compute
// backend runs — one nearest-neighbour scale at the requested ratio from an
// optional crop, in NV12 or P010, after an optional clockwise quarter turn,
// the HDR10 tone mapper as its only filter, and a second surface composited
// with a global alpha — and nothing here creates a VkInstance or opens a
// node, so a client can query them during GPU sandbox initialisation.

#include "driver.h"

#include "../util/error.h"

#include <algorithm>
#include <cstring>

namespace va {

namespace {

// Advertised on both ends. FFmpeg 8.x looks for Explicit first and then
// carries the frame's own colour properties instead of mapping them, so the
// processor never has to guess a standard; the fixed ones are there for a
// client that predates explicit properties.
VAProcColorStandardType kStandards[] = { VAProcColorStandardNone, VAProcColorStandardBT601,
    VAProcColorStandardBT709, VAProcColorStandardBT2020, VAProcColorStandardExplicit };

// One kernel per sample width, chosen from the surfaces the picture names.
// P010 is listed only when the decoder node reported 10-bit capture formats.
uint32_t kPixelFormatsNv12[] = { VA_FOURCC_NV12 };
uint32_t kPixelFormatsNv12P010[] = { VA_FOURCC_NV12, VA_FOURCC_P010 };

// The tone mapper reads a 10-bit HDR10 surface, so it is offered only when
// the node reported a 10-bit capture format.
constexpr unsigned kHdrFilters = 1;

} // namespace

VAStatus queryVideoProcFilters(VADriverContextP ctx, VAContextID id, VAProcFilterType* filters, unsigned* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    (void)context(d, id); // invalid contexts are an error even for an empty answer
    iris::require(count != nullptr, "null filter count", VA_STATUS_ERROR_INVALID_PARAMETER);
    // One filter: the HDR10 tone mapper.
    const unsigned n = d.capabilities.p010 ? kHdrFilters : 0;
    if (filters == nullptr) {
        *count = n;
        return VA_STATUS_SUCCESS;
    }
    if (*count < n) {
        *count = n;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    if (n)
        filters[0] = VAProcFilterHighDynamicRangeToneMapping;
    *count = n;
    return VA_STATUS_SUCCESS;
}

VAStatus queryVideoProcFilterCaps(VADriverContextP ctx, VAContextID id, VAProcFilterType type, void* caps,
    unsigned* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    (void)context(d, id);
    iris::require(count != nullptr, "null filter caps count", VA_STATUS_ERROR_INVALID_PARAMETER);
    // Only HDR10 to SDR: the kernel's curve is the BT.2390 EETF toward a
    // 100 cd/m2 display, and it has no way to produce HDR output metadata.
    if (type != VAProcFilterHighDynamicRangeToneMapping || !d.capabilities.p010) {
        *count = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
    }
    if (caps == nullptr) {
        *count = 1;
        return VA_STATUS_SUCCESS;
    }
    if (*count < 1) {
        *count = 1;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    VAProcFilterCapHighDynamicRange hdr = {};
    hdr.metadata_type = VAProcHighDynamicRangeMetadataHDR10;
    hdr.caps_flag = VA_TONE_MAPPING_HDR_TO_SDR;
    std::memcpy(caps, &hdr, sizeof(hdr));
    *count = 1;
    return VA_STATUS_SUCCESS;
}

VAStatus queryVideoProcPipelineCaps(VADriverContextP ctx, VAContextID id, VABufferID* filters, unsigned count,
    VAProcPipelineCaps* caps)
{
    (void)filters;
    (void)count;
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto c = context(d, id);
    iris::require(c->video_proc(), "pipeline caps on a non-VideoProc context",
        VA_STATUS_ERROR_INVALID_CONTEXT);
    iris::require(caps != nullptr, "null pipeline caps", VA_STATUS_ERROR_INVALID_PARAMETER);
    // FFmpeg passes an uninitialised struct, so every field is written here;
    // the two colour-standard arrays must point at storage that outlives the
    // call, which is why they are static.
    *caps = {};
    caps->pipeline_flags = 0;
    caps->filter_flags = 0;
    caps->num_forward_references = 0;
    caps->num_backward_references = 0;
    caps->input_color_standards = kStandards;
    caps->num_input_color_standards = static_cast<uint32_t>(sizeof(kStandards) / sizeof(kStandards[0]));
    caps->output_color_standards = kStandards;
    caps->num_output_color_standards = static_cast<uint32_t>(sizeof(kStandards) / sizeof(kStandards[0]));
    // All three clockwise quarter turns; mirroring is not implemented.
    caps->rotation_flags = (1u << VA_ROTATION_NONE) | (1u << VA_ROTATION_90) | (1u << VA_ROTATION_180)
        | (1u << VA_ROTATION_270);
    // What `overlay_vaapi` refuses to run without. Per-pixel (premultiplied)
    // alpha and luma keying are separate kernels that do not exist here, so
    // only the global alpha is advertised.
    caps->blend_flags = VA_BLEND_GLOBAL_ALPHA;
    caps->mirror_flags = 0;
    caps->num_additional_outputs = 0;
    const bool p010 = d.capabilities.p010;
    caps->num_input_pixel_formats = p010 ? 2 : 1;
    caps->input_pixel_format = p010 ? kPixelFormatsNv12P010 : kPixelFormatsNv12;
    caps->num_output_pixel_formats = p010 ? 2 : 1;
    caps->output_pixel_format = p010 ? kPixelFormatsNv12P010 : kPixelFormatsNv12;
    caps->min_input_width = std::max(kVppMinGeometry, d.capabilities.min_width);
    caps->min_input_height = std::max(kVppMinGeometry, d.capabilities.min_height);
    caps->max_input_width = d.capabilities.max_width;
    caps->max_input_height = d.capabilities.max_height;
    caps->min_output_width = kVppMinGeometry;
    caps->min_output_height = kVppMinGeometry;
    caps->max_output_width = d.capabilities.max_width;
    caps->max_output_height = d.capabilities.max_height;
    return VA_STATUS_SUCCESS;
}

} // namespace va
