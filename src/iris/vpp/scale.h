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

// One NV12 or P010 scale at any ratio, from an optional crop of the source,
// with an optional clockwise rotation. Geometry is in bytes and follows Layout
// (src/iris/slots.h): chroma_offset is the byte offset of the UV plane,
// data_offset is the luma start. src_x/src_y_offset is where the sampled
// rectangle begins inside the source surface; src_width/src_height is that
// rectangle's size, and width/height is the destination. All four are even
// (NV12 stores one chroma pair per 2x2 luma block).
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
    // VA mirroring: VA_MIRROR_HORIZONTAL as bit 0, VA_MIRROR_VERTICAL as bit 1,
    // applied to the rotated rectangle and before the scale, which is the order
    // the VA pipeline buffer documents.
    uint32_t mirror = 0;
    // Bytes per sample on each end: 1 for eight bits, 2 for ten (P010). They
    // are independent, so the kernel converts bit depth as well as scaling —
    // the same exact conversion swscale applies, `code << 2` up and `code >> 2`
    // down, which round-trips a picture whose low two bits are clear.
    uint32_t src_bytes = 1;
    uint32_t dst_bytes = 1;
    // VA_FILTER_INTERPOLATION_* as the kernel sees it. 0 is the point map
    // (swscale's `flags=neighbor`), 1 blends the four samples around the
    // destination sample's centre, 4 is VA's "advanced": the driver picks per
    // axis, which is the area average when the axis shrinks and the cubic when
    // it grows. The unspecified and nearest-neighbour methods are both 0 —
    // the point map is this driver's default. 2 and 3 are the advanced pick's
    // own values and are not reachable from a client's filter_flags.
    uint32_t interpolation = 0;
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
//
// The overlay is sampled from its own rectangle — overlay_x/overlay_row and
// overlay_crop_width/overlay_crop_height inside the overlay surface — with the
// same tap model the scale kernel runs, selected by `interpolation` (0 the
// point map, 1 the four-tap blend, 4 the driver's per-axis pick). The overlay
// surface is a Y'CbCr one, so its alpha is the layer's scalar alone.
struct OverlayJob {
    int main_fd = -1, overlay_fd = -1, dst_fd = -1;
    uint32_t main_size = 0, overlay_size = 0, dst_size = 0;
    uint32_t main_stride = 0, overlay_stride = 0, dst_stride = 0;
    uint32_t main_uv = 0, overlay_uv = 0, dst_uv = 0;
    uint32_t main_y = 0, overlay_y = 0, dst_y = 0;
    uint32_t main_x = 0, main_row = 0;
    uint32_t main_width = 0, main_height = 0;
    uint32_t overlay_width = 0, overlay_height = 0;
    uint32_t overlay_x = 0, overlay_row = 0;
    uint32_t overlay_crop_width = 0, overlay_crop_height = 0;
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint32_t dst_width = 0, dst_height = 0;
    uint32_t bytes = 1;
    // The destination's range: false is limited (16..235 luma, 128 +- 112
    // chroma at eight bits), true is full (0..255). It reaches the kernel
    // because a premultiplied source's neutral code and the encoded output
    // both depend on it.
    bool full_range = false;
    // As in ScaleJob: 0 the point map, 1 the four-tap blend, 4 the driver's
    // own per-axis pick.
    uint32_t interpolation = 0;
    // VA_BLEND_PREMULTIPLIED_ALPHA: the overlay's codes already carry alpha,
    // so only the main surface is scaled by (1 - alpha).
    bool premultiplied = false;
    float alpha = 1.0f;
};

// The same composite with a packed RGB overlay: main and destination are
// still NV12 or P010, the overlay surface is BGRA or RGBA (one 32-bit word per
// pixel), and the kernel converts it to the output's Y'CbCr matrix before
// mixing. The main rectangle is the destination's size, as in OverlayJob, so
// main_width/main_height are implied by dst_width/dst_height. `bgra` names the
// overlay's byte order as VA does (VA_FOURCC_BGRA vs VA_FOURCC_RGBA), and
// `premultiplied` selects out = overlay + (1 - alpha) * main for a source
// whose colours are already multiplied by their alpha.
//
// A packed surface carries an alpha byte, and VA scopes it to
// VA_BLEND_PREMULTIPLIED_ALPHA ("RGBA surfaces only"): naming that flag makes
// the layer's alpha the global one times each sample's own, so a client that
// hands the processor a real alpha channel gets the picture that channel
// describes rather than a flat rectangle. Without it the global alpha alone
// applies and the byte is not read, so an opaque image whose alpha happens to
// be zero is not made invisible. The overlay rectangle, the interpolation
// method and the destination's range are as in OverlayJob.
struct OverlayRgbJob {
    int main_fd = -1, overlay_fd = -1, dst_fd = -1;
    uint32_t main_size = 0, overlay_size = 0, dst_size = 0;
    uint32_t main_stride = 0, overlay_stride = 0, dst_stride = 0;
    uint32_t main_uv = 0, dst_uv = 0;
    uint32_t main_y = 0, overlay_y = 0, dst_y = 0;
    uint32_t main_x = 0, main_row = 0;
    uint32_t overlay_width = 0, overlay_height = 0;
    uint32_t overlay_x = 0, overlay_row = 0;
    uint32_t overlay_crop_width = 0, overlay_crop_height = 0;
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint32_t dst_width = 0, dst_height = 0;
    uint32_t bytes = 1;
    bool bgra = true;
    bool premultiplied = false;
    bool full_range = false;
    uint32_t interpolation = 0;
    // Matrix for the RGB-to-Y'CbCr step, from the picture's colour standard:
    // 0 BT.709 (also what an unnamed standard means), 1 BT.601, 2 BT.2020.
    uint32_t standard = 0;
    float alpha = 1.0f;
};

// True when this driver was built with the Vulkan VPP backend. Capability
// queries use this instead of touching Vulkan: creating a VkInstance during
// Chromium's GPU-sandbox init is not allowed (docs/architecture.md §7).
bool compiled_in();

// Dispatch the pinned kernel (one per destination sample width) over
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

// Dispatch the packed-RGB composite (overlay_rgb.spv). Same failure contract.
bool overlay_rgb(const OverlayRgbJob& job);

} // namespace vpp
} // namespace iris
