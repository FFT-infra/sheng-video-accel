// SPDX-License-Identifier: MIT
//
// HEVC: VPS/SPS/PPS synthesised from VA picture parameters on every access
// unit. The original SPS reference picture sets are not in VA, so every slice
// header is rewritten to carry an explicit RPS and an explicit reference list
// modification that reproduce VA's reference graph; everything else in the
// header is copied bit for bit. This follows the approach of
// strongtz/libva-v4l2 (MIT).

#include "translator.h"

#include "../util/error.h"
#include "bitwriter.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>

namespace iris::codec {

namespace {

bool is_main10(VAProfile profile) { return profile == VAProfileHEVCMain10; }

void profile_tier_level(BitWriter& writer, VAProfile profile, unsigned max_sub_layers_minus1)
{
    writer.bits(0, 2); // general_profile_space
    writer.bit(0); // general_tier_flag
    writer.bits(is_main10(profile) ? 2 : 1, 5); // general_profile_idc
    // Compatibility: Main (bit 1) and Main 10 (bit 2), MSB-first in the
    // 32-bit flag word.
    writer.bits(is_main10(profile) ? 0x20000000u : 0x60000000u, 32);
    writer.bit(1); // progressive_source
    writer.bit(0); // interlaced_source
    writer.bit(0); // non_packed_constraint
    writer.bit(1); // frame_only_constraint
    writer.bits(0, 44); // reserved
    writer.bits(186, 8); // level 6.2: above every stream the firmware accepts
    for (unsigned i = 0; i < max_sub_layers_minus1; ++i) {
        writer.bit(0);
        writer.bit(0);
    }
    for (unsigned i = max_sub_layers_minus1; i < 8; ++i)
        writer.bits(0, 2);
}

void scaling_lists(BitWriter& writer, const VAIQMatrixBufferHEVC& matrix)
{
    for (unsigned size_id = 0; size_id < 4; ++size_id) {
        const unsigned count = size_id == 3 ? 2 : 6;
        const unsigned side = size_id == 0 ? 4 : 8;
        for (unsigned id = 0; id < count; ++id) {
            const uint8_t* values = size_id == 0 ? matrix.ScalingList4x4[id]
                : size_id == 1                   ? matrix.ScalingList8x8[id]
                : size_id == 2                   ? matrix.ScalingList16x16[id]
                                                 : matrix.ScalingList32x32[id];
            writer.bit(1); // scaling_list_pred_mode_flag: explicit
            int previous = 8;
            if (size_id > 1) {
                previous = size_id == 2 ? matrix.ScalingListDC16x16[id] : matrix.ScalingListDC32x32[id];
                writer.se(previous - 8);
            }
            // VA is raster ordered; the bitstream uses the up-right diagonal scan.
            for (unsigned diagonal = 0; diagonal < 2 * side - 1; ++diagonal) {
                for (int y = static_cast<int>(std::min(diagonal, side - 1)), x = static_cast<int>(diagonal) - y;
                     y >= 0 && x < static_cast<int>(side); --y, ++x) {
                    const int value = values[y * side + x];
                    int delta = (value - previous + 256) % 256;
                    if (delta > 127)
                        delta -= 256;
                    writer.se(delta);
                    previous = value;
                }
            }
        }
    }
}

bool has_start_code(const uint8_t* data, size_t size)
{
    return size >= 3 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1));
}

size_t start_code_length(const uint8_t* data, size_t size)
{
    return has_start_code(data, size) ? (data[2] == 1 ? 3 : 4) : 0;
}

// VA's view of the DPB, sorted the way the spec builds RefPicSetStCurrBefore,
// StCurrAfter and LtCurr, plus the initial RefPicList0/1 that results.
struct Reference {
    unsigned index; // into ReferenceFrames[]
    int32_t poc;
    bool used;
};
struct References {
    std::vector<Reference> before, after, long_term;
    std::vector<unsigned> list[2];
    unsigned total_curr() const { return static_cast<unsigned>(list[0].size()); }
};

References collect_references(const VAPictureParameterBufferHEVC& p)
{
    References result;
    std::vector<int32_t> pocs;
    for (unsigned i = 0; i < 15; ++i) {
        const auto& r = p.ReferenceFrames[i];
        if (r.picture_id == VA_INVALID_SURFACE || (r.flags & VA_PICTURE_HEVC_INVALID))
            continue;
        require(r.pic_order_cnt != p.CurrPic.pic_order_cnt
                && std::find(pocs.begin(), pocs.end(), r.pic_order_cnt) == pocs.end(),
            "HEVC reference POC collides", VA_STATUS_ERROR_INVALID_BUFFER);
        pocs.push_back(r.pic_order_cnt);
        const bool lt = r.flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE;
        const bool used = r.flags
            & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE | VA_PICTURE_HEVC_RPS_ST_CURR_AFTER | VA_PICTURE_HEVC_RPS_LT_CURR);
        auto& bucket = lt ? result.long_term : r.pic_order_cnt < p.CurrPic.pic_order_cnt ? result.before : result.after;
        bucket.push_back({ i, r.pic_order_cnt, used });
    }
    std::sort(result.before.begin(), result.before.end(), [](auto& a, auto& b) { return a.poc > b.poc; });
    std::sort(result.after.begin(), result.after.end(), [](auto& a, auto& b) { return a.poc < b.poc; });
    std::sort(result.long_term.begin(), result.long_term.end(), [](auto& a, auto& b) { return a.poc > b.poc; });
    // Spec 8.3.4: L0 = StCurrBefore, StCurrAfter, LtCurr; L1 = StCurrAfter,
    // StCurrBefore, LtCurr (each list cycled to num_ref_idx, done by the
    // decoder). Only the "used" entries count.
    for (unsigned l = 0; l < 2; ++l) {
        for (const auto& r : l ? result.after : result.before)
            if (r.used)
                result.list[l].push_back(r.index);
        for (const auto& r : l ? result.before : result.after)
            if (r.used)
                result.list[l].push_back(r.index);
        for (const auto& r : result.long_term)
            if (r.used)
                result.list[l].push_back(r.index);
    }
    return result;
}

// Explicit st_ref_pic_set(0) plus the long-term section, written into a
// slice header in place of whatever the original carried.
void write_rps(BitWriter& b, const References& refs, const VAPictureParameterBufferHEVC& p)
{
    b.bit(0); // short_term_ref_pic_set_sps_flag: the generated SPS has none
    // idx == num_short_term_ref_pic_sets == 0: no inter_ref_pic_set_prediction_flag
    b.ue(static_cast<uint32_t>(refs.before.size()));
    b.ue(static_cast<uint32_t>(refs.after.size()));
    for (unsigned l = 0; l < 2; ++l) {
        int64_t previous = p.CurrPic.pic_order_cnt;
        for (const auto& r : l ? refs.after : refs.before) {
            const int64_t delta = l ? static_cast<int64_t>(r.poc) - previous : previous - r.poc;
            require(delta > 0 && delta <= 32768, "HEVC short-term POC delta out of range", VA_STATUS_ERROR_INVALID_BUFFER);
            b.ue(static_cast<uint32_t>(delta - 1));
            b.bit(r.used);
            previous = r.poc;
        }
    }
    if (p.slice_parsing_fields.bits.long_term_ref_pics_present_flag) {
        // num_long_term_sps is absent (num_long_term_ref_pics_sps == 0 in our SPS)
        b.ue(static_cast<uint32_t>(refs.long_term.size()));
        const unsigned n = p.log2_max_pic_order_cnt_lsb_minus4 + 4;
        const unsigned mask = (1u << n) - 1;
        const int64_t current_msb = static_cast<int64_t>(p.CurrPic.pic_order_cnt)
            - (static_cast<unsigned>(p.CurrPic.pic_order_cnt) & mask);
        int64_t previous_cycle = 0;
        for (const auto& r : refs.long_term) {
            const unsigned lsb = static_cast<unsigned>(r.poc) & mask;
            const int64_t cycle = (current_msb - (static_cast<int64_t>(r.poc) - lsb)) / (1 << n);
            require(cycle >= previous_cycle && cycle < 0x7fffffff, "HEVC long-term MSB cycle unsupported",
                VA_STATUS_ERROR_UNIMPLEMENTED);
            b.bits(lsb, n); // poc_lsb_lt
            b.bit(r.used); // used_by_curr_pic_lt_flag
            b.bit(1); // delta_poc_msb_present_flag: always explicit
            b.ue(static_cast<uint32_t>(cycle - previous_cycle));
            previous_cycle = cycle;
        }
    } else {
        require(refs.long_term.empty(), "HEVC long-term references without the SPS flag", VA_STATUS_ERROR_INVALID_BUFFER);
    }
}

// Rewrite one slice segment NAL: keep every field except pps id (-> 0),
// no_output_of_prior_pics (-> 0), pic_output_flag (-> 1), the RPS and the
// reference list modification. Returns the NAL with emulation prevention.
void rewrite_slice(const uint8_t* nal, size_t size, const VASliceParameterBufferHEVC& slice,
    const VAPictureParameterBufferHEVC& p, const References& refs, std::vector<uint8_t>& out)
{
    require(size >= 3, "truncated HEVC slice NAL", VA_STATUS_ERROR_INVALID_BUFFER);
    const unsigned header = static_cast<unsigned>(nal[0]) << 8 | nal[1];
    const unsigned type = (header >> 9) & 63;
    require(!(header & 0x8000) && !(header & 0x1f8) && (header & 7) && type <= 31,
        "invalid or multilayer HEVC slice NAL", VA_STATUS_ERROR_INVALID_BUFFER);
    const auto& f = p.slice_parsing_fields.bits;

    BitReader r(nal + 2, size - 2, 0);
    BitWriter b;
    size_t copied = 0; // reader position from which verbatim copy resumes
    auto copy_to = [&](size_t end) {
        const size_t saved = r.position();
        r.seek(copied);
        r.copy(b, end);
        copied = end;
        r.seek(saved);
    };

    const unsigned first = r.bit();
    b.bit(first);
    if (type >= 16 && type <= 23) {
        r.bit(); // no_output_of_prior_pics_flag
        b.bit(0); // VA owns output; every picture must complete
    }
    r.ue(); // slice_pic_parameter_set_id
    b.ue(0);
    copied = r.position();
    bool dependent = false;
    if (!first) {
        if (f.dependent_slice_segments_enabled_flag)
            dependent = r.bit();
        const unsigned ctb = 1u << (p.log2_min_luma_coding_block_size_minus3 + 3 + p.log2_diff_max_min_luma_coding_block_size);
        const unsigned count = ((p.pic_width_in_luma_samples + ctb - 1) / ctb) * ((p.pic_height_in_luma_samples + ctb - 1) / ctb);
        const unsigned address = r.bits(nal_ceil_log2(count));
        require(address == slice.slice_segment_address, "HEVC slice address mismatch", VA_STATUS_ERROR_INVALID_BUFFER);
    }
    if (!dependent) {
        r.skip(p.num_extra_slice_header_bits);
        const unsigned slice_type = r.ue();
        require(slice_type <= 2 && slice_type == slice.LongSliceFlags.fields.slice_type, "HEVC slice type mismatch",
            VA_STATUS_ERROR_INVALID_BUFFER);
        if (f.output_flag_present_flag) {
            copy_to(r.position());
            r.bit(); // pic_output_flag
            b.bit(1);
            copied = r.position();
        }
        if (p.pic_fields.bits.separate_colour_plane_flag)
            r.bits(2); // colour_plane_id, copied
        if (type != 19 && type != 20) {
            r.bits(p.log2_max_pic_order_cnt_lsb_minus4 + 4); // slice_pic_order_cnt_lsb, copied
            copy_to(r.position());
            // Skip the original RPS syntax entirely.
            const bool from_sps = r.bit();
            if (from_sps) {
                require(p.num_short_term_ref_pic_sets > 0, "HEVC slice references an SPS RPS the stream lacks",
                    VA_STATUS_ERROR_INVALID_BUFFER);
                r.bits(nal_ceil_log2(p.num_short_term_ref_pic_sets));
            } else {
                const size_t start = r.position();
                const bool predicted = p.num_short_term_ref_pic_sets && r.bit();
                if (predicted) {
                    require(p.st_rps_bits > 0, "HEVC predicted RPS without st_rps_bits", VA_STATUS_ERROR_INVALID_BUFFER);
                    r.seek(start);
                    r.skip(p.st_rps_bits);
                } else {
                    const unsigned neg = r.ue(), pos = r.ue();
                    require(neg + pos <= 16, "invalid HEVC RPS size", VA_STATUS_ERROR_INVALID_BUFFER);
                    for (unsigned i = 0; i < neg + pos; ++i) {
                        r.ue();
                        r.bit();
                    }
                }
            }
            if (f.long_term_ref_pics_present_flag) {
                const unsigned ns = p.num_long_term_ref_pic_sps ? r.ue() : 0;
                const unsigned np = r.ue();
                require(ns <= p.num_long_term_ref_pic_sps && ns + np <= 16, "invalid HEVC long-term RPS",
                    VA_STATUS_ERROR_INVALID_BUFFER);
                for (unsigned i = 0; i < ns + np; ++i) {
                    if (i < ns) {
                        r.bits(nal_ceil_log2(p.num_long_term_ref_pic_sps));
                    } else {
                        r.bits(p.log2_max_pic_order_cnt_lsb_minus4 + 4);
                        r.bit();
                    }
                    if (r.bit())
                        r.ue();
                }
            }
            copied = r.position();
            write_rps(b, refs, p);
            if (f.sps_temporal_mvp_enabled_flag)
                r.bit(); // slice_temporal_mvp_enabled_flag, copied
        }
        if (f.sample_adaptive_offset_enabled_flag) {
            r.bit();
            r.bit();
        }
        if (slice_type != 2) {
            unsigned counts[2] = { p.num_ref_idx_l0_default_active_minus1 + 1u, p.num_ref_idx_l1_default_active_minus1 + 1u };
            if (r.bit()) { // num_ref_idx_active_override_flag
                counts[0] = r.ue() + 1;
                if (slice_type == 0)
                    counts[1] = r.ue() + 1;
            }
            const unsigned lists = slice_type == 0 ? 2 : 1;
            const unsigned total = refs.total_curr();
            require(total > 0 && total <= 16, "HEVC inter slice without reference pictures", VA_STATUS_ERROR_INVALID_BUFFER);
            copy_to(r.position());
            for (unsigned l = 0; l < lists; ++l) {
                require(counts[l] <= 15, "invalid HEVC active reference count", VA_STATUS_ERROR_INVALID_BUFFER);
                // Skip the original modification, if any.
                if (f.lists_modification_present_flag && total > 1 && r.bit())
                    r.skip(counts[l] * nal_ceil_log2(total));
                // Ours: the generated PPS always allows modification, and we
                // spell out VA's RefPicList as positions in our initial list.
                if (total > 1)
                    b.bit(1); // ref_pic_list_modification_flag_lX
                for (unsigned j = 0; j < counts[l]; ++j) {
                    const unsigned index = slice.RefPicList[l][j];
                    auto it = std::find(refs.list[l].begin(), refs.list[l].end(), index);
                    require(it != refs.list[l].end(), "HEVC active reference missing from RPS", VA_STATUS_ERROR_INVALID_BUFFER);
                    if (total > 1)
                        b.bits(static_cast<uint32_t>(it - refs.list[l].begin()), nal_ceil_log2(total));
                }
            }
            copied = r.position();
        }
    }
    // The rest of the header is copied up to the byte_alignment() marker,
    // which is the last 1 bit before slice data. VA's offset includes the
    // two-byte NAL header and excludes emulation prevention bytes.
    require(slice.slice_data_byte_offset >= 3, "missing HEVC slice data offset", VA_STATUS_ERROR_INVALID_BUFFER);
    const size_t data = static_cast<size_t>(slice.slice_data_byte_offset - 2) * 8;
    if (data > r.size_bits() || data < r.position())
        throw Error(VA_STATUS_ERROR_INVALID_BUFFER,
            "invalid HEVC slice data offset: offset=" + std::to_string(slice.slice_data_byte_offset)
                + " parsed_bits=" + std::to_string(r.position()) + " nal_bits=" + std::to_string(r.size_bits())
                + " type=" + std::to_string(type) + " dependent=" + std::to_string(dependent));
    size_t alignment = data;
    const size_t parsed = r.position();
    do {
        require(alignment > parsed && data - alignment < 8, "invalid HEVC byte alignment", VA_STATUS_ERROR_INVALID_BUFFER);
        r.seek(--alignment);
    } while (!r.bit());
    r.seek(copied);
    r.copy(b, alignment);
    b.trailing_bits();
    r.seek(data);
    r.copy(b, r.size_bits());
    append_escaped_nal(out, { static_cast<uint8_t>(header >> 8), static_cast<uint8_t>(header & 0xff) }, b);
}

class HEVC final : public Translator {
public:
    explicit HEVC(VAProfile profile)
        : profile_(profile)
    {
        require(profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10, "HEVC profile not supported",
            VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    }
    uint32_t v4l2_pixelformat() const override { return v4l2_fourcc('H', 'E', 'V', 'C'); }

    void translate(const Picture& picture, AccessUnit& out) override
    {
        require(picture.parameters && !picture.slice_data.empty() && !picture.slice_parameters.empty(),
            "HEVC picture without parameters or slices", VA_STATUS_ERROR_INVALID_PARAMETER);
        const auto& parameters = *static_cast<const VAPictureParameterBufferHEVC*>(picture.parameters);
        const unsigned depth = profile_ == VAProfileHEVCMain10 ? 2 : 0;
        require(parameters.pic_fields.bits.chroma_format_idc == 1 && !parameters.pic_fields.bits.separate_colour_plane_flag
                && parameters.bit_depth_luma_minus8 == depth && parameters.bit_depth_chroma_minus8 == depth,
            "HEVC stream does not match the profile's 4:2:0 bit depth", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        require(!(parameters.CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC), "interlaced HEVC unsupported",
            VA_STATUS_ERROR_UNIMPLEMENTED);
        const auto* matrix = static_cast<const VAIQMatrixBufferHEVC*>(picture.iq_matrix);
        if (matrix)
            scaling_ = *matrix; // VA may free the buffer after this picture
        const VAIQMatrixBufferHEVC* effective
            = parameters.pic_fields.bits.scaling_list_enabled_flag && scaling_ ? &*scaling_ : nullptr;

        // Clients differ in how they hand over slices: FFmpeg renders one
        // slice data buffer per slice parameter buffer (offset 0 in each),
        // GStreamer and Chromium may put every slice of the picture in one
        // data buffer and let each parameter set locate its NAL by offset.
        // Pair by index when the counts match, otherwise address into the
        // single blob.
        require(!picture.slice_parameters.empty(), "HEVC picture has no slice parameters",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        require(!picture.slice_data.empty(), "HEVC picture has no slice data",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        std::vector<std::pair<const uint8_t*, size_t>> nals;
        const bool paired = picture.slice_data.size() == picture.slice_parameters.size();
        for (size_t i = 0; i < picture.slice_parameters.size(); ++i) {
            const auto* sp = static_cast<const VASliceParameterBufferHEVC*>(picture.slice_parameters[i]);
            const auto& [blob, blob_size] = paired ? picture.slice_data[i] : picture.slice_data.front();
            require(static_cast<size_t>(sp->slice_data_offset) + sp->slice_data_size <= blob_size,
                "HEVC slice exceeds its data buffer", VA_STATUS_ERROR_INVALID_BUFFER);
            const uint8_t* nal = blob + sp->slice_data_offset;
            const size_t nal_size = sp->slice_data_size;
            const size_t prefix = start_code_length(nal, nal_size);
            nals.emplace_back(nal + prefix, nal_size - prefix);
        }
        require(nals.front().second > 0, "HEVC slice data is empty", VA_STATUS_ERROR_INVALID_PARAMETER);
        const unsigned first_type = (nals.front().first[0] >> 1) & 63;
        require(started_ || (first_type >= 16 && first_type <= 21), "HEVC decode must start at an IRAP picture",
            VA_STATUS_ERROR_DECODING_ERROR);
        // RASL pictures after a CRA/BLA that opened the sequence reference a
        // GOP the decoder never saw; the firmware produces nothing for them.
        require(!(no_rasl_output_ && (first_type == 8 || first_type == 9)),
            "HEVC RASL picture unavailable after random access", VA_STATUS_ERROR_DECODING_ERROR);

        const References refs = collect_references(parameters);
        out.bytes.clear();
        out.deferred = false;
        const bool sequence_start = first_type >= 16 && first_type <= 23;
        const auto vps = hevc_vps(profile_, parameters);
        const auto sps = hevc_sps(profile_, parameters, effective);
        const auto pps = hevc_pps(parameters);
        // The firmware retains the parameter sets, so an unchanged trio is not
        // resent; H.264 showed the same repetition costs throughput at 720p.
        // An IRAP always carries them: it must be independently decodable, and
        // a restart after a drain leaves the firmware with nothing in hand.
        if (sequence_start || vps != last_vps_) {
            out.bytes.insert(out.bytes.end(), vps.begin(), vps.end());
            last_vps_ = vps;
        }
        if (sequence_start || sps != last_sps_) {
            out.bytes.insert(out.bytes.end(), sps.begin(), sps.end());
            last_sps_ = sps;
        }
        if (sequence_start || pps != last_pps_) {
            out.bytes.insert(out.bytes.end(), pps.begin(), pps.end());
            last_pps_ = pps;
        }
        for (size_t i = 0; i < nals.size(); ++i) {
            const auto* sp = static_cast<const VASliceParameterBufferHEVC*>(picture.slice_parameters[i]);
            rewrite_slice(nals[i].first, nals[i].second, *sp, parameters, refs, out.bytes);
        }
        out.sequence_start = sequence_start;
        if (first_type >= 16 && first_type <= 21)
            no_rasl_output_ = !started_ || first_type <= 20;
        started_ = true;
    }

    bool flush(AccessUnit&) override { return false; }

private:
    VAProfile profile_;
    std::optional<VAIQMatrixBufferHEVC> scaling_;
    std::vector<uint8_t> last_vps_, last_sps_, last_pps_;
    bool started_ = false;
    bool no_rasl_output_ = false;
};

} // namespace

std::vector<uint8_t> hevc_vps(VAProfile profile, const VAPictureParameterBufferHEVC& picture)
{
    BitWriter writer;
    writer.bits(0, 4); // vps_video_parameter_set_id
    writer.bit(1); // vps_base_layer_internal_flag
    writer.bit(1); // vps_base_layer_available_flag
    writer.bits(0, 6); // vps_max_layers_minus1
    writer.bits(1, 3); // vps_max_sub_layers_minus1
    writer.bit(0); // vps_temporal_id_nesting_flag
    writer.bits(0xffff, 16);
    profile_tier_level(writer, profile, 1);
    writer.bit(1); // vps_sub_layer_ordering_info_present_flag
    for (unsigned i = 0; i < 2; ++i) {
        writer.ue(std::min<unsigned>(31, picture.sps_max_dec_pic_buffering_minus1));
        writer.ue(picture.pic_fields.bits.NoPicReorderingFlag ? 0 : (i == 0 ? 3 : 4));
        writer.ue(0);
    }
    writer.bits(0, 6); // vps_max_layer_id
    writer.ue(0); // vps_num_layer_sets_minus1
    writer.bit(0); // vps_timing_info_present_flag
    writer.bit(0); // vps_extension_flag
    writer.trailing_bits();
    std::vector<uint8_t> result;
    append_escaped_nal(result, { 32 << 1, 1 }, writer);
    return result;
}

std::vector<uint8_t> hevc_sps(VAProfile profile, const VAPictureParameterBufferHEVC& picture,
    const VAIQMatrixBufferHEVC* scaling)
{
    BitWriter writer;
    writer.bits(0, 4); // sps_video_parameter_set_id
    writer.bits(1, 3); // sps_max_sub_layers_minus1
    writer.bit(1); // sps_temporal_id_nesting_flag
    profile_tier_level(writer, profile, 1);
    writer.ue(0); // sps_seq_parameter_set_id
    writer.ue(picture.pic_fields.bits.chroma_format_idc);
    if (picture.pic_fields.bits.chroma_format_idc == 3)
        writer.bit(picture.pic_fields.bits.separate_colour_plane_flag);
    writer.ue(picture.pic_width_in_luma_samples);
    writer.ue(picture.pic_height_in_luma_samples);
    writer.bit(0); // conformance_window_flag: VA reports the coded size; the
                   // visible crop comes from the decoder's COMPOSE rectangle
    writer.ue(picture.bit_depth_luma_minus8);
    writer.ue(picture.bit_depth_chroma_minus8);
    writer.ue(picture.log2_max_pic_order_cnt_lsb_minus4);
    writer.bit(1); // sps_sub_layer_ordering_info_present_flag
    for (unsigned i = 0; i < 2; ++i) {
        writer.ue(std::min<unsigned>(31, picture.sps_max_dec_pic_buffering_minus1));
        writer.ue(picture.pic_fields.bits.NoPicReorderingFlag ? 0 : (i == 0 ? 3 : 4));
        writer.ue(0);
    }
    writer.ue(picture.log2_min_luma_coding_block_size_minus3);
    writer.ue(picture.log2_diff_max_min_luma_coding_block_size);
    writer.ue(picture.log2_min_transform_block_size_minus2);
    writer.ue(picture.log2_diff_max_min_transform_block_size);
    // Bitstream order is inter then intra; the VA struct has them swapped.
    writer.ue(picture.max_transform_hierarchy_depth_inter);
    writer.ue(picture.max_transform_hierarchy_depth_intra);
    writer.bit(picture.pic_fields.bits.scaling_list_enabled_flag);
    if (picture.pic_fields.bits.scaling_list_enabled_flag) {
        writer.bit(scaling != nullptr);
        if (scaling)
            scaling_lists(writer, *scaling);
    }
    writer.bit(picture.pic_fields.bits.amp_enabled_flag);
    writer.bit(picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag);
    writer.bit(picture.pic_fields.bits.pcm_enabled_flag);
    if (picture.pic_fields.bits.pcm_enabled_flag) {
        writer.bits(picture.pcm_sample_bit_depth_luma_minus1, 4);
        writer.bits(picture.pcm_sample_bit_depth_chroma_minus1, 4);
        writer.ue(picture.log2_min_pcm_luma_coding_block_size_minus3);
        writer.ue(picture.log2_diff_max_min_pcm_luma_coding_block_size);
        writer.bit(picture.pic_fields.bits.pcm_loop_filter_disabled_flag);
    }

    // The original RPS definitions are not in VA. Declare none here and let
    // every rewritten slice header carry its own explicit set.
    writer.ue(0); // num_short_term_ref_pic_sets
    writer.bit(picture.slice_parsing_fields.bits.long_term_ref_pics_present_flag);
    if (picture.slice_parsing_fields.bits.long_term_ref_pics_present_flag)
        writer.ue(0); // num_long_term_ref_pics_sps: slices carry theirs explicitly
    writer.bit(picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag);
    writer.bit(picture.pic_fields.bits.strong_intra_smoothing_enabled_flag);
    writer.bit(0); // vui_parameters_present_flag
    writer.bit(0); // sps_extension_present_flag
    writer.trailing_bits();
    std::vector<uint8_t> result;
    append_escaped_nal(result, { 33 << 1, 1 }, writer);
    return result;
}

std::vector<uint8_t> hevc_pps(const VAPictureParameterBufferHEVC& picture)
{
    BitWriter writer;
    writer.ue(0); // pps_pic_parameter_set_id
    writer.ue(0); // pps_seq_parameter_set_id
    writer.bit(picture.slice_parsing_fields.bits.dependent_slice_segments_enabled_flag);
    writer.bit(picture.slice_parsing_fields.bits.output_flag_present_flag);
    writer.bits(picture.num_extra_slice_header_bits, 3);
    writer.bit(picture.pic_fields.bits.sign_data_hiding_enabled_flag);
    writer.bit(picture.slice_parsing_fields.bits.cabac_init_present_flag);
    writer.ue(std::min<unsigned>(14, picture.num_ref_idx_l0_default_active_minus1));
    writer.ue(std::min<unsigned>(14, picture.num_ref_idx_l1_default_active_minus1));
    // init_qp_minus26 ranges down to -(26 + QpBdOffsetY): 10-bit streams
    // legitimately go below -26.
    writer.se(std::clamp<int>(picture.init_qp_minus26, -(26 + 6 * static_cast<int>(picture.bit_depth_luma_minus8)), 25));
    writer.bit(picture.pic_fields.bits.constrained_intra_pred_flag);
    writer.bit(picture.pic_fields.bits.transform_skip_enabled_flag);
    writer.bit(picture.pic_fields.bits.cu_qp_delta_enabled_flag);
    if (picture.pic_fields.bits.cu_qp_delta_enabled_flag)
        writer.ue(picture.diff_cu_qp_delta_depth);
    writer.se(std::clamp<int>(picture.pps_cb_qp_offset, -12, 12));
    writer.se(std::clamp<int>(picture.pps_cr_qp_offset, -12, 12));
    writer.bit(picture.slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag);
    writer.bit(picture.pic_fields.bits.weighted_pred_flag);
    writer.bit(picture.pic_fields.bits.weighted_bipred_flag);
    writer.bit(picture.pic_fields.bits.transquant_bypass_enabled_flag);
    writer.bit(picture.pic_fields.bits.tiles_enabled_flag);
    writer.bit(picture.pic_fields.bits.entropy_coding_sync_enabled_flag);
    if (picture.pic_fields.bits.tiles_enabled_flag) {
        // The PPS declares one size per column/row less one, and VA stores
        // them in 19- and 21-entry arrays. A larger count would announce
        // more syntax than the loop writes and shift everything after it.
        require(picture.num_tile_columns_minus1 <= 19, "HEVC tile column count exceeds the VA array",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        require(picture.num_tile_rows_minus1 <= 21, "HEVC tile row count exceeds the VA array",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        writer.ue(picture.num_tile_columns_minus1);
        writer.ue(picture.num_tile_rows_minus1);
        // VA carries the explicit column/row sizes; a stream with uniform
        // spacing has them all equal, but writing them out is correct for
        // both cases and avoids a wrong PPS for non-uniform tiles.
        writer.bit(0); // uniform_spacing_flag
        for (unsigned i = 0; i < picture.num_tile_columns_minus1 && i < 19; ++i)
            writer.ue(picture.column_width_minus1[i]);
        for (unsigned i = 0; i < picture.num_tile_rows_minus1 && i < 21; ++i)
            writer.ue(picture.row_height_minus1[i]);
        writer.bit(picture.pic_fields.bits.loop_filter_across_tiles_enabled_flag);
    }
    writer.bit(picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag);
    // pps_deblocking_filter_control_present_flag is not in VA; always
    // present, so the offsets and override flag are never lost.
    writer.bit(1);
    writer.bit(picture.slice_parsing_fields.bits.deblocking_filter_override_enabled_flag);
    writer.bit(picture.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag);
    if (!picture.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) {
        writer.se(std::clamp<int>(picture.pps_beta_offset_div2, -6, 6));
        writer.se(std::clamp<int>(picture.pps_tc_offset_div2, -6, 6));
    }
    writer.bit(0); // pps_scaling_list_data_present_flag (lists live in the SPS)
    // Rewritten slice headers always spell their reference lists out.
    writer.bit(1); // lists_modification_present_flag
    // Range is 0..CtbLog2SizeY-2, so up to 4 for 64x64 CTBs; do not clamp.
    writer.ue(picture.log2_parallel_merge_level_minus2);
    writer.bit(picture.slice_parsing_fields.bits.slice_segment_header_extension_present_flag);
    writer.bit(0); // pps_extension_present_flag
    writer.trailing_bits();
    std::vector<uint8_t> result;
    append_escaped_nal(result, { 34 << 1, 1 }, writer);
    return result;
}

std::unique_ptr<Translator> make_hevc(VAProfile profile) { return std::make_unique<HEVC>(profile); }

} // namespace iris::codec
