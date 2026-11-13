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

// One nearest-neighbour NV12 scale at any ratio, from an optional crop of the
// source. Geometry is in bytes and follows Layout (src/iris/slots.h):
// chroma_offset is the byte offset of the UV plane, data_offset is the luma
// start. src_x/src_y_offset is where the sampled rectangle begins inside the
// source surface; src_width/src_height is that rectangle's size, and
// width/height is the destination. All four are even (NV12 stores one chroma
// pair per 2x2 luma block).
struct ScaleJob {
    int src_fd = -1, dst_fd = -1;
    uint32_t src_size = 0, dst_size = 0;
    uint32_t src_stride = 0, src_uv = 0, dst_stride = 0, dst_uv = 0;
    uint32_t src_y = 0, dst_y = 0;
    uint32_t src_x = 0, src_y_offset = 0;
    uint32_t src_width = 0, src_height = 0;
    uint32_t width = 0, height = 0;
    // Bytes per sample on both ends: 1 for NV12, 2 for P010. Source and
    // destination must agree; the kernels do not convert bit depth.
    uint32_t bytes = 1;
};

// True when this driver was built with the Vulkan VPP backend. Capability
// queries use this instead of touching Vulkan: creating a VkInstance during
// Chromium's GPU-sandbox init is not allowed (docs/architecture.md §7).
bool compiled_in();

// Dispatch the pinned kernel (NV12 or P010, selected by ScaleJob::bytes) over
// the two imported DMA-BUFs. Returns false when the backend is unavailable or
// the device rejected the job; the VA layer must then fail the picture and
// never publish partial pixels.
bool scale_nv12_or_p010(const ScaleJob& job);

} // namespace vpp
} // namespace iris
