// SPDX-License-Identifier: MIT
//
// Encode translators: the VA encode buffers a client rendered for one
// picture in, the parameters the Iris encoder accepts out. Pure functions
// plus a small per-context object; no device, no VA object tables, no ioctl.

#pragma once

#include "annexb.h"

#include "../util/encode-params.h"

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
    unsigned surface_width = 0, surface_height = 0;
};

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

class Encoder {
public:
    virtual ~Encoder() = default;
    // V4L2_PIX_FMT_H264 or V4L2_PIX_FMT_HEVC.
    virtual uint32_t v4l2_pixelformat() const = 0;
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
