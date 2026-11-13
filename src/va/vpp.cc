// SPDX-License-Identifier: MIT
//
// The three queries on the VPP vtable. They describe what the compute
// backend runs — one NV12 nearest-neighbour scale at the requested ratio, no
// filters, no rotation, no blending — and nothing here creates a VkInstance or
// opens a node, so a client can query them during GPU sandbox initialisation.

#include "driver.h"

#include "../util/error.h"

#include <algorithm>

namespace va {

namespace {

// Advertised on both ends. FFmpeg 8.x looks for Explicit first and then
// carries the frame's own colour properties instead of mapping them, so the
// processor never has to guess a standard; the fixed ones are there for a
// client that predates explicit properties.
VAProcColorStandardType kStandards[] = { VAProcColorStandardNone, VAProcColorStandardBT601,
    VAProcColorStandardBT709, VAProcColorStandardBT2020, VAProcColorStandardExplicit };

// The kernel reads and writes NV12. P010 has no path in the compute shader.
uint32_t kPixelFormats[] = { VA_FOURCC_NV12 };

} // namespace

VAStatus queryVideoProcFilters(VADriverContextP ctx, VAContextID id, VAProcFilterType* filters, unsigned* count)
{
    (void)filters;
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    (void)context(d, id); // invalid contexts are an error even for an empty answer
    // No filter is implemented, so the chain is always empty.
    *count = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus queryVideoProcFilterCaps(VADriverContextP ctx, VAContextID id, VAProcFilterType, void* caps, unsigned* count)
{
    (void)caps;
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    (void)context(d, id);
    *count = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
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
    // Only the identity rotation and mirror are implemented.
    caps->rotation_flags = 1u << VA_ROTATION_NONE;
    caps->blend_flags = 0;
    caps->mirror_flags = 0;
    caps->num_additional_outputs = 0;
    caps->num_input_pixel_formats = 1;
    caps->input_pixel_format = kPixelFormats;
    caps->num_output_pixel_formats = 1;
    caps->output_pixel_format = kPixelFormats;
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
