// SPDX-License-Identifier: MIT
//
// Encode translators: the VA encode buffers a client rendered for one
// picture in, the parameters the Iris encoder accepts out. Pure functions
// plus a small per-context object; no device, no VA object tables, no ioctl.

#pragma once

#include "annexb.h"

#include "../util/encode-params.h"
#include "../util/error.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include <linux/videodev2.h>
#include <va/va.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
}

namespace iris::codec {

// Everything the glue collected between vaBeginPicture and vaEndPicture for
// an encode context. The codec tag is the object itself, so the pointers are
// cast by the Encoder that produced them.
struct EncodePicture {
    const void* sequence = nullptr; // VAEncSequenceParameterBuffer<Codec>
    const void* picture = nullptr; // VAEncPictureParameterBuffer<Codec>
    std::vector<const void*> misc; // VAEncMiscParameterBuffer<...>
    // VAEncSliceParameterBuffer<Codec>. The firmware owns the slice layout,
    // but CQP takes the picture QP from the first slice's delta.
    std::vector<const void*> slices;
    // Raw packed headers the client sent with this picture, in render order.
    // Each is one Annex-B NAL as the client wrote it, start code included.
    std::vector<std::vector<uint8_t>> packed_headers;
    // The client's ROI regions (VAEncROI), copied out of the misc buffer that
    // named them. The array that buffer points at is client memory, valid
    // only until vaEndPicture, so the regions are copied when the picture is
    // rendered and the buffer itself is not read again.
    std::vector<VAEncROI> roi;
    unsigned surface_width = 0, surface_height = 0;
};

// The delta-QP range the encoder node's ROI control takes (min -31, max 30
// in packaging/kernel/0017-*). VA's roi_value is an int8_t and a client may
// name anything in that range, so a value outside the control's is clamped
// rather than refused: the region still means "cheaper" or "dearer".
constexpr int32_t kRoiMinDeltaQp = -31;
constexpr int32_t kRoiMaxDeltaQp = 30;

// Map the client's ROI rectangles onto the encoder's per-macroblock delta-QP
// grid, one cell per LCU of `lcu` pixels (16 for H.264, 32 for HEVC) over the
// surface's visible size. That is the grid the kernel builds from its own
// unaligned OUTPUT format, so the two agree. A cell no region covers is 0, a
// rectangle is clipped to the picture, and when two regions overlap the one
// the client listed first wins — VA gives overlapping regions no meaning and
// Intel's driver resolves them the same way. Returns an empty map when the
// picture carries no regions.
inline std::vector<int32_t> roi_map_of(const EncodePicture& picture, unsigned lcu)
{
    if (picture.roi.empty() || !picture.surface_width || !picture.surface_height || !lcu)
        return {};
    const unsigned columns = (picture.surface_width + lcu - 1) / lcu;
    const unsigned rows = (picture.surface_height + lcu - 1) / lcu;
    std::vector<int32_t> map(static_cast<size_t>(columns) * rows, 0);
    std::vector<char> claimed(map.size(), 0);
    for (const VAEncROI& region : picture.roi) {
        const VARectangle& rect = region.roi_rectangle;
        const int32_t left = std::max<int32_t>(0, rect.x);
        const int32_t top = std::max<int32_t>(0, rect.y);
        const int32_t right = std::min<int32_t>(picture.surface_width, static_cast<int32_t>(rect.x) + rect.width);
        const int32_t bottom = std::min<int32_t>(picture.surface_height, static_cast<int32_t>(rect.y) + rect.height);
        if (right <= left || bottom <= top)
            continue;
        const int32_t delta = std::clamp<int32_t>(region.roi_value, kRoiMinDeltaQp, kRoiMaxDeltaQp);
        const unsigned first_column = static_cast<unsigned>(left) / lcu;
        const unsigned first_row = static_cast<unsigned>(top) / lcu;
        const unsigned last_column = (static_cast<unsigned>(right) + lcu - 1) / lcu;
        const unsigned last_row = (static_cast<unsigned>(bottom) + lcu - 1) / lcu;
        for (unsigned row = first_row; row < last_row; ++row) {
            for (unsigned column = first_column; column < last_column; ++column) {
                const size_t cell = static_cast<size_t>(row) * columns + column;
                if (claimed[cell])
                    continue;
                claimed[cell] = 1;
                map[cell] = delta;
            }
        }
    }
    return map;
}

// Payload of the first misc buffer of `type`, or nullptr. Every misc buffer
// is a VAEncMiscParameterBuffer header followed by the typed payload.
inline const void* find_misc(const std::vector<const void*>& misc, VAEncMiscParameterType type)
{
    for (const void* raw : misc) {
        const auto* buffer = static_cast<const VAEncMiscParameterBuffer*>(raw);
        if (buffer && buffer->type == type)
            return buffer->data;
    }
    return nullptr;
}

// The temporal layer count from a VAEncMiscParameterTemporalLayerStructure,
// or `current` when this picture carries none. The structure belongs to a
// coded sequence: a client sends it with the first picture, so the count is
// remembered and repeated like the rate-control parameters. The firmware
// derives layer membership from the count alone, so a structure whose
// pattern cannot follow that hierarchy is refused instead of silently
// coded with a different membership.
inline unsigned temporal_layers_of(const std::vector<const void*>& misc, unsigned current)
{
    const auto* structure = static_cast<const VAEncMiscParameterTemporalLayerStructure*>(
        find_misc(misc, VAEncMiscParameterTypeTemporalLayerStructure));
    if (!structure)
        return current;
    require(structure->number_of_layers >= 1 && structure->number_of_layers <= 6,
        "temporal layers must be between 1 and 6", VA_STATUS_ERROR_INVALID_PARAMETER);
    require(structure->periodicity >= 1 && structure->periodicity <= 32,
        "temporal layer periodicity must be between 1 and 32", VA_STATUS_ERROR_INVALID_PARAMETER);
    for (uint32_t i = 0; i < structure->periodicity; ++i)
        require(structure->layer_id[i] < structure->number_of_layers,
            "a temporal layer id exceeds the layer count", VA_STATUS_ERROR_INVALID_PARAMETER);
    // The firmware derives membership from the count, and the advertised
    // attribute says per-layer bitrate control is off, so the client's
    // pattern is advisory here; the count is what is honoured.
    return structure->number_of_layers;
}

class Encoder {
public:
    virtual ~Encoder() = default;
    // V4L2_PIX_FMT_H264 or V4L2_PIX_FMT_HEVC.
    virtual uint32_t v4l2_pixelformat() const = 0;
    // The visible picture size the client's sequence parameters ask for, when
    // the codec carries one there. Returns false when it does not; the caller
    // then codes the context geometry.
    virtual bool coded_size(const EncodePicture& picture, unsigned& width, unsigned& height) const
    {
        (void)picture;
        (void)width;
        (void)height;
        return false;
    }
    // The coded buffer the client associated with this picture, named in its
    // picture parameter buffer. VA_INVALID_ID when the client sent none.
    virtual VABufferID coded_buf(const EncodePicture& picture) const = 0;
    // Throws iris::Error for a picture the firmware cannot encode.
    virtual EncodeParams translate(const EncodePicture& picture) = 0;
    // SPS/PPS/SEI to put in front of `access_unit` when the client asked for
    // an IDR and the firmware wrote a bare one. The default keeps every
    // context free of codec state and returns the unit unchanged; the
    // codecs that need it override this.
    virtual std::vector<uint8_t> prepend_parameter_sets(const std::vector<uint8_t>& access_unit,
        std::vector<NalUnit>& last)
    {
        (void)last;
        return access_unit;
    }
};

std::unique_ptr<Encoder> make_h264_encoder(VAProfile profile, RateMode rate_mode);
std::unique_ptr<Encoder> make_hevc_encoder(VAProfile profile, RateMode rate_mode);

} // namespace iris::codec
