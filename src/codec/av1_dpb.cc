// SPDX-License-Identifier: MIT
//
// AV1 translator with an internal eight-slot DPB. VA-API does not carry
// refresh_frame_flags; the former translator held each picture until the next
// access unit's ref_frame_map named the slots it refreshed. A client that
// syncs each surface before submitting the next picture could not service
// that hold.
//
// The driver now chooses the refresh slots itself. Each internal slot holds
// the VA surface that last refreshed it. Before building a frame we rewrite
// ref_frame_idx[] (and the reference state) from VA's slot numbers to ours,
// pick a free slot, and emit the frame immediately. When all eight slots are
// still referenced and the previous frame was left uncommitted, a hidden
// NOSHOW frame replays that picture into the one slot the current picture no
// longer names. Both FRAME OBUs live in one temporal unit.
//
// The hidden frame produces a second CAPTURE completion with no client
// surface. AccessUnit::completions = 2 tells the session to lend the same
// client buffer twice, exactly as it already does for a VP9 alt-ref
// superframe; the first NOSHOW completion is consumed as "invisible".
// Kernel patch 0005 is not needed for this completion accounting.

#include "av1_dpb.h"

#include "../util/error.h"
#include "av1_obu.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace iris::codec {

namespace {

constexpr unsigned kSlots = 8;

struct Slot {
    VASurfaceID surface = VA_INVALID_SURFACE;
};

struct Pending {
    VADecPictureParameterBufferAV1 params {};
    std::vector<uint8_t> tile_data;
    std::vector<AV1TileEntry> tiles;
    uint32_t surface_id = 0;
    bool valid = false;
};

class AV1Dpb final : public Translator {
public:
    explicit AV1Dpb(VAProfile profile)
    {
        require(profile == VAProfileAV1Profile0, "AV1 profile not supported", VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    }
    uint32_t v4l2_pixelformat() const override { return v4l2_fourcc('A', 'V', '0', '1'); }

    void translate(const Picture& picture, AccessUnit& out) override
    {
        require(picture.parameters, "AV1 picture without parameters", VA_STATUS_ERROR_INVALID_PARAMETER);
        const auto& params = *static_cast<const VADecPictureParameterBufferAV1*>(picture.parameters);

        const bool starts_sequence = !seen_sequence_
            || params.pic_info_fields.bits.frame_type == 0; // KEY_FRAME
        if (starts_sequence) {
            sequence_ = av1_sequence_state(params);
            references_ = {};
            for (Slot& slot : slots_)
                slot.surface = VA_INVALID_SURFACE;
            pending_ = {};
            seen_sequence_ = true;
            emit_sequence_header_ = true;
        }

        std::vector<uint8_t> tile_data;
        std::vector<AV1TileEntry> tiles;
        collect_tiles(picture, tile_data, tiles);

        const bool resets = av1_refreshes_all_slots(params);
        out.bytes.clear();
        out.deferred = false;
        out.sequence_start = starts_sequence;
        // A client show_frame=0 picture has no pixels to publish. An install
        // unit keeps that flag: when its shown half is hidden too, the
        // firmware returns two NOSHOW completions under one token. The
        // session consumes the first as invisible and publishes the client's
        // hidden picture on the second (session.cc). Marking the target shown
        // makes both completions invisible and leaves the client waiting.
        out.hidden = params.pic_info_fields.bits.show_frame == 0;
        out.released_surface = VA_INVALID_ID;
        out.completions = 1;

        const char* reject = nullptr;
        // A pending picture refreshed no slot. If this picture still names it
        // and has dropped one reference, install it as a hidden NOSHOW frame
        // first. The install must be registered in the DPB before the shown
        // frame's ref_frame_idx[] is resolved, otherwise a reference to the
        // pending surface has no slot to point at.
        const bool need_install = pending_.valid && !resets && pending_referenced(params);
        if (need_install) {
            const int install_slot = free_slot(params);
            require(install_slot >= 0, "AV1 reference map exceeds eight frames",
                VA_STATUS_ERROR_INVALID_PARAMETER);

            VADecPictureParameterBufferAV1 hidden = pending_.params;
            hidden.pic_info_fields.bits.show_frame = 0;
            hidden.pic_info_fields.bits.showable_frame = 0;
            // A hidden key frame must not take the implicit all-slot refresh
            // of a shown key frame. Another backend writes it as INTRA_ONLY for the same
            // reason; an INTER_FRAME pending picture keeps its own type and
            // its own references.
            if (hidden.pic_info_fields.bits.frame_type == 0) // KEY_FRAME
                hidden.pic_info_fields.bits.frame_type = 2; // INTRA_ONLY_FRAME
            remap_references(hidden);

            const uint8_t hidden_refresh = static_cast<uint8_t>(1u << install_slot);
            // The hidden frame is decoded against the DPB as it stood before
            // the install; the shown frame against the DPB after it.
            const AV1ReferenceState hidden_references = references_;
            remember(pending_.surface_id, hidden_refresh, pending_.params);
            av1_update_reference_gm(references_, hidden, hidden_refresh);

            VADecPictureParameterBufferAV1 shown = params;
            remap_references(shown);
            const int shown_slot = free_slot(params);
            const uint8_t shown_refresh = resets ? 0xff
                : shown_slot >= 0 ? static_cast<uint8_t>(1u << shown_slot) : 0;

            const bool built = av1_build_install_temporal_unit(out.bytes,
                hidden, sequence_, hidden_references, hidden_refresh,
                pending_.tile_data.data(), pending_.tiles,
                shown, sequence_, references_, shown_refresh,
                tile_data.data(), tiles,
                emit_sequence_header_, &reject);
            require(built, reject ? reject : "AV1 install temporal unit rejected",
                VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
            out.completions = 2;
            remember(picture.surface_id, shown_refresh, params);
            av1_update_reference_gm(references_, shown, shown_refresh);
            pending_ = shown_refresh ? Pending {}
                : Pending { params, std::move(tile_data), std::move(tiles), picture.surface_id, true };
        } else {
            VADecPictureParameterBufferAV1 remapped = params;
            remap_references(remapped);
            const int slot = free_slot(params);
            const uint8_t refresh = resets ? 0xff
                : slot >= 0 ? static_cast<uint8_t>(1u << slot) : 0;
            const bool built = av1_build_temporal_unit(out.bytes, remapped, sequence_, references_,
                emit_sequence_header_, refresh, tile_data.data(), tiles, &reject);
            require(built, reject ? reject : "AV1 temporal unit rejected",
                VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
            remember(picture.surface_id, refresh, params);
            av1_update_reference_gm(references_, remapped, refresh);
            pending_ = refresh ? Pending {}
                : Pending { params, std::move(tile_data), std::move(tiles), picture.surface_id, true };
        }
        emit_sequence_header_ = false;
    }

    bool flush(AccessUnit&) override
    {
        // No picture is held: every vaEndPicture already emitted its unit.
        pending_ = {};
        return false;
    }

private:
    static void collect_tiles(const Picture& picture, std::vector<uint8_t>& data,
        std::vector<AV1TileEntry>& tiles)
    {
        // VASliceDataBufferType may arrive as one contiguous buffer while
        // VASliceParameterBufferType carries one entry per tile. The OBU
        // writer checks tiles.size() against tile_cols * tile_rows, so the
        // slice parameters, not the data buffers, define tile boundaries.
        for (const auto& [raw, size] : picture.slice_data)
            data.insert(data.end(), raw, raw + size);
        for (const void* raw : picture.slice_parameters) {
            const auto* slice = static_cast<const VASliceParameterBufferAV1*>(raw);
            tiles.push_back({ slice->slice_data_offset, slice->slice_data_size });
        }
        if (tiles.empty())
            tiles.push_back({ 0, static_cast<uint32_t>(data.size()) });
    }

    bool pending_referenced(const VADecPictureParameterBufferAV1& params) const
    {
        return std::find(std::begin(params.ref_frame_map), std::end(params.ref_frame_map),
            pending_.surface_id) != std::end(params.ref_frame_map);
    }

    bool referenced(const VADecPictureParameterBufferAV1& params, VASurfaceID surface) const
    {
        return surface != VA_INVALID_SURFACE
            && std::find(std::begin(params.ref_frame_map), std::end(params.ref_frame_map), surface)
                != std::end(params.ref_frame_map);
    }

    int free_slot(const VADecPictureParameterBufferAV1& params) const
    {
        for (unsigned s = 0; s < kSlots; ++s)
            if (!referenced(params, slots_[s].surface))
                return static_cast<int>(s);
        return -1;
    }

    // libva's ref_frame_idx[] names entries in ref_frame_map[8]. The AV1 bit
    // stream instead codes the eight DPB slot numbers directly (3 bits each),
    // and av1_obu.cc uses the same numbers to index AV1ReferenceState. Resolve
    // each VA surface to the internal slot that currently holds it.
    void remap_references(VADecPictureParameterBufferAV1& remapped) const
    {
        const bool intra = remapped.pic_info_fields.bits.frame_type == 0
            || remapped.pic_info_fields.bits.frame_type == 2;
        if (intra)
            return;
        for (unsigned i = 0; i < 7; ++i) {
            const VASurfaceID id = remapped.ref_frame_map[remapped.ref_frame_idx[i]];
            require(id != VA_INVALID_SURFACE, "AV1 reference slot is empty",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            int slot = -1;
            for (unsigned s = 0; s < kSlots; ++s)
                if (slots_[s].surface == id) {
                    slot = static_cast<int>(s);
                    break;
                }
            require(slot >= 0, "AV1 reference is outside the internal DPB",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            remapped.ref_frame_idx[i] = static_cast<uint8_t>(slot);
        }
    }

    void remember(VASurfaceID surface, uint8_t refresh,
        const VADecPictureParameterBufferAV1& params)
    {
        if (!refresh)
            return;
        // Retire every old occurrence of this surface first. Doing this in the
        // same pass as the assignment below clears a slot we just wrote, which
        // leaves the DPB oscillating between two slots instead of filling.
        for (unsigned s = 0; s < kSlots; ++s)
            if (slots_[s].surface == surface)
                slots_[s].surface = VA_INVALID_SURFACE;
        bool assigned = false;
        for (unsigned s = 0; s < kSlots; ++s) {
            if (refresh & (1u << s)) {
                // A frame that refreshes every slot (a shown key frame) still
                // needs only one canonical slot for its surface identity.
                slots_[s].surface = assigned ? VA_INVALID_SURFACE : surface;
                references_.order_hint[s] = params.order_hint;
                assigned = true;
            }
        }
    }

    AV1SequenceState sequence_;
    AV1ReferenceState references_;
    Slot slots_[kSlots];
    Pending pending_;
    bool seen_sequence_ = false;
    bool emit_sequence_header_ = false;
};

} // namespace

std::unique_ptr<Translator> make_av1_dpb(VAProfile profile)
{
    return std::make_unique<AV1Dpb>(profile);
}

} // namespace iris::codec
