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

// One nearest-neighbour NV12 or P010 scale at any ratio, from an optional
// crop of the source, with an optional clockwise rotation. Geometry is in
// bytes and follows Layout (src/iris/slots.h): chroma_offset is the byte
// offset of the UV plane, data_offset is the luma start. src_x/src_y_offset is
// where the sampled rectangle begins inside the source surface;
// src_width/src_height is that rectangle's size, and width/height is the
// destination. All four are even (NV12 stores one chroma pair per 2x2 luma
// block).
struct ScaleJob {
    int src_fd = -1, dst_fd = -1;
    uint32_t src_size = 0, dst_size = 0;
    uint32_t src_stride = 0, src_uv = 0, dst_stride = 0, dst_uv = 0;
    uint32_t src_y = 0, dst_y = 0;
    uint32_t src_x = 0, src_y_offset = 0;
    uint32_t src_width = 0, src_height = 0;
    uint32_t width = 0, height = 0;
    // Clockwise quarter turns, as VA_ROTATION_NONE/90/180/270 = 0/1/2/3. The
    // rotation happens before the scale, so the sampled rectangle transposes
    // for a quarter turn and width/height may be its transpose or any other
    // even size.
    uint32_t rotation = 0;
    // Bytes per sample on both ends: 1 for NV12, 2 for P010. Source and
    // destination must agree; the kernels do not convert bit depth.
    uint32_t bytes = 1;
};

// One HDR10 P010 picture to SDR NV12: the source is BT.2020 primaries with the
// PQ transfer, limited range; the destination is BT.709 with the BT.709 OETF,
// limited range. The kernel applies the PQ EOTF, the ITU-R BT.2390 EETF toward
// a 100 cd/m² peak as one gain on the largest channel, the linear BT.2020 to
// BT.709 matrix, and the BT.709 OETF. source_peak_nits is the HDR10 mastering
// display peak; width and height are visible and even.
struct ToneMapJob {
    int src_fd = -1, dst_fd = -1;
    uint32_t src_size = 0, dst_size = 0;
    uint32_t src_stride = 0, src_uv = 0, dst_stride = 0, dst_uv = 0;
    uint32_t src_y = 0, dst_y = 0;
    uint32_t width = 0, height = 0;
    float source_peak_nits = 1000.0f;
};

// Global-alpha compositing: the main surface's rectangle is copied into the
// output at its own scale and the overlay surface is scaled into
// x/y/width/height of the output, where every pixel becomes
// alpha * overlay + (1 - alpha) * main. All three surfaces share one sample
// width (bytes); main_width/main_height is the copied rectangle and equals the
// destination, main_x/main_row is where it starts inside the main surface, and
// the overlay rectangle is even on both axes so no chroma cell is half
// covered.
struct OverlayJob {
    int main_fd = -1, overlay_fd = -1, dst_fd = -1;
    uint32_t main_size = 0, overlay_size = 0, dst_size = 0;
    uint32_t main_stride = 0, overlay_stride = 0, dst_stride = 0;
    uint32_t main_uv = 0, overlay_uv = 0, dst_uv = 0;
    uint32_t main_y = 0, overlay_y = 0, dst_y = 0;
    uint32_t main_x = 0, main_row = 0;
    uint32_t main_width = 0, main_height = 0;
    uint32_t overlay_width = 0, overlay_height = 0;
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint32_t dst_width = 0, dst_height = 0;
    uint32_t bytes = 1;
    float alpha = 1.0f;
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

// Dispatch the tone-mapping kernel (tonemap.spv) over the two imported
// DMA-BUFs. Same failure contract as the scale: false means the VA layer must
// fail the picture rather than publish partial pixels.
bool tonemap_hdr10_to_sdr(const ToneMapJob& job);

// Dispatch the compositing kernel (overlay.spv) over the three imported
// DMA-BUFs. Same failure contract again.
bool overlay(const OverlayJob& job);

} // namespace vpp
} // namespace iris
