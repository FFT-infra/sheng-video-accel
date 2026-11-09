// SPDX-License-Identifier: MIT
//
// Codec translators: VA-API decode parameters in, one access unit the Iris
// stateful firmware accepts out. Pure functions plus a small per-context
// state object; no device, no VA object tables, no ioctl.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include <linux/videodev2.h>
#include <va/va.h>
#include <va/va_dec_av1.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp9.h>
}

#include "../util/fourcc.h"

namespace iris::codec {

// The VA buffers a client renders for one picture, in whatever order they
// arrived. The glue collects them between vaBeginPicture and vaEndPicture and
// hands the whole set to the translator.
struct Picture {
    const void* parameters = nullptr; // VAPictureParameterBuffer<Codec>
    const void* iq_matrix = nullptr; // VAIQMatrixBuffer<Codec>, optional
    std::vector<const void*> slice_parameters; // VASliceParameterBuffer<Codec>, one per slice
    std::vector<std::pair<const uint8_t*, size_t>> slice_data; // in render order
    unsigned surface_width = 0, surface_height = 0;
    // Identity of the target surface (AV1 refresh recovery compares this).
    uint32_t surface_id = 0;
};

// One access unit ready for submission, or a reason it cannot be built.
struct AccessUnit {
    std::vector<uint8_t> bytes;
    bool sequence_start = false; // IDR / key frame: a safe restart point
    // The picture decodes but is never displayed (AV1 show_frame=0, a
    // reference the stream shows later). The node reports it with no output
    // frame, so its target completes without pixels.
    bool hidden = false;
    // Set when the translator holds this picture back (AV1) and nothing is
    // to be submitted yet.
    bool deferred = false;
    // For a translator that holds pictures back: the surface the bytes in
    // this AU belong to, which is not the picture just translated.
    uint32_t released_surface = VA_INVALID_ID;
    // How many CAPTURE completions the firmware produces for this access
    // unit. VP9 packs an invisible frame and a show_existing_frame header
    // into one superframe, and the firmware decodes that as two pictures,
    // so the buffer the first one landed in is still needed by the second.
    unsigned completions = 1;
};

class Translator {
public:
    virtual ~Translator() = default;
    virtual uint32_t v4l2_pixelformat() const = 0;
    // Build the AU for `picture`. Throws iris::Error on unsupported input.
    // `out.deferred` means "come back after the next picture"; `flush()`
    // releases whatever is still held.
    virtual void translate(const Picture& picture, AccessUnit& out) = 0;
    // Release a held picture at end of stream. Returns false when nothing
    // was held.
    virtual bool flush(AccessUnit& out) = 0;
    // The surface the flushed/deferred AU belongs to (AV1 only).
    virtual uint32_t held_surface() const { return VA_INVALID_ID; }
    // True when the held picture's reference-slot flags are fixed by the
    // bitstream itself (AV1: a shown key frame refreshes all eight slots), so
    // it can be submitted now, without its successor.
    virtual bool held_flags_known() const { return false; }
};

std::unique_ptr<Translator> make_h264(VAProfile profile);
std::unique_ptr<Translator> make_hevc(VAProfile profile);
std::unique_ptr<Translator> make_vp9(VAProfile profile);
std::unique_ptr<Translator> make_av1(VAProfile profile);

// Parameter-set builders exposed for the host unit tests.
std::vector<uint8_t> h264_sps(uint8_t profile_idc, unsigned visible_width, unsigned visible_height,
    const VAPictureParameterBufferH264& picture);
std::vector<uint8_t> h264_pps(uint8_t profile_idc, const VAPictureParameterBufferH264& picture,
    const VASliceParameterBufferH264& slice);
std::vector<uint8_t> hevc_vps(VAProfile profile, const VAPictureParameterBufferHEVC& picture);
std::vector<uint8_t> hevc_sps(VAProfile profile, const VAPictureParameterBufferHEVC& picture,
    const VAIQMatrixBufferHEVC* scaling);
std::vector<uint8_t> hevc_pps(const VAPictureParameterBufferHEVC& picture);

} // namespace iris::codec
