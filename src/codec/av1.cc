// SPDX-License-Identifier: MIT
//
// AV1: VA supplies a parsed frame header plus bare tile payloads; the
// stateful node wants complete OBU temporal units. av1_obu.cc rebuilds
// them. One field the header needs is not in VA at all: refresh_frame_flags.
// It is recovered from the *next* picture's reference map (a slot that then
// holds this frame's surface is a slot this frame wrote), so every picture
// is held back until its successor arrives or the stream is flushed.

#include "translator.h"

#include "../util/error.h"
#include "av1_obu.h"

#include <cstring>

namespace iris::codec {

namespace {

class AV1 final : public Translator {
public:
    explicit AV1(VAProfile profile)
    {
        require(profile == VAProfileAV1Profile0, "AV1 profile not supported", VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    }
    uint32_t v4l2_pixelformat() const override { return v4l2_fourcc('A', 'V', '0', '1'); }

    void translate(const Picture& picture, AccessUnit& out) override
    {
        require(picture.parameters, "AV1 picture without parameters", VA_STATUS_ERROR_INVALID_PARAMETER);
        const auto& parameters = *static_cast<const VADecPictureParameterBufferAV1*>(picture.parameters);

        // Sequence state and the previous picture's refresh flags come from
        // this picture's view of the world.
        const bool key_frame = parameters.pic_info_fields.bits.frame_type == 0;
        const bool starts_sequence = !seen_sequence_ || key_frame;
        // A picture released at download time refreshed no slot, so the
        // references this one makes are not what the stream asked for. Only
        // an intra picture that refreshes everything by itself can recover;
        // anything else would decode into a silently wrong image.
        if (released_without_successor_) {
            require(av1_refreshes_all_slots(parameters),
                "AV1 stream continued after a picture was released at download time",
                VA_STATUS_ERROR_OPERATION_FAILED);
            released_without_successor_ = false;
        }
        uint8_t previous_refresh = 0;
        if (held_) {
            for (unsigned slot = 0; slot < 8; ++slot)
                if (parameters.ref_frame_map[slot] == held_picture_.current_frame
                    && held_picture_.current_frame != VA_INVALID_SURFACE)
                    previous_refresh |= static_cast<uint8_t>(1u << slot);
        }

        // Release the held picture into `out`. It is still coded against the
        // sequence it arrived in, so this has to happen before this picture's
        // sequence state replaces that one: at a resolution change the two
        // disagree, and writing the old frame with the new width/height bits
        // corrupts its header.
        out.bytes.clear();
        out.deferred = false;
        out.sequence_start = false;
        if (held_) {
            build(previous_refresh, out);
        } else {
            out.deferred = true;
        }

        if (starts_sequence) {
            sequence_ = av1_sequence_state(parameters);
            seen_sequence_ = true;
            emit_sequence_header_ = true;
        }

        // ...and hold this one.
        held_ = true;
        held_surface_ = picture.surface_id;
        held_picture_ = parameters;
        held_tiles_.clear();
        for (const void* raw : picture.slice_parameters) {
            const auto* slice = static_cast<const VASliceParameterBufferAV1*>(raw);
            held_tiles_.push_back({ slice->slice_data_offset, slice->slice_data_size });
        }
        held_data_.clear();
        for (const auto& [data, size] : picture.slice_data)
            held_data_.insert(held_data_.end(), data, data + size);
        held_emits_sequence_header_ = emit_sequence_header_;
        emit_sequence_header_ = false;
        held_sequence_start_ = key_frame;
    }

    bool flush(AccessUnit& out) override
    {
        if (!held_)
            return false;
        out.bytes.clear();
        out.deferred = false;
        // Nothing follows: no slot needs to survive this frame. That is the
        // right answer only if the stream really ends here; if another
        // picture arrives, its references are no longer what the encoder
        // wrote, and translate() says so.
        const bool known = held_flags_known();
        build(0, out);
        released_without_successor_ = !known;
        return true;
    }

    uint32_t held_surface() const override { return held_ ? held_surface_ : VA_INVALID_ID; }

    bool held_flags_known() const override
    {
        // A shown key frame and a switch frame refresh all eight slots by
        // definition, so their bits are complete before any successor.
        return held_ && av1_refreshes_all_slots(held_picture_);
    }

private:
    void build(uint8_t refresh_frame_flags, AccessUnit& out)
    {
        const char* reject = nullptr;
        const bool built = av1_build_temporal_unit(out.bytes, held_picture_, sequence_, references_,
            held_emits_sequence_header_, refresh_frame_flags, held_data_.data(), held_tiles_, &reject);
        // The OBU writer drops the field for the frames that refresh every
        // slot implicitly; the reference state has to follow what the
        // firmware will actually do, not what the caller passed.
        const uint8_t slots = av1_refreshes_all_slots(held_picture_) ? 0xff : refresh_frame_flags;
        held_ = false;
        require(built, reject ? reject : "AV1 temporal unit cannot be rebuilt", VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
        for (unsigned slot = 0; slot < 8; ++slot)
            if (slots & (1u << slot))
                references_.order_hint[slot] = held_picture_.order_hint;
        av1_update_reference_gm(references_, held_picture_, slots);
        out.sequence_start = held_sequence_start_;
        out.hidden = !held_picture_.pic_info_fields.bits.show_frame;
        out.released_surface = held_surface_;
    }

    AV1SequenceState sequence_;
    AV1ReferenceState references_;
    bool seen_sequence_ = false;
    bool emit_sequence_header_ = false;
    bool held_ = false;
    uint32_t held_surface_ = VA_INVALID_ID;
    VADecPictureParameterBufferAV1 held_picture_ {};
    std::vector<AV1TileEntry> held_tiles_;
    std::vector<uint8_t> held_data_;
    bool held_emits_sequence_header_ = false;
    bool held_sequence_start_ = false;
    // Set by flush(): a picture was submitted without its successor, so its
    // refresh slots were assumed empty. A following non-intra picture would
    // decode against the wrong references.
    bool released_without_successor_ = false;
};

} // namespace

std::unique_ptr<Translator> make_av1(VAProfile profile) { return std::make_unique<AV1>(profile); }

} // namespace iris::codec
