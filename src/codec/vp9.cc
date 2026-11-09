// SPDX-License-Identifier: MIT
//
// VP9: VA hands the driver the complete compressed frame, which is exactly
// what the stateful node wants. The one translation is for invisible
// (alt-ref) frames: the firmware suppresses their output, while VA expects
// a completed surface, so the frame is wrapped in a superframe with a
// show_existing_frame header that displays the slot it just refreshed.

#include "translator.h"

#include "../util/error.h"

namespace iris::codec {

namespace {

class VP9 final : public Translator {
public:
    explicit VP9(VAProfile profile)
        : expected_profile_(profile == VAProfileVP9Profile2 ? 2 : 0)
    {
        require(profile == VAProfileVP9Profile0 || profile == VAProfileVP9Profile2, "VP9 profile not supported",
            VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    }
    uint32_t v4l2_pixelformat() const override { return v4l2_fourcc('V', 'P', '9', '0'); }

    void translate(const Picture& picture, AccessUnit& out) override
    {
        require(picture.parameters && picture.slice_data.size() == 1, "VP9 requires one complete frame per picture",
            VA_STATUS_ERROR_INVALID_BUFFER);
        const auto& p = *static_cast<const VADecPictureParameterBufferVP9*>(picture.parameters);
        require(p.profile == expected_profile_ && p.bit_depth == (expected_profile_ ? 10 : 8)
                && p.pic_fields.bits.subsampling_x && p.pic_fields.bits.subsampling_y,
            "VP9 supports Profile 0 8-bit and Profile 2 10-bit 4:2:0", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        const auto [data, size] = picture.slice_data.front();
        require(size >= 3 && p.frame_header_length_in_bytes && p.first_partition_size
                && static_cast<size_t>(p.frame_header_length_in_bytes) + p.first_partition_size <= size,
            "VP9 frame must include its uncompressed and compressed headers", VA_STATUS_ERROR_INVALID_BUFFER);

        size_t pos = 0;
        auto bit = [&]() {
            require(pos < size * 8, "truncated VP9 header", VA_STATUS_ERROR_INVALID_BUFFER);
            const size_t n = pos++;
            return (data[n / 8] >> (7 - n % 8)) & 1u;
        };
        auto bits = [&](unsigned count) {
            unsigned value = 0;
            while (count--)
                value = (value << 1) | bit();
            return value;
        };
        const unsigned marker = bits(2);
        unsigned raw_profile = bit();
        raw_profile |= bit() << 1;
        require(marker == 2 && raw_profile == expected_profile_, "VP9 raw header/profile mismatch",
            VA_STATUS_ERROR_INVALID_BUFFER);
        if (raw_profile == 3)
            bit(); // reserved
        require(!bit(), "VP9 show_existing_frame must reuse the existing VA surface", VA_STATUS_ERROR_UNIMPLEMENTED);
        const unsigned frame_type = bit(), show_frame = bit(), error_resilient = bit();
        require(frame_type == p.pic_fields.bits.frame_type && show_frame == p.pic_fields.bits.show_frame
                && error_resilient == p.pic_fields.bits.error_resilient_mode,
            "VP9 raw header does not match VA flags", VA_STATUS_ERROR_INVALID_BUFFER);

        out.bytes.assign(data, data + size);
        out.deferred = false;
        out.sequence_start = frame_type == 0;
        out.completions = 1;
        if (show_frame)
            return;

        // Invisible frame: find the first slot it refreshes and append a
        // show_existing_frame header for it inside a superframe.
        unsigned refresh = 0xff;
        if (frame_type) {
            const unsigned intra_only = bit();
            if (!error_resilient)
                bits(2); // reset_frame_context
            if (intra_only) {
                require(bits(24) == 0x498342, "VP9 intra sync code", VA_STATUS_ERROR_INVALID_BUFFER);
                if (raw_profile) {
                    require(!bit(), "VP9 12-bit unsupported", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
                    require(bits(3) != 7, "VP9 RGB unsupported", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
                    bit(); // color_range
                }
            }
            refresh = bits(8);
        }
        require(refresh, "VP9 invisible frame refreshes no slot", VA_STATUS_ERROR_UNIMPLEMENTED);
        unsigned slot = 0;
        while (!(refresh & (1u << slot)))
            ++slot;
        out.bytes.push_back(static_cast<uint8_t>(0x88 | (raw_profile << 3) | slot));
        unsigned magnitude = 1;
        while (magnitude < 4 && size >= (1ull << (8 * magnitude)))
            ++magnitude;
        const uint8_t marker_byte = static_cast<uint8_t>(0xc0 | ((magnitude - 1) << 3) | 1);
        out.bytes.push_back(marker_byte);
        for (unsigned n : { static_cast<unsigned>(size), 1u })
            for (unsigned i = 0; i < magnitude; ++i)
                out.bytes.push_back(static_cast<uint8_t>(n >> (8 * i)));
        out.bytes.push_back(marker_byte);
        // The firmware decodes the superframe as two pictures: the invisible
        // frame it suppresses, then the show_existing_frame header's copy.
        // Two completions under one token, and the Import policy has to know
        // before it decides how many buffers to lend this picture.
        out.completions = 2;
    }

    bool flush(AccessUnit&) override { return false; }

private:
    unsigned expected_profile_;
};

} // namespace

std::unique_ptr<Translator> make_vp9(VAProfile profile) { return std::make_unique<VP9>(profile); }

} // namespace iris::codec
