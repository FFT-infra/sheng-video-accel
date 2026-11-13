// SPDX-License-Identifier: MIT
//
// The optional Vulkan video processor. libva does not give a driver an
// installation path it can read at runtime, so the pinned SPIR-V module is
// embedded in the .so; the Vulkan headers stay out of this header so the
// driver still builds on hosts without a Vulkan SDK (CI). The only public
// surface is a plain job struct and two functions.

#pragma once

#include <cstdint>

namespace iris {
namespace vpp {

// One nearest-neighbour NV12 scale at any ratio. Geometry is in bytes and
// follows Layout (src/iris/slots.h): chroma_offset is the byte offset of the
// UV plane, data_offset is the luma start. src_width/src_height and
// width/height are the visible source and destination sizes, both even (NV12
// stores one chroma pair per 2x2 luma block).
struct ScaleJob {
    int src_fd = -1, dst_fd = -1;
    uint32_t src_size = 0, dst_size = 0;
    uint32_t src_stride = 0, src_uv = 0, dst_stride = 0, dst_uv = 0;
    uint32_t src_y = 0, dst_y = 0;
    uint32_t src_width = 0, src_height = 0;
    uint32_t width = 0, height = 0;
};

// True when this driver was built with the Vulkan VPP backend. Capability
// queries use this instead of touching Vulkan: creating a VkInstance during
// Chromium's GPU-sandbox init is not allowed (docs/architecture.md §7).
bool compiled_in();

// Dispatch the pinned NV12 kernel over the two imported DMA-BUFs. Returns
// false when the backend is unavailable or the device rejected the job; the
// VA layer must then fail the picture and never publish partial pixels.
bool scale_nv12(const ScaleJob& job);

} // namespace vpp
} // namespace iris
