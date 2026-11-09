/* Rebuild AV1 OBU temporal units from VA-API decode parameters. */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
#include <va/va.h>
#include <va/va_dec_av1.h>
}

// Iris' stateful AV1 node consumes complete OBU temporal units, while VA-API
// hands the driver a parsed frame header plus bare tile payloads. Nothing in
// the VA buffers is a bitstream, so the only way to drive the node is to write
// the OBUs back out from the parameter struct.
//
// The reconstruction does not have to reproduce the original bytes, only a
// stream that decodes to the same pixels. Where VA omits a field that is ours
// to choose (level, frame id numbering, timing info) the writer picks a value
// and stays self-consistent with it.

// Reference slot state the frame header needs but VA does not carry: the order
// hint stored in each of the eight reference slots. Maintained by the caller
// across frames, since skip-mode signalling depends on it.
struct AV1ReferenceState {
    unsigned order_hint[8] = {};
    // gm_params saved per reference slot (spec 7.20): global motion is coded
    // as a delta against the primary reference frame's parameters, so the
    // writer needs what each slot's frame carried. Index [slot][ref 1..7][6].
    int32_t gm_params[8][8][6] = {};
    bool gm_valid[8] = {};
};

// After a frame is written, record its global-motion parameters into the
// slots it refreshed. The writer cannot do this itself because it does not
// know refresh_frame_flags until the caller derived them.
void av1_update_reference_gm(AV1ReferenceState& references, const VADecPictureParameterBufferAV1& picture,
    uint8_t refresh_frame_flags);

// Everything the sequence header needs that must stay fixed for the stream.
struct AV1SequenceState {
    unsigned max_frame_width = 0;
    unsigned max_frame_height = 0;
    unsigned frame_width_bits = 0;
    unsigned frame_height_bits = 0;
    unsigned seq_level_idx = 0;
    bool separate_uv_delta_q = false;
    bool valid = false;
};

// One tile's payload inside the slice data buffer VA handed us.
struct AV1TileEntry {
    uint32_t offset;
    uint32_t size;
};

// Tells a picture that refreshes all eight reference slots by definition (a
// shown key frame or a switch frame) apart from one whose refresh_frame_flags
// the frame header has to carry. Those pictures need no successor to be
// submitted, because their slots are not a choice. The caller's reference
// bookkeeping must agree with this: the writer omits the field for them.
bool av1_refreshes_all_slots(const VADecPictureParameterBufferAV1& picture);

// Derive the fixed sequence parameters from the first frame of a sequence.
AV1SequenceState av1_sequence_state(const VADecPictureParameterBufferAV1& picture);

// Append a temporal delimiter, an optional sequence header, and one OBU_FRAME
// carrying the supplied tiles.
//
// Returns false and sets *reject_reason when the frame uses something this
// writer cannot represent. Callers must treat that as "cannot decode this
// stream" rather than emitting a partial unit: a malformed OBU desynchronises
// the stateful decoder for every frame that follows.
bool av1_build_temporal_unit(std::vector<uint8_t>& out,
    const VADecPictureParameterBufferAV1& picture, const AV1SequenceState& sequence,
    const AV1ReferenceState& references, bool emit_sequence_header, uint8_t refresh_frame_flags,
    const uint8_t* tile_data, const std::vector<AV1TileEntry>& tiles, const char** reject_reason);
