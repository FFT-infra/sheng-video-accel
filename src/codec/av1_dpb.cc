// SPDX-License-Identifier: MIT
//
// Experimental AV1 translator with an internal eight-slot DPB. VA-API does
// not carry refresh_frame_flags, so the shipped translator holds each picture
// until the next access unit's ref_frame_map names the slots it refreshed.
// That hold is what Firefox 158 cannot service: it syncs each surface on its
// decoder thread, so the next picture never arrives.
//
// Here the driver chooses the refresh slots itself. Each internal slot holds
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
// superframe; the first NOSHOW completion is consumed as "invisible". This is
// the userspace path that may avoid kernel patch 0005.

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
            seen_sequence_ = true;
            emit_sequence_header_ = true;
        }

        // Which VA surfaces does this picture still need? A slot is free when
        // no surface in ref_frame_map[] occupies it.
        bool busy[kSlots] = {};
        for (unsigned i = 0; i < kSlots; ++i) {
            const VASurfaceID id = params.ref_frame_map[i];
            if (id == VA_INVALID_SURFACE)
                continue;
            for (unsigned s = 0; s < kSlots; ++s)
                if (slots_[s].surface == id)
                    busy[s] = true;
        }
        int free_slot = -1;
        for (unsigned s = 0; s < kSlots; ++s)
            if (!busy[s]) {
                free_slot = static_cast<int>(s);
                break;
            }

        // Build a copy of the VA parameters whose ref_frame_idx[] names our
        // internal slots. av1_obu.cc codes those indices verbatim and uses
        // the same numbers to index AV1ReferenceState. Intra frames code no
        // reference indices, so a VA_INVALID_SURFACE there is expected and
        // must not be resolved.
        VADecPictureParameterBufferAV1 remapped = params;
        const bool intra = params.pic_info_fields.bits.frame_type == 0
            || params.pic_info_fields.bits.frame_type == 2;
        if (!intra) {
            for (unsigned i = 0; i < 7; ++i) {
                const VASurfaceID id = params.ref_frame_map[params.ref_frame_idx[i]];
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
        for (unsigned s = 0; s < kSlots; ++s)
            remapped.ref_frame_map[s] = slots_[s].surface;

        std::vector<uint8_t> tile_data;
        std::vector<AV1TileEntry> tiles;
        collect_tiles(picture, tile_data, tiles);

        const bool resets = av1_refreshes_all_slots(params);
        // A pending picture is the one that could not refresh any slot. As
        // soon as a successor still references it and has released a slot,
        // that slot is where the pending picture is installed before the
        // successor is decoded.
        const bool need_install = pending_.valid && !resets && pending_referenced(params)
            && free_slot >= 0;

        out.bytes.clear();
        out.deferred = false;
        out.sequence_start = starts_sequence;
        out.hidden = params.pic_info_fields.bits.show_frame == 0;
        out.released_surface = VA_INVALID_ID;
        out.completions = 1;

        const char* reject = nullptr;
        if (need_install) {
            // The current picture drops a reference, so one slot is free for
            // the uncommitted picture. Recompute which one after excluding
            // the surfaces this picture still names.
            int install_slot = -1;
            for (unsigned s = 0; s < kSlots; ++s) {
                bool referenced = false;
                for (unsigned i = 0; i < kSlots; ++i)
                    if (params.ref_frame_map[i] == slots_[s].surface
                        && slots_[s].surface != VA_INVALID_SURFACE)
                        referenced = true;
                if (!referenced) {
                    install_slot = static_cast<int>(s);
                    break;
                }
            }
            require(install_slot >= 0, "AV1 reference map exceeds eight frames",
                VA_STATUS_ERROR_INVALID_PARAMETER);

            VADecPictureParameterBufferAV1 hidden_params = pending_.params;
            hidden_params.pic_info_fields.bits.show_frame = 0;
            hidden_params.pic_info_fields.bits.frame_type = 1; // INTER_FRAME
            const uint8_t hidden_refresh = static_cast<uint8_t>(1u << install_slot);
            const uint8_t shown_refresh = static_cast<uint8_t>(1u << install_slot);

            const bool built = av1_build_install_temporal_unit(out.bytes,
                hidden_params, sequence_, references_, hidden_refresh,
                pending_.tile_data.data(), pending_.tiles,
                remapped, sequence_, references_, shown_refresh,
                tile_data.data(), tiles,
                emit_sequence_header_, &reject);
            require(built, reject ? reject : "AV1 install temporal unit rejected",
                VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
            out.completions = 2;
            remember(pending_.surface_id, hidden_refresh, params);
            remember(picture.surface_id, shown_refresh, params);
            av1_update_reference_gm(references_, hidden_params, hidden_refresh);
            av1_update_reference_gm(references_, remapped, shown_refresh);
        } else {
            const uint8_t refresh = resets ? 0xff
                : free_slot >= 0 ? static_cast<uint8_t>(1u << free_slot) : 0;
            const bool built = av1_build_temporal_unit(out.bytes, remapped, sequence_, references_,
                emit_sequence_header_, refresh, tile_data.data(), tiles, &reject);
            require(built, reject ? reject : "AV1 temporal unit rejected",
                VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
            remember(picture.surface_id, refresh, remapped);
            av1_update_reference_gm(references_, remapped, refresh);
        }
        emit_sequence_header_ = false;

        // With no free slot and no pending replay, this picture refreshes
        // nothing. Keep it so the next picture can install it into whichever
        // slot that next picture drops.
        if (free_slot < 0 && !need_install) {
            pending_ = { params, std::move(tile_data), std::move(tiles), picture.surface_id, true };
        } else {
            pending_ = {};
        }
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
        for (const auto& [raw, size] : picture.slice_data) {
            const uint32_t offset = static_cast<uint32_t>(data.size());
            data.insert(data.end(), raw, raw + size);
            tiles.push_back({ offset, static_cast<uint32_t>(size) });
        }
        if (tiles.empty())
            tiles.push_back({ 0, 0 });
    }

    bool pending_referenced(const VADecPictureParameterBufferAV1& params) const
    {
        return std::find(std::begin(params.ref_frame_map), std::end(params.ref_frame_map),
            pending_.surface_id) != std::end(params.ref_frame_map);
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
