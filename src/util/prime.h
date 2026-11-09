// SPDX-License-Identifier: MIT
//
// Client-supplied surface memory: a DRM_PRIME_2 descriptor or a PRIME_1
// buffer table, checked against the layout the VPU writes. Device-free, so
// the rules can be tested without a node (docs/architecture.md §3.3).
//
// The decoder writes the CAPTURE layout and nothing else: luma at the start
// of the object, chroma exactly after the aligned luma rows, one pitch for
// both planes. A client buffer with any other geometry would be decoded into
// the wrong pixels instead of failing, so it is refused.

#pragma once

#include <cstdint>

extern "C" {
#include <va/va.h>
#include <va/va_drmcommon.h>
}

namespace iris {

// Byte geometry of one NV12/P010 buffer in the Iris CAPTURE layout.
struct PlaneLayout {
    unsigned stride = 0; // bytes per row, both planes
    unsigned storage_height = 0; // luma rows before the chroma plane
    unsigned chroma_offset = 0;
    unsigned size = 0; // both planes, without the 4 KiB rounding
};

// The CAPTURE layout for `fourcc` at `width` x `height`; the one definition
// of it (`iris_yuv_buffer_size_nv12`/`_p010`). Throws
// VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT for any other fourcc.
PlaneLayout plane_layout(uint32_t fourcc, unsigned width, unsigned height);

// Both return the layout the client's memory must have, or throw
// VA_STATUS_ERROR_INVALID_PARAMETER. `allocation` is the DMA-BUF's real
// length from lseek, or 0 when the kernel does not report one.
PlaneLayout validate_prime2(const VADRMPRIMESurfaceDescriptor& descriptor, unsigned width, unsigned height,
    uint32_t fourcc, uint64_t allocation);
PlaneLayout validate_prime1(const VASurfaceAttribExternalBuffers& buffers, unsigned width, unsigned height,
    uint32_t fourcc, uint64_t allocation);

} // namespace iris
