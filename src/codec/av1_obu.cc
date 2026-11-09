/* Rebuild AV1 OBU temporal units from VA-API decode parameters. */

#include "av1_obu.h"

#include <algorithm>
#include <cstring>

#include "bitwriter.h"

namespace {

enum ObuType {
    OBU_SEQUENCE_HEADER = 1,
    OBU_TEMPORAL_DELIMITER = 2,
    OBU_FRAME = 6,
};

enum FrameType {
    KEY_FRAME = 0,
    INTER_FRAME = 1,
    INTRA_ONLY_FRAME = 2,
    SWITCH_FRAME = 3,
};

constexpr unsigned PRIMARY_REF_NONE = 7;
constexpr unsigned REFS_PER_FRAME = 7;
constexpr unsigned NUM_REF_FRAMES = 8;
constexpr unsigned MAX_SEGMENTS = 8;
constexpr unsigned SEG_LVL_MAX = 8;
constexpr unsigned MAX_TILE_WIDTH = 4096;
constexpr unsigned MAX_TILE_AREA = 4096 * 2304;
constexpr unsigned MAX_TILE_COLS = 64;
constexpr unsigned MAX_TILE_ROWS = 64;
constexpr unsigned RESTORE_NONE = 0;

// Spec 5.9.14, Segmentation_Feature_Bits / _Signed / _Max.
constexpr unsigned kSegFeatureBits[SEG_LVL_MAX] = { 8, 6, 6, 6, 6, 3, 0, 0 };
constexpr bool kSegFeatureSigned[SEG_LVL_MAX] = { true, true, true, true, true, false, false, false };
constexpr int kSegFeatureMax[SEG_LVL_MAX] = { 255, 63, 63, 63, 63, 7, 0, 0 };

unsigned ceil_log2(unsigned value)
{
    if (value < 2)
        return 0;
    unsigned bits = 0;
    unsigned n = value - 1;
    while (n > 0) {
        n >>= 1;
        ++bits;
    }
    return bits;
}

// Spec 5.9.15 tile_log2(blkSize, target): smallest k with blkSize << k >= target.
unsigned tile_log2(unsigned blk_size, unsigned target)
{
    unsigned k = 0;
    while ((blk_size << k) < target)
        ++k;
    return k;
}

// Spec 5.9.3.
int relative_dist(int a, int b, bool enable_order_hint, unsigned order_hint_bits)
{
    if (!enable_order_hint)
        return 0;
    int diff = a - b;
    const int m = 1 << (order_hint_bits - 1);
    return (diff & (m - 1)) - (diff & m);
}

// The smallest level whose limits cover this frame. VA does not carry the
// level, and an under-declared one makes conformant decoders reject the
// stream, so pick from the table rather than always writing zero.
unsigned level_for_size(unsigned width, unsigned height)
{
    struct Level {
        unsigned idx;
        unsigned max_width;
        unsigned max_height;
        unsigned max_pixels;
    };
    static constexpr Level levels[] = {
        { 0, 2048, 1152, 147456 },
        { 1, 2816, 1584, 278784 },
        { 4, 4352, 2448, 665856 },
        { 5, 5504, 3096, 1065024 },
        { 8, 6144, 3456, 2359296 },
        { 12, 8192, 4352, 8912896 },
        { 16, 16384, 8704, 35651584 },
    };
    for (const auto& level : levels) {
        if (width <= level.max_width && height <= level.max_height
            && width * height <= level.max_pixels)
            return level.idx;
    }
    return 16;
}

void write_obu(std::vector<uint8_t>& out, ObuType type, const std::vector<uint8_t>& payload)
{
    BitWriter header;
    header.bit(0); // obu_forbidden_bit
    header.bits(type, 4);
    header.bit(0); // obu_extension_flag
    header.bit(1); // obu_has_size_field
    header.bit(0); // obu_reserved_1bit
    header.leb128(payload.size());
    out.insert(out.end(), header.data().begin(), header.data().end());
    out.insert(out.end(), payload.begin(), payload.end());
}

// Spec 5.9.12.
void write_delta_q(BitWriter& writer, int value)
{
    if (value == 0) {
        writer.bit(0); // delta_coded
        return;
    }
    writer.bit(1);
    writer.su(value, 7);
}

// Spec 5.5.2. Our sequence header never signals a colour description, so the
// decoder derives unspecified primaries/transfer/matrix, which is what a
// VA client already assumes.
void write_color_config(BitWriter& writer, const VADecPictureParameterBufferAV1& picture,
    bool separate_uv_delta_q)
{
    const auto& seq = picture.seq_info_fields.fields;
    writer.bit(picture.bit_depth_idx != 0); // high_bitdepth
    // seq_profile is 0 here, so no twelve_bit and no profile-2 subsampling
    // signalling; mono_chrome is present for profiles other than 1.
    writer.bit(seq.mono_chrome);
    writer.bit(0); // color_description_present_flag
    if (seq.mono_chrome) {
        writer.bit(seq.color_range);
        writer.bit(separate_uv_delta_q);
        return;
    }
    writer.bit(seq.color_range);
    // Profile 0 fixes subsampling at 4:2:0, so no bits are written for it.
    if (seq.subsampling_x && seq.subsampling_y)
        writer.bits(0, 2); // chroma_sample_position: UNKNOWN
    writer.bit(separate_uv_delta_q);
}

std::vector<uint8_t> build_sequence_header(
    const VADecPictureParameterBufferAV1& picture, const AV1SequenceState& sequence)
{
    const auto& seq = picture.seq_info_fields.fields;
    BitWriter writer;
    writer.bits(picture.profile, 3);
    writer.bit(seq.still_picture);
    writer.bit(0); // reduced_still_picture_header
    writer.bit(0); // timing_info_present_flag
    writer.bit(0); // initial_display_delay_present_flag
    writer.bits(0, 5); // operating_points_cnt_minus_1
    writer.bits(0, 12); // operating_point_idc[0]
    writer.bits(sequence.seq_level_idx, 5);
    if (sequence.seq_level_idx > 7)
        writer.bit(0); // seq_tier[0]: Main
    writer.bits(sequence.frame_width_bits - 1, 4);
    writer.bits(sequence.frame_height_bits - 1, 4);
    writer.bits(sequence.max_frame_width - 1, sequence.frame_width_bits);
    writer.bits(sequence.max_frame_height - 1, sequence.frame_height_bits);
    writer.bit(0); // frame_id_numbers_present_flag
    writer.bit(seq.use_128x128_superblock);
    writer.bit(seq.enable_filter_intra);
    writer.bit(seq.enable_intra_edge_filter);
    writer.bit(seq.enable_interintra_compound);
    writer.bit(seq.enable_masked_compound);
    // VA reports warped motion, reference MVs, superres and restoration only
    // per frame. Enabling them at sequence level does not force their use; it
    // just makes the per-frame flag present, which is what lets the frame
    // header carry the value VA actually gave us.
    writer.bit(1); // enable_warped_motion
    writer.bit(seq.enable_dual_filter);
    writer.bit(seq.enable_order_hint);
    if (seq.enable_order_hint) {
        writer.bit(seq.enable_jnt_comp);
        writer.bit(1); // enable_ref_frame_mvs
    }
    writer.bit(1); // seq_choose_screen_content_tools -> SELECT
    writer.bit(1); // seq_choose_integer_mv -> SELECT
    if (seq.enable_order_hint)
        writer.bits(picture.order_hint_bits_minus_1, 3);
    writer.bit(1); // enable_superres
    writer.bit(seq.enable_cdef);
    writer.bit(1); // enable_restoration
    write_color_config(writer, picture, sequence.separate_uv_delta_q);
    writer.bit(seq.film_grain_params_present);
    writer.trailing_bits();
    return writer.data();
}

// Spec 5.9.15.
bool write_tile_info(BitWriter& writer, const VADecPictureParameterBufferAV1& picture,
    unsigned mi_cols, unsigned mi_rows, unsigned& tile_cols_log2, unsigned& tile_rows_log2,
    const char** reject_reason)
{
    const bool use_128 = picture.seq_info_fields.fields.use_128x128_superblock;
    const unsigned sb_cols = use_128 ? ((mi_cols + 31) >> 5) : ((mi_cols + 15) >> 4);
    const unsigned sb_rows = use_128 ? ((mi_rows + 31) >> 5) : ((mi_rows + 15) >> 4);
    const unsigned sb_shift = use_128 ? 5 : 4;
    const unsigned sb_size = sb_shift + 2;
    const unsigned max_tile_width_sb = MAX_TILE_WIDTH >> sb_size;
    const unsigned max_tile_area_sb = MAX_TILE_AREA >> (2 * sb_size);
    const unsigned min_log2_tile_cols = tile_log2(max_tile_width_sb, sb_cols);
    const unsigned max_log2_tile_cols = tile_log2(1, std::min(sb_cols, MAX_TILE_COLS));
    const unsigned max_log2_tile_rows = tile_log2(1, std::min(sb_rows, MAX_TILE_ROWS));
    const unsigned min_log2_tiles
        = std::max(min_log2_tile_cols, tile_log2(max_tile_area_sb, sb_rows * sb_cols));

    writer.bit(picture.pic_info_fields.bits.uniform_tile_spacing_flag);
    if (picture.pic_info_fields.bits.uniform_tile_spacing_flag) {
        tile_cols_log2 = ceil_log2(picture.tile_cols);
        if (tile_cols_log2 < min_log2_tile_cols || tile_cols_log2 > max_log2_tile_cols) {
            *reject_reason = "tile column count outside the range the spec allows";
            return false;
        }
        for (unsigned i = min_log2_tile_cols; i < max_log2_tile_cols; ++i) {
            const bool increment = i < tile_cols_log2;
            writer.bit(increment);
            if (!increment)
                break;
        }
        const unsigned min_log2_tile_rows
            = min_log2_tiles > tile_cols_log2 ? min_log2_tiles - tile_cols_log2 : 0;
        tile_rows_log2 = ceil_log2(picture.tile_rows);
        if (tile_rows_log2 < min_log2_tile_rows || tile_rows_log2 > max_log2_tile_rows) {
            *reject_reason = "tile row count outside the range the spec allows";
            return false;
        }
        for (unsigned i = min_log2_tile_rows; i < max_log2_tile_rows; ++i) {
            const bool increment = i < tile_rows_log2;
            writer.bit(increment);
            if (!increment)
                break;
        }
    } else {
        unsigned start_sb = 0;
        unsigned i = 0;
        for (; start_sb < sb_cols && i < picture.tile_cols; ++i) {
            const unsigned max_width = std::min(sb_cols - start_sb, max_tile_width_sb);
            writer.ns(picture.width_in_sbs_minus_1[i], max_width);
            start_sb += picture.width_in_sbs_minus_1[i] + 1;
        }
        tile_cols_log2 = ceil_log2(picture.tile_cols);
        start_sb = 0;
        i = 0;
        for (; start_sb < sb_rows && i < picture.tile_rows; ++i) {
            const unsigned max_height = sb_rows - start_sb;
            writer.ns(picture.height_in_sbs_minus_1[i], max_height);
            start_sb += picture.height_in_sbs_minus_1[i] + 1;
        }
        tile_rows_log2 = ceil_log2(picture.tile_rows);
    }
    if (tile_cols_log2 > 0 || tile_rows_log2 > 0) {
        writer.bits(picture.context_update_tile_id, tile_rows_log2 + tile_cols_log2);
        writer.bits(3, 2); // tile_size_bytes_minus_1: always use four-byte sizes
    }
    return true;
}

// Spec 5.9.12.
void write_quantization_params(
    BitWriter& writer, const VADecPictureParameterBufferAV1& picture, bool separate_uv_delta_q)
{
    const bool mono_chrome = picture.seq_info_fields.fields.mono_chrome;
    writer.bits(picture.base_qindex, 8);
    write_delta_q(writer, picture.y_dc_delta_q);
    if (!mono_chrome) {
        bool diff_uv_delta = false;
        if (separate_uv_delta_q) {
            diff_uv_delta = picture.v_dc_delta_q != picture.u_dc_delta_q
                || picture.v_ac_delta_q != picture.u_ac_delta_q;
            writer.bit(diff_uv_delta);
        }
        write_delta_q(writer, picture.u_dc_delta_q);
        write_delta_q(writer, picture.u_ac_delta_q);
        if (diff_uv_delta) {
            write_delta_q(writer, picture.v_dc_delta_q);
            write_delta_q(writer, picture.v_ac_delta_q);
        }
    }
    writer.bit(picture.qmatrix_fields.bits.using_qmatrix);
    if (picture.qmatrix_fields.bits.using_qmatrix) {
        writer.bits(picture.qmatrix_fields.bits.qm_y, 4);
        writer.bits(picture.qmatrix_fields.bits.qm_u, 4);
        if (separate_uv_delta_q)
            writer.bits(picture.qmatrix_fields.bits.qm_v, 4);
    }
}

// Spec 5.9.14.
void write_segmentation_params(BitWriter& writer, const VADecPictureParameterBufferAV1& picture)
{
    const auto& seg = picture.seg_info;
    writer.bit(seg.segment_info_fields.bits.enabled);
    if (!seg.segment_info_fields.bits.enabled)
        return;
    bool update_data = true;
    if (picture.primary_ref_frame != PRIMARY_REF_NONE) {
        writer.bit(seg.segment_info_fields.bits.update_map);
        if (seg.segment_info_fields.bits.update_map)
            writer.bit(seg.segment_info_fields.bits.temporal_update);
        writer.bit(seg.segment_info_fields.bits.update_data);
        update_data = seg.segment_info_fields.bits.update_data;
    }
    if (!update_data)
        return;
    for (unsigned segment = 0; segment < MAX_SEGMENTS; ++segment) {
        for (unsigned feature = 0; feature < SEG_LVL_MAX; ++feature) {
            const bool enabled = (seg.feature_mask[segment] >> feature) & 1u;
            writer.bit(enabled);
            if (!enabled)
                continue;
            const int value = seg.feature_data[segment][feature];
            if (kSegFeatureSigned[feature])
                writer.su(value, kSegFeatureBits[feature] + 1);
            else
                writer.bits(static_cast<uint64_t>(value), kSegFeatureBits[feature]);
        }
    }
}

// Spec 5.9.11.
void write_loop_filter_params(BitWriter& writer, const VADecPictureParameterBufferAV1& picture)
{
    writer.bits(picture.filter_level[0], 6);
    writer.bits(picture.filter_level[1], 6);
    if (!picture.seq_info_fields.fields.mono_chrome
        && (picture.filter_level[0] || picture.filter_level[1])) {
        writer.bits(picture.filter_level_u, 6);
        writer.bits(picture.filter_level_v, 6);
    }
    writer.bits(picture.loop_filter_info_fields.bits.sharpness_level, 3);
    writer.bit(picture.loop_filter_info_fields.bits.mode_ref_delta_enabled);
    if (picture.loop_filter_info_fields.bits.mode_ref_delta_enabled) {
        writer.bit(picture.loop_filter_info_fields.bits.mode_ref_delta_update);
        if (picture.loop_filter_info_fields.bits.mode_ref_delta_update) {
            for (unsigned i = 0; i < NUM_REF_FRAMES; ++i) {
                writer.bit(1); // update_ref_delta
                writer.su(picture.ref_deltas[i], 7);
            }
            for (unsigned i = 0; i < 2; ++i) {
                writer.bit(1); // update_mode_delta
                writer.su(picture.mode_deltas[i], 7);
            }
        }
    }
}

// Spec 5.9.19.
bool write_cdef_params(
    BitWriter& writer, const VADecPictureParameterBufferAV1& picture, const char** reject_reason)
{
    writer.bits(picture.cdef_damping_minus_3, 2);
    writer.bits(picture.cdef_bits, 2);
    for (unsigned i = 0; i < (1u << picture.cdef_bits); ++i) {
        const unsigned y_primary = picture.cdef_y_strengths[i] >> 2;
        const unsigned y_secondary = picture.cdef_y_strengths[i] & 3u;
        const unsigned uv_primary = picture.cdef_uv_strengths[i] >> 2;
        const unsigned uv_secondary = picture.cdef_uv_strengths[i] & 3u;
        if (y_primary > 15 || uv_primary > 15) {
            *reject_reason = "cdef strength outside the four-bit primary range";
            return false;
        }
        writer.bits(y_primary, 4);
        writer.bits(y_secondary, 2);
        if (!picture.seq_info_fields.fields.mono_chrome) {
            writer.bits(uv_primary, 4);
            writer.bits(uv_secondary, 2);
        }
    }
    return true;
}

// Spec 5.9.20.
void write_lr_params(BitWriter& writer, const VADecPictureParameterBufferAV1& picture)
{
    const auto& lr = picture.loop_restoration_fields.bits;
    const unsigned types[3]
        = { lr.yframe_restoration_type, lr.cbframe_restoration_type, lr.crframe_restoration_type };
    const unsigned planes = picture.seq_info_fields.fields.mono_chrome ? 1 : 3;
    bool uses_lr = false;
    bool uses_chroma_lr = false;
    // The bitstream codes the restoration type through a two-bit remap table,
    // Remap_Lr_Type = { NONE, SWITCHABLE, WIENER, SGRPROJ }, while VA reports
    // the resolved type directly.
    static constexpr unsigned to_coded[4] = { 0, 2, 3, 1 };
    for (unsigned plane = 0; plane < planes; ++plane) {
        writer.bits(to_coded[types[plane] & 3u], 2);
        if (types[plane] != RESTORE_NONE) {
            uses_lr = true;
            if (plane > 0)
                uses_chroma_lr = true;
        }
    }
    if (!uses_lr)
        return;
    // VA carries the resolved lr_unit_shift: 1 or 2 with 128x128 superblocks,
    // 0..2 otherwise. The bitstream codes it as unary increments from the
    // range minimum, so the 128x128 case writes one less than VA's value.
    if (picture.seq_info_fields.fields.use_128x128_superblock) {
        writer.bit(lr.lr_unit_shift > 1 ? 1 : 0);
    } else {
        writer.bit(lr.lr_unit_shift ? 1 : 0);
        if (lr.lr_unit_shift)
            writer.bit(lr.lr_unit_shift > 1 ? 1 : 0);
    }
    if (picture.seq_info_fields.fields.subsampling_x
        && picture.seq_info_fields.fields.subsampling_y && uses_chroma_lr)
        writer.bit(lr.lr_uv_shift);
}

} // namespace

AV1SequenceState av1_sequence_state(const VADecPictureParameterBufferAV1& picture)
{
    AV1SequenceState state;
    state.max_frame_width = picture.frame_width_minus1 + 1u;
    state.max_frame_height = picture.frame_height_minus1 + 1u;
    state.frame_width_bits = std::max(1u, ceil_log2(state.max_frame_width));
    state.frame_height_bits = std::max(1u, ceil_log2(state.max_frame_height));
    state.seq_level_idx = level_for_size(state.max_frame_width, state.max_frame_height);
    state.separate_uv_delta_q = picture.v_dc_delta_q != picture.u_dc_delta_q
        || picture.v_ac_delta_q != picture.u_ac_delta_q;
    state.valid = true;
    return state;
}

namespace {

// --- global_motion_params() (spec 5.9.24 - 5.9.26) --------------------------
//
// VA hands the driver the *decoded* gm_params per reference (wmmat), which is
// exactly the value the decoder ends up with. The bitstream, though, codes
// each parameter as a subexp-coded delta against the primary reference
// frame's parameter (or the default when there is no primary reference), so
// the writer inverts decode_signed_subexp_with_ref().

constexpr unsigned WARPEDMODEL_PREC_BITS = 16;
constexpr unsigned GM_ABS_TRANS_BITS = 12;
constexpr unsigned GM_ABS_TRANS_ONLY_BITS = 9;
constexpr unsigned GM_ABS_ALPHA_BITS = 12;
constexpr unsigned GM_ALPHA_PREC_BITS = 15;
constexpr unsigned GM_TRANS_PREC_BITS = 6;
constexpr unsigned GM_TRANS_ONLY_PREC_BITS = 3;
constexpr int32_t WARPEDMODEL_ONE = 1 << WARPEDMODEL_PREC_BITS;

enum WarpType { IDENTITY = 0, TRANSLATION = 1, ROTZOOM = 2, AFFINE = 3 };

void default_gm_params(int32_t out[6])
{
    out[0] = 0;
    out[1] = 0;
    out[2] = WARPEDMODEL_ONE;
    out[3] = 0;
    out[4] = 0;
    out[5] = WARPEDMODEL_ONE;
}

// Forward re-centering per spec inverse_recenter(r, v):
//   if v > 2r: v; else if v odd: r - (v+1)/2; else r + v/2
// The writer needs the inverse: given target t, find v.
unsigned recenter_forward(unsigned r, unsigned t)
{
    if (t > 2 * r)
        return t;
    if (t >= r)
        return 2 * (t - r);
    return 2 * (r - t) - 1;
}

void write_subexp(BitWriter& writer, unsigned num_syms, unsigned value)
{
    // Inverse of decode_subexp(numSyms), spec 5.9.27.
    unsigned i = 0;
    unsigned mk = 0;
    const unsigned k = 3;
    for (;;) {
        const unsigned b2 = i ? k + i - 1 : k;
        const unsigned a = 1u << b2;
        if (num_syms <= mk + 3 * a) {
            // ns(numSyms - mk)
            writer.ns(value - mk, num_syms - mk);
            return;
        }
        const bool more = value >= mk + a;
        writer.bit(more);
        if (!more) {
            writer.bits(value - mk, b2);
            return;
        }
        i++;
        mk += a;
    }
}

void write_unsigned_subexp_with_ref(BitWriter& writer, unsigned mx, unsigned r, unsigned v)
{
    // Inverse of decode_unsigned_subexp_with_ref (spec 5.9.26).
    if ((r << 1) <= mx) {
        write_subexp(writer, mx, recenter_forward(r, v));
    } else {
        write_subexp(writer, mx, recenter_forward(mx - 1 - r, mx - 1 - v));
    }
}

void write_signed_subexp_with_ref(BitWriter& writer, int low, int high, int r, int v)
{
    // Inverse of decode_signed_subexp_with_ref (spec 5.9.26).
    write_unsigned_subexp_with_ref(writer, static_cast<unsigned>(high - low), static_cast<unsigned>(r - low),
        static_cast<unsigned>(v - low));
}

// Spec 5.9.25 read_global_param(), inverted. `value` and `reference` are the
// full-precision gm_params entries; the bitstream carries them shifted.
void write_global_param(BitWriter& writer, unsigned type, unsigned idx, int32_t value, int32_t reference,
    bool allow_high_precision_mv)
{
    unsigned abs_bits = GM_ABS_ALPHA_BITS;
    unsigned prec_bits = GM_ALPHA_PREC_BITS;
    if (idx < 2) {
        if (type == TRANSLATION) {
            abs_bits = GM_ABS_TRANS_ONLY_BITS - (allow_high_precision_mv ? 0 : 1);
            prec_bits = GM_TRANS_ONLY_PREC_BITS - (allow_high_precision_mv ? 0 : 1);
        } else {
            abs_bits = GM_ABS_TRANS_BITS;
            prec_bits = GM_TRANS_PREC_BITS;
        }
    }
    const unsigned prec_diff = WARPEDMODEL_PREC_BITS - prec_bits;
    // The decoder reconstructs (coded << precDiff) + round; invert that.
    const int32_t round = (idx % 3) == 2 ? (1 << WARPEDMODEL_PREC_BITS) : 0;
    const int sub = (idx % 3) == 2 ? (1 << prec_bits) : 0;
    const int mx = 1 << abs_bits;
    const int r = (reference >> prec_diff) - sub;
    const int v = static_cast<int>((value - round) >> prec_diff);
    write_signed_subexp_with_ref(writer, -mx, mx + 1, r, v);
}

bool write_global_motion_params(BitWriter& writer, const VADecPictureParameterBufferAV1& picture,
    const AV1ReferenceState& references, bool frame_is_intra, const char** reject_reason)
{
    if (frame_is_intra)
        return true;
    for (unsigned ref = 1; ref < NUM_REF_FRAMES; ++ref) {
        const auto& wm = picture.wm[ref - 1];
        const unsigned type = wm.wmtype;
        if (type > AFFINE) {
            *reject_reason = "unknown global motion type";
            return false;
        }
        // Reference parameters: the primary reference frame's saved
        // gm_params, or the defaults (spec setup_past_independence / 7.20).
        int32_t prev[6];
        default_gm_params(prev);
        if (picture.primary_ref_frame != PRIMARY_REF_NONE) {
            const unsigned slot = picture.ref_frame_idx[picture.primary_ref_frame];
            if (slot < NUM_REF_FRAMES && references.gm_valid[slot])
                std::memcpy(prev, references.gm_params[slot][ref], sizeof(prev));
        }
        const bool is_global = type != IDENTITY;
        writer.bit(is_global);
        if (!is_global)
            continue;
        const bool is_rot_zoom = type == ROTZOOM;
        writer.bit(is_rot_zoom);
        if (!is_rot_zoom)
            writer.bit(type == TRANSLATION);
        const bool hp = picture.pic_info_fields.bits.allow_high_precision_mv != 0;
        if (type >= ROTZOOM) {
            write_global_param(writer, type, 2, wm.wmmat[2], prev[2], hp);
            write_global_param(writer, type, 3, wm.wmmat[3], prev[3], hp);
            if (type == AFFINE) {
                write_global_param(writer, type, 4, wm.wmmat[4], prev[4], hp);
                write_global_param(writer, type, 5, wm.wmmat[5], prev[5], hp);
            }
        }
        write_global_param(writer, type, 0, wm.wmmat[0], prev[0], hp);
        write_global_param(writer, type, 1, wm.wmmat[1], prev[1], hp);
    }
    return true;
}

} // namespace

bool av1_refreshes_all_slots(const VADecPictureParameterBufferAV1& picture)
{
    const auto& info = picture.pic_info_fields.bits;
    return info.frame_type == SWITCH_FRAME || (info.frame_type == KEY_FRAME && info.show_frame);
}

void av1_update_reference_gm(AV1ReferenceState& references, const VADecPictureParameterBufferAV1& picture,
    uint8_t refresh_frame_flags)
{
    const bool intra = picture.pic_info_fields.bits.frame_type == KEY_FRAME
        || picture.pic_info_fields.bits.frame_type == INTRA_ONLY_FRAME;
    for (unsigned slot = 0; slot < NUM_REF_FRAMES; ++slot) {
        if (!(refresh_frame_flags & (1u << slot)))
            continue;
        for (unsigned ref = 1; ref < NUM_REF_FRAMES; ++ref) {
            if (intra) {
                int32_t defaults[6];
                default_gm_params(defaults);
                std::memcpy(references.gm_params[slot][ref], defaults, sizeof(defaults));
            } else {
                std::memcpy(references.gm_params[slot][ref], picture.wm[ref - 1].wmmat, 6 * sizeof(int32_t));
            }
        }
        references.gm_valid[slot] = true;
    }
}

bool av1_build_temporal_unit(std::vector<uint8_t>& out,
    const VADecPictureParameterBufferAV1& picture, const AV1SequenceState& sequence,
    const AV1ReferenceState& references, bool emit_sequence_header, uint8_t refresh_frame_flags,
    const uint8_t* tile_data, const std::vector<AV1TileEntry>& tiles, const char** reject_reason)
{
    static const char* unused = nullptr;
    if (!reject_reason)
        reject_reason = &unused;
    *reject_reason = nullptr;

    if (!sequence.valid) {
        *reject_reason = "no sequence header has been established yet";
        return false;
    }
    if (picture.profile != 0) {
        *reject_reason = "only AV1 profile 0 is supported";
        return false;
    }
    if (picture.film_grain_info.film_grain_info_fields.bits.apply_grain) {
        *reject_reason = "film grain parameters are not reconstructed";
        return false;
    }
    if (picture.pic_info_fields.bits.large_scale_tile) {
        *reject_reason = "large scale tile streams are not supported";
        return false;
    }

    const auto& pic = picture.pic_info_fields.bits;
    const auto& seq = picture.seq_info_fields.fields;
    const unsigned frame_type = pic.frame_type;
    const bool frame_is_intra = frame_type == KEY_FRAME || frame_type == INTRA_ONLY_FRAME;
    const unsigned order_hint_bits = seq.enable_order_hint ? picture.order_hint_bits_minus_1 + 1u : 0u;
    const unsigned frame_width = picture.frame_width_minus1 + 1u;
    const unsigned frame_height = picture.frame_height_minus1 + 1u;
    const bool frame_size_override = frame_width != sequence.max_frame_width
        || frame_height != sequence.max_frame_height;

    BitWriter writer;
    writer.bit(0); // show_existing_frame: VA never routes these to the decoder
    writer.bits(frame_type, 2);
    writer.bit(pic.show_frame);
    if (!pic.show_frame)
        writer.bit(pic.showable_frame);

    const bool forced_error_resilient = av1_refreshes_all_slots(picture);
    const bool error_resilient = forced_error_resilient ? true : pic.error_resilient_mode != 0;
    if (!forced_error_resilient)
        writer.bit(pic.error_resilient_mode);

    writer.bit(pic.disable_cdf_update);
    // seq_force_screen_content_tools is SELECT in our sequence header, so the
    // frame always carries the flag.
    writer.bit(pic.allow_screen_content_tools);
    if (pic.allow_screen_content_tools)
        writer.bit(pic.force_integer_mv);

    if (frame_type != SWITCH_FRAME)
        writer.bit(frame_size_override);
    else if (!frame_size_override) {
        *reject_reason = "switch frames must use the sequence maximum frame size";
        return false;
    }

    if (order_hint_bits > 0)
        writer.bits(picture.order_hint, order_hint_bits);

    if (!frame_is_intra && !error_resilient)
        writer.bits(picture.primary_ref_frame, 3);

    const uint8_t all_frames = 0xff;
    const bool refresh_is_implicit = av1_refreshes_all_slots(picture);
    if (!refresh_is_implicit)
        writer.bits(refresh_frame_flags, 8);
    const uint8_t effective_refresh = refresh_is_implicit ? all_frames : refresh_frame_flags;

    if ((!frame_is_intra || effective_refresh != all_frames) && error_resilient
        && seq.enable_order_hint) {
        for (unsigned i = 0; i < NUM_REF_FRAMES; ++i)
            writer.bits(references.order_hint[i], order_hint_bits);
    }

    auto write_frame_size = [&]() {
        if (frame_size_override) {
            writer.bits(frame_width - 1, sequence.frame_width_bits);
            writer.bits(frame_height - 1, sequence.frame_height_bits);
        }
        // enable_superres is set in our sequence header, so the flag is always
        // present here.
        writer.bit(pic.use_superres);
        if (pic.use_superres) {
            if (picture.superres_scale_denominator < 9) {
                *reject_reason = "superres denominator below the coded minimum";
                return false;
            }
            writer.bits(picture.superres_scale_denominator - 9, 3);
        }
        return true;
    };
    auto write_render_size = [&]() {
        writer.bit(0); // render_and_frame_size_different
    };

    if (frame_is_intra) {
        if (!write_frame_size())
            return false;
        write_render_size();
        if (pic.allow_screen_content_tools && !pic.use_superres)
            writer.bit(pic.allow_intrabc);
    } else {
        if (seq.enable_order_hint)
            writer.bit(0); // frame_refs_short_signaling
        for (unsigned i = 0; i < REFS_PER_FRAME; ++i)
            writer.bits(picture.ref_frame_idx[i], 3);
        if (frame_size_override && !error_resilient) {
            // frame_size_with_refs(): declining every candidate reference size
            // falls through to an explicit frame size, which is what VA gives us.
            for (unsigned i = 0; i < REFS_PER_FRAME; ++i)
                writer.bit(0); // found_ref
            if (!write_frame_size())
                return false;
            write_render_size();
        } else {
            if (!write_frame_size())
                return false;
            write_render_size();
        }
        if (!pic.force_integer_mv)
            writer.bit(pic.allow_high_precision_mv);
        // read_interpolation_filter()
        const bool switchable = picture.interp_filter == 4;
        writer.bit(switchable);
        if (!switchable)
            writer.bits(picture.interp_filter, 2);
        writer.bit(pic.is_motion_mode_switchable);
        if (!error_resilient)
            writer.bit(pic.use_ref_frame_mvs);
    }

    if (!pic.disable_cdf_update)
        writer.bit(pic.disable_frame_end_update_cdf);

    const unsigned mi_cols = 2 * ((frame_width + 7) >> 3);
    const unsigned mi_rows = 2 * ((frame_height + 7) >> 3);
    unsigned tile_cols_log2 = 0;
    unsigned tile_rows_log2 = 0;
    if (!write_tile_info(writer, picture, mi_cols, mi_rows, tile_cols_log2, tile_rows_log2,
            reject_reason))
        return false;

    write_quantization_params(writer, picture, sequence.separate_uv_delta_q);
    write_segmentation_params(writer, picture);

    // delta_q_params / delta_lf_params
    if (picture.base_qindex > 0)
        writer.bit(picture.mode_control_fields.bits.delta_q_present_flag);
    const bool delta_q_present
        = picture.base_qindex > 0 && picture.mode_control_fields.bits.delta_q_present_flag;
    if (delta_q_present)
        writer.bits(picture.mode_control_fields.bits.log2_delta_q_res, 2);
    if (delta_q_present) {
        if (!pic.allow_intrabc)
            writer.bit(picture.mode_control_fields.bits.delta_lf_present_flag);
        if (picture.mode_control_fields.bits.delta_lf_present_flag) {
            writer.bits(picture.mode_control_fields.bits.log2_delta_lf_res, 2);
            writer.bit(picture.mode_control_fields.bits.delta_lf_multi);
        }
    }

    // Spec 7.12.2: a frame is losslessly coded when every segment resolves to
    // qindex zero with no delta. Segment-level qindex offsets are only in play
    // when segmentation is enabled.
    bool coded_lossless = picture.base_qindex == 0 && picture.y_dc_delta_q == 0
        && picture.u_dc_delta_q == 0 && picture.u_ac_delta_q == 0 && picture.v_dc_delta_q == 0
        && picture.v_ac_delta_q == 0;
    if (coded_lossless && picture.seg_info.segment_info_fields.bits.enabled) {
        for (unsigned segment = 0; segment < MAX_SEGMENTS && coded_lossless; ++segment) {
            if ((picture.seg_info.feature_mask[segment] & 1u)
                && picture.seg_info.feature_data[segment][0] != 0)
                coded_lossless = false;
        }
    }
    const bool all_lossless = coded_lossless && !pic.use_superres;

    if (!coded_lossless && !pic.allow_intrabc)
        write_loop_filter_params(writer, picture);
    if (!coded_lossless && !pic.allow_intrabc && seq.enable_cdef) {
        if (!write_cdef_params(writer, picture, reject_reason))
            return false;
    }
    if (!all_lossless && !pic.allow_intrabc)
        write_lr_params(writer, picture);

    // read_tx_mode()
    if (!coded_lossless)
        writer.bit(picture.mode_control_fields.bits.tx_mode == 2);

    // frame_reference_mode()
    const bool reference_select = !frame_is_intra && picture.mode_control_fields.bits.reference_select;
    if (!frame_is_intra)
        writer.bit(reference_select);

    // skip_mode_params(): the flag is only coded when a forward reference
    // exists, so the same search the decoder performs has to run here.
    bool skip_mode_allowed = false;
    if (!frame_is_intra && reference_select && seq.enable_order_hint) {
        int forward_idx = -1;
        int backward_idx = -1;
        int forward_hint = 0;
        int backward_hint = 0;
        for (unsigned i = 0; i < REFS_PER_FRAME; ++i) {
            const int ref_hint
                = static_cast<int>(references.order_hint[picture.ref_frame_idx[i]]);
            const int distance = relative_dist(ref_hint, static_cast<int>(picture.order_hint),
                seq.enable_order_hint, order_hint_bits);
            if (distance < 0) {
                if (forward_idx < 0
                    || relative_dist(ref_hint, forward_hint, seq.enable_order_hint, order_hint_bits)
                        > 0) {
                    forward_idx = static_cast<int>(i);
                    forward_hint = ref_hint;
                }
            } else if (distance > 0) {
                if (backward_idx < 0
                    || relative_dist(ref_hint, backward_hint, seq.enable_order_hint, order_hint_bits)
                        < 0) {
                    backward_idx = static_cast<int>(i);
                    backward_hint = ref_hint;
                }
            }
        }
        if (forward_idx >= 0 && backward_idx >= 0) {
            skip_mode_allowed = true;
        } else if (forward_idx >= 0) {
            int second_forward_idx = -1;
            int second_forward_hint = 0;
            for (unsigned i = 0; i < REFS_PER_FRAME; ++i) {
                const int ref_hint
                    = static_cast<int>(references.order_hint[picture.ref_frame_idx[i]]);
                if (relative_dist(ref_hint, forward_hint, seq.enable_order_hint, order_hint_bits)
                    < 0) {
                    if (second_forward_idx < 0
                        || relative_dist(ref_hint, second_forward_hint, seq.enable_order_hint,
                               order_hint_bits)
                            > 0) {
                        second_forward_idx = static_cast<int>(i);
                        second_forward_hint = ref_hint;
                    }
                }
            }
            skip_mode_allowed = second_forward_idx >= 0;
        }
    }
    if (skip_mode_allowed)
        writer.bit(picture.mode_control_fields.bits.skip_mode_present);

    if (!frame_is_intra && !error_resilient)
        writer.bit(pic.allow_warped_motion);
    writer.bit(picture.mode_control_fields.bits.reduced_tx_set_used);

    if (!write_global_motion_params(writer, picture, references, frame_is_intra, reject_reason))
        return false;

    // film_grain_params(): apply_grain was verified zero above.
    if (seq.film_grain_params_present && (pic.show_frame || pic.showable_frame))
        writer.bit(0);

    // An OBU_FRAME carries the frame header immediately followed by the tile
    // group, separated only by byte alignment.
    writer.byte_align();

    // tile_group_obu(): a single group covering every tile. tile_start_and_end
    // is omitted when the frame has exactly one tile.
    const unsigned tile_count = static_cast<unsigned>(picture.tile_cols) * picture.tile_rows;
    if (tiles.size() != tile_count) {
        *reject_reason = "tile payload count does not match the signalled tile grid";
        return false;
    }
    if (tile_count > 1)
        writer.bit(0); // tile_start_and_end_present_flag
    writer.byte_align();

    std::vector<uint8_t> payload = writer.data();
    for (size_t i = 0; i < tiles.size(); ++i) {
        // Every tile but the last is length-prefixed. tile_info() above
        // declared tile_size_bytes_minus_1 = 3, so the prefix is four bytes
        // little-endian and no tile may exceed 2^32 bytes.
        if (i + 1 < tiles.size()) {
            const uint32_t size_minus_1 = tiles[i].size - 1;
            for (unsigned byte = 0; byte < 4; ++byte)
                payload.push_back(static_cast<uint8_t>((size_minus_1 >> (byte * 8)) & 0xffu));
        }
        payload.insert(payload.end(), tile_data + tiles[i].offset,
            tile_data + tiles[i].offset + tiles[i].size);
    }

    if (emit_sequence_header) {
        write_obu(out, OBU_TEMPORAL_DELIMITER, {});
        write_obu(out, OBU_SEQUENCE_HEADER, build_sequence_header(picture, sequence));
    } else {
        write_obu(out, OBU_TEMPORAL_DELIMITER, {});
    }
    write_obu(out, OBU_FRAME, payload);
    return true;
}
