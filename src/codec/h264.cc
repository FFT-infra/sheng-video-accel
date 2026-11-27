// SPDX-License-Identifier: MIT
//
// H.264: VA long-format parameters carry no SPS/PPS bytes, so both are
// synthesised from the picture parameters. They go out when they change and
// at every sequence start; an unchanged parameter set is not repeated.
//
// The firmware keeps the parameter sets it has been given, so repeating them
// is redundant work: at 720p it cost 4.7 % of decode throughput (906-921
// against 950-964 fps over five interleaved rounds at pinned clocks) and
// bought nothing. The earlier comment here claimed Iris "fails later AUs that
// arrive without their parameter sets"; the matrix, a 1800-frame
// byte-identical framemd5 and 100 resolution switches all pass without the
// repetition.

#include "translator.h"

#include "../util/error.h"
#include "bitwriter.h"

#include <algorithm>
#include <cstring>

namespace iris::codec {

namespace {

enum : uint8_t {
    kProfileBaseline = 66,
    kProfileMain = 77,
    kProfileHigh = 100,
};

uint8_t profile_idc(VAProfile profile)
{
    switch (profile) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    case VAProfileH264Baseline:
#pragma GCC diagnostic pop
    case VAProfileH264ConstrainedBaseline:
        return kProfileBaseline;
    case VAProfileH264Main:
        return kProfileMain;
    case VAProfileH264High:
        return kProfileHigh;
    default:
        throw Error(VA_STATUS_ERROR_UNSUPPORTED_PROFILE, "H.264 profile not supported");
    }
}

// Table A-1: the frame-size and DPB limit of each level. VA-API does not
// carry level_idc and Iris stalls on a level far from what the stream needs
// in either direction, so derive the lowest level that fits.
struct Level {
    unsigned idc, max_frame_mbs, max_dpb_mbs;
};
constexpr Level kLevels[] = {
    { 10, 99, 396 }, { 11, 396, 900 }, { 12, 396, 2376 }, { 20, 396, 2376 }, { 21, 792, 4752 },
    { 22, 1620, 8100 }, { 30, 1620, 8100 }, { 31, 3600, 18000 }, { 32, 5120, 20480 }, { 40, 8192, 32768 },
    { 42, 8704, 34816 }, { 50, 22080, 110400 }, { 51, 36864, 184320 }, { 60, 139264, 696320 },
};

unsigned level_for(const VAPictureParameterBufferH264& picture)
{
    const unsigned frame_mbs = (picture.picture_width_in_mbs_minus1 + 1u) * (picture.picture_height_in_mbs_minus1 + 1u);
    const unsigned dpb_mbs = (picture.num_ref_frames + 1u) * frame_mbs;
    for (const auto& level : kLevels)
        if (frame_mbs <= level.max_frame_mbs && dpb_mbs <= level.max_dpb_mbs)
            return level.idc;
    return 62;
}

bool has_start_code(const uint8_t* data, size_t size)
{
    return size >= 3 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1));
}

// Whether the AU contains an IDR NAL (type 5).
bool contains_idr(const std::vector<uint8_t>& au)
{
    for (size_t i = 0; i + 3 < au.size();) {
        size_t start;
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)
            start = i + 3;
        else if (i + 4 < au.size() && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1)
            start = i + 4;
        else {
            ++i;
            continue;
        }
        if (start < au.size() && (au[start] & 0x1f) == 5)
            return true;
        i = start;
    }
    return false;
}

class H264 final : public Translator {
public:
    explicit H264(VAProfile profile)
        : profile_idc_(profile_idc(profile))
    {
    }
    uint32_t v4l2_pixelformat() const override { return v4l2_fourcc('H', '2', '6', '4'); }

    void translate(const Picture& picture, AccessUnit& out) override
    {
        require(picture.parameters && !picture.slice_parameters.empty() && !picture.slice_data.empty(),
            "H.264 picture without parameters or slices", VA_STATUS_ERROR_INVALID_PARAMETER);
        const auto& parameters = *static_cast<const VAPictureParameterBufferH264*>(picture.parameters);
        const auto& first_slice = *static_cast<const VASliceParameterBufferH264*>(picture.slice_parameters.front());
        // A field picture is half a frame, and the node takes whole frames
        // only. Refuse it here: submitted as a frame, the first DQBUF fails
        // and the client has already paid for the round trip before falling
        // back to software.
        require(parameters.seq_fields.bits.frame_mbs_only_flag
                && !(parameters.CurrPic.flags & (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD)),
            "interlaced H.264 unsupported", VA_STATUS_ERROR_UNIMPLEMENTED);
        // The firmware consumes a picture that depends on one it never saw
        // and completes nothing for it, so a client that syncs it waits out
        // the cold-start bound per picture. Any all-intra picture is a start:
        // the node decodes from an open-GOP I picture as it does from an IDR.
        const bool intra = std::all_of(picture.slice_parameters.begin(), picture.slice_parameters.end(),
            [](const void* slice) { return static_cast<const VASliceParameterBufferH264*>(slice)->slice_type % 5 == 2; });
        require(started_ || intra, "H.264 decode must start at an intra picture", VA_STATUS_ERROR_DECODING_ERROR);

        out.bytes.clear();
        out.deferred = false;
        // Slices first: whether this is a sequence start depends on the
        // assembled access unit, and the parameter sets go in front of it.
        for (const auto& [data, size] : picture.slice_data) {
            if (size == 0)
                continue;
            if (!has_start_code(data, size))
                out.bytes.insert(out.bytes.end(), { 0, 0, 1 });
            out.bytes.insert(out.bytes.end(), data, data + size);
        }
        // Sequence start: an IDR NAL, or VA metadata that only a random
        // access point produces (I slice, frame_num 0, POC 0).
        const unsigned slice_type = first_slice.slice_type % 5;
        const bool sequence_start = contains_idr(out.bytes)
            || (slice_type == 2 && parameters.frame_num == 0 && parameters.CurrPic.TopFieldOrderCnt == 0);

        const auto sps = h264_sps(profile_idc_, picture.surface_width, picture.surface_height, parameters);
        const auto pps = h264_pps(profile_idc_, parameters, first_slice);
        // The firmware retains the parameter sets, so an unchanged pair is not
        // resent. A sequence start always carries them: an IDR must be
        // independently decodable, and a restart after a drain leaves the
        // firmware with nothing in hand.
        std::vector<uint8_t> prefix;
        if (sequence_start || sps != last_sps_) {
            prefix.insert(prefix.end(), sps.begin(), sps.end());
            last_sps_ = sps;
        }
        if (sequence_start || pps != last_pps_) {
            prefix.insert(prefix.end(), pps.begin(), pps.end());
            last_pps_ = pps;
        }
        out.bytes.insert(out.bytes.begin(), prefix.begin(), prefix.end());
        out.sequence_start = sequence_start;
        started_ = true;
    }

    bool flush(AccessUnit&) override { return false; }

private:
    uint8_t profile_idc_;
    // The pair last written to the firmware, so an unchanged one is not
    // repeated. Per translator, which is per VA context.
    std::vector<uint8_t> last_sps_;
    std::vector<uint8_t> last_pps_;
    bool started_ = false;
};

} // namespace

std::vector<uint8_t> h264_sps(uint8_t profile, unsigned visible_width, unsigned visible_height,
    const VAPictureParameterBufferH264& picture)
{
    BitWriter writer;
    const unsigned coded_width = (picture.picture_width_in_mbs_minus1 + 1u) * 16u;
    const unsigned coded_height = (picture.picture_height_in_mbs_minus1 + 1u) * 16u;
    const unsigned chroma = picture.seq_fields.bits.chroma_format_idc;
    const unsigned crop_unit_x = chroma == 0 ? 1 : 2;
    const unsigned crop_unit_y = (picture.seq_fields.bits.frame_mbs_only_flag ? 1 : 2) * (chroma == 0 ? 1 : 2);

    writer.bits(profile, 8);
    // constraint_set0 marks real Baseline; firmware and conformance decoders
    // use it to tell Baseline from a Main stream that avoids Main tools.
    writer.bit(profile == kProfileBaseline);
    writer.bits(0, 7);
    writer.bits(level_for(picture), 8);
    writer.ue(0); // seq_parameter_set_id
    if (profile >= kProfileHigh) {
        writer.ue(chroma);
        if (chroma == 3)
            writer.bit(picture.seq_fields.bits.residual_colour_transform_flag);
        writer.ue(picture.bit_depth_luma_minus8);
        writer.ue(picture.bit_depth_chroma_minus8);
        writer.bit(0); // qpprime_y_zero_transform_bypass_flag
        writer.bit(0); // seq_scaling_matrix_present_flag
    }
    writer.ue(picture.seq_fields.bits.log2_max_frame_num_minus4);
    writer.ue(picture.seq_fields.bits.pic_order_cnt_type);
    if (picture.seq_fields.bits.pic_order_cnt_type == 0) {
        writer.ue(picture.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    } else if (picture.seq_fields.bits.pic_order_cnt_type == 1) {
        // VA does not carry the type-1 offset arrays; a zero-offset
        // representation is valid and accepted by the firmware.
        writer.bit(picture.seq_fields.bits.delta_pic_order_always_zero_flag);
        writer.se(0);
        writer.se(0);
        writer.ue(0);
    }
    writer.ue(picture.num_ref_frames);
    writer.bit(picture.seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
    writer.ue(picture.picture_width_in_mbs_minus1);
    writer.ue(picture.picture_height_in_mbs_minus1);
    writer.bit(picture.seq_fields.bits.frame_mbs_only_flag);
    if (!picture.seq_fields.bits.frame_mbs_only_flag)
        writer.bit(picture.seq_fields.bits.mb_adaptive_frame_field_flag);
    writer.bit(picture.seq_fields.bits.direct_8x8_inference_flag);

    // The crop comes from the client's surface geometry, which is the
    // visible size it wants; the coded size is macroblock-aligned.
    const unsigned visible_w = visible_width && visible_width <= coded_width ? visible_width : coded_width;
    const unsigned visible_h = visible_height && visible_height <= coded_height ? visible_height : coded_height;
    const unsigned crop_right = (coded_width - visible_w) / crop_unit_x;
    const unsigned crop_bottom = (coded_height - visible_h) / crop_unit_y;
    writer.bit(crop_right || crop_bottom);
    if (crop_right || crop_bottom) {
        writer.ue(0);
        writer.ue(crop_right);
        writer.ue(0);
        writer.ue(crop_bottom);
    }
    writer.bit(0); // vui_parameters_present_flag
    writer.trailing_bits();
    std::vector<uint8_t> result;
    append_escaped_nal(result, { 0x67 }, writer);
    return result;
}

std::vector<uint8_t> h264_pps(uint8_t profile, const VAPictureParameterBufferH264& picture,
    const VASliceParameterBufferH264& slice)
{
    BitWriter writer;
    writer.ue(0); // pic_parameter_set_id
    writer.ue(0); // seq_parameter_set_id
    writer.bit(picture.pic_fields.bits.entropy_coding_mode_flag);
    writer.bit(picture.pic_fields.bits.pic_order_present_flag);
    writer.ue(0); // num_slice_groups_minus1
    writer.ue(slice.num_ref_idx_l0_active_minus1);
    writer.ue(slice.num_ref_idx_l1_active_minus1);
    writer.bit(picture.pic_fields.bits.weighted_pred_flag);
    writer.bits(picture.pic_fields.bits.weighted_bipred_idc, 2);
    writer.se(picture.pic_init_qp_minus26);
    writer.se(picture.pic_init_qs_minus26);
    writer.se(picture.chroma_qp_index_offset);
    writer.bit(picture.pic_fields.bits.deblocking_filter_control_present_flag);
    writer.bit(picture.pic_fields.bits.constrained_intra_pred_flag);
    writer.bit(picture.pic_fields.bits.redundant_pic_cnt_present_flag);
    if (profile >= kProfileHigh) {
        writer.bit(picture.pic_fields.bits.transform_8x8_mode_flag);
        writer.bit(0); // pic_scaling_matrix_present_flag
        writer.se(picture.second_chroma_qp_index_offset);
    }
    writer.trailing_bits();
    std::vector<uint8_t> result;
    append_escaped_nal(result, { 0x68 }, writer);
    return result;
}

std::unique_ptr<Translator> make_h264(VAProfile profile) { return std::make_unique<H264>(profile); }

} // namespace iris::codec
