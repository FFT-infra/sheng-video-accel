// SPDX-License-Identifier: MIT
//
// H.264 encode parameters. The firmware builds SPS/PPS from the V4L2
// controls, so this translation only has to pin profile, level, rate
// control, GOP and entropy coding; nothing here emits a bitstream.

#include "annexb.h"
#include "encode.h"

#include "../util/error.h"

namespace iris::codec {

namespace {

struct Level {
    uint8_t idc;
    int menu;
};

// level_idc values from H.264 Annex A. 1b is 9 and sits below 1.0.
constexpr Level kLevels[] = {
    { 9, V4L2_MPEG_VIDEO_H264_LEVEL_1B },
    { 10, V4L2_MPEG_VIDEO_H264_LEVEL_1_0 },
    { 11, V4L2_MPEG_VIDEO_H264_LEVEL_1_1 },
    { 12, V4L2_MPEG_VIDEO_H264_LEVEL_1_2 },
    { 13, V4L2_MPEG_VIDEO_H264_LEVEL_1_3 },
    { 20, V4L2_MPEG_VIDEO_H264_LEVEL_2_0 },
    { 21, V4L2_MPEG_VIDEO_H264_LEVEL_2_1 },
    { 22, V4L2_MPEG_VIDEO_H264_LEVEL_2_2 },
    { 30, V4L2_MPEG_VIDEO_H264_LEVEL_3_0 },
    { 31, V4L2_MPEG_VIDEO_H264_LEVEL_3_1 },
    { 32, V4L2_MPEG_VIDEO_H264_LEVEL_3_2 },
    { 40, V4L2_MPEG_VIDEO_H264_LEVEL_4_0 },
    { 41, V4L2_MPEG_VIDEO_H264_LEVEL_4_1 },
    { 42, V4L2_MPEG_VIDEO_H264_LEVEL_4_2 },
    { 50, V4L2_MPEG_VIDEO_H264_LEVEL_5_0 },
    { 51, V4L2_MPEG_VIDEO_H264_LEVEL_5_1 },
    { 52, V4L2_MPEG_VIDEO_H264_LEVEL_5_2 },
    { 60, V4L2_MPEG_VIDEO_H264_LEVEL_6_0 },
    { 61, V4L2_MPEG_VIDEO_H264_LEVEL_6_1 },
    { 62, V4L2_MPEG_VIDEO_H264_LEVEL_6_2 },
};

// The highest level the firmware has at or below `idc`; -1 asks the driver
// to keep its default. Rounding down never promises a level the stream may
// exceed.
int level_menu(uint8_t idc)
{
    if (idc == 0)
        return -1;
    int menu = -1;
    for (const Level& level : kLevels)
        if (level.idc <= idc)
            menu = level.menu;
    return menu;
}

// VA slice types are the H.264 syntax values 0..2 and 5..7 (no switching
// slices). Modulo 5: 0 is P, 1 is B, 2 is I. The firmware writes one slice per
// picture and owns the picture type, so the only thing worth reading is
// whether the client asked for an intra picture without an IDR: that request
// is the one the driver has to promote, because Iris' FORCE_KEY_FRAME emits
// an IDR.
bool intra_slice(uint8_t slice_type)
{
    return slice_type % 5 == 2;
}

bool p_slice(uint8_t slice_type)
{
    return slice_type % 5 == 0;
}

bool b_slice(uint8_t slice_type)
{
    return slice_type % 5 == 1;
}

int profile_menu(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
    case VAProfileH264Main:
        return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
    case VAProfileH264High:
        return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    case VAProfileH264Baseline:
#pragma GCC diagnostic pop
        return V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
    default:
        throw Error(VA_STATUS_ERROR_UNSUPPORTED_PROFILE, "no H.264 profile menu value");
    }
}

class H264Encoder final : public Encoder {
public:
    H264Encoder(VAProfile profile, RateMode rate_mode)
        : profile_(profile)
        , rate_mode_(rate_mode)
    {
    }

    std::vector<uint8_t> prepend_parameter_sets(const std::vector<uint8_t>& access_unit,
        std::vector<NalUnit>& last) override
    {
        return codec::prepend_parameter_sets(access_unit, last, Codec::H264);
    }

    uint32_t v4l2_pixelformat() const override { return V4L2_PIX_FMT_H264; }

    // An H.264 context may be macroblock-aligned (FFmpeg creates a 1920x1088
    // context for a 1920x1080 picture), and the sequence parameters are where
    // the client says how much of it is the picture: the coded size minus the
    // frame crop. Chroma-format cropping offsets count chroma samples, so
    // each removes two luma pixels.
    bool coded_size(const EncodePicture& picture, unsigned& width, unsigned& height) const override
    {
        const auto* sequence = static_cast<const VAEncSequenceParameterBufferH264*>(picture.sequence);
        if (sequence == nullptr || sequence->picture_width_in_mbs == 0 || sequence->picture_height_in_mbs == 0)
            return false;
        unsigned coded_width = unsigned(sequence->picture_width_in_mbs) * 16;
        unsigned coded_height = unsigned(sequence->picture_height_in_mbs) * 16;
        if (sequence->frame_cropping_flag) {
            const uint64_t crop_x = uint64_t(sequence->frame_crop_left_offset)
                + sequence->frame_crop_right_offset;
            const uint64_t crop_y = uint64_t(sequence->frame_crop_top_offset)
                + sequence->frame_crop_bottom_offset;
            if (crop_x * 2 >= coded_width || crop_y * 2 >= coded_height)
                return false;
            coded_width -= unsigned(crop_x * 2);
            coded_height -= unsigned(crop_y * 2);
        }
        width = coded_width;
        height = coded_height;
        return true;
    }

    VABufferID coded_buf(const EncodePicture& picture) const override
    {
        const auto* params = static_cast<const VAEncPictureParameterBufferH264*>(picture.picture);
        return params ? params->coded_buf : VA_INVALID_ID;
    }

    EncodeParams translate(const EncodePicture& picture) override
    {
        const auto* sequence = static_cast<const VAEncSequenceParameterBufferH264*>(picture.sequence);
        const auto* params = static_cast<const VAEncPictureParameterBufferH264*>(picture.picture);
        require(sequence != nullptr, "H.264 encode picture has no sequence parameters",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        require(params != nullptr, "H.264 encode picture has no picture parameters",
            VA_STATUS_ERROR_INVALID_PARAMETER);

        // The encoder's raw port is NV12 only (iris_venc.c formats_out).
        require(sequence->bit_depth_luma_minus8 == 0 && sequence->bit_depth_chroma_minus8 == 0,
            "10-bit encode is not supported", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        require(sequence->seq_fields.bits.chroma_format_idc == 1, "4:2:0 encode is required",
            VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        // VA counts ip_period as the distance between two references, so one
        // B picture per P is ip_period 2. The encoder node takes at most one
        // B picture (V4L2_CID_MPEG_VIDEO_B_FRAMES max), and it schedules the
        // GOP itself from that single number.
        const unsigned b_frames = sequence->ip_period > 1 ? sequence->ip_period - 1 : 0;
        require(b_frames <= 1, "more than one B picture per P is not supported",
            VA_STATUS_ERROR_UNIMPLEMENTED);

        EncodeParams out;
        out.profile = profile_menu(profile_);
        out.level = level_menu(sequence->level_idc);
        out.b_frames = b_frames;
        out.rate_mode = rate_mode_;
        out.bitrate = sequence->bits_per_second;
        out.gop_size = sequence->intra_period;
        out.cabac = params->pic_fields.bits.entropy_coding_mode_flag != 0;
        out.keyframe = params->pic_fields.bits.idr_pic_flag != 0;
        out.reconstruction = params->CurrPic.picture_id;
        // The firmware consumes pictures in display order; the client submits
        // them in coding order. H.264 names the display position in the
        // picture order count, which VA carries on the current picture.
        out.display_order = params->CurrPic.TopFieldOrderCnt;

        if (const auto* rate = static_cast<const VAEncMiscParameterRateControl*>(
                find_misc(picture.misc, VAEncMiscParameterTypeRateControl))) {
            if (rate->bits_per_second) {
                // The field is the rate controller's maximum; the target is
                // target_percentage of it (va.h). A client asking for VBR
                // without a maximum gets 2x its own rate at 50 % — FFmpeg
                // fills that in when -maxrate is absent — so programming the
                // raw field runs the encoder at twice the requested rate.
                const uint32_t percentage = rate->target_percentage ? rate->target_percentage : 100;
                out.bitrate = static_cast<uint32_t>(
                    static_cast<uint64_t>(rate->bits_per_second) * percentage / 100);
                // A maximum above the target is the client's VBR ceiling.
                if (rate->bits_per_second > out.bitrate)
                    out.peak_bitrate = rate->bits_per_second;
                bitrate_ = out.bitrate;
                peak_bitrate_ = out.peak_bitrate;
            }
            if (rate->min_qp)
                out.min_qp = rate->min_qp;
            if (rate->max_qp)
                out.max_qp = rate->max_qp;
        } else if (bitrate_) {
            // No rate control in this picture: keep what the client last
            // configured instead of the sequence field, which is a rate and
            // not the target the client asked for.
            out.bitrate = bitrate_;
            out.peak_bitrate = peak_bitrate_;
        }
        if (const auto* frame = static_cast<const VAEncMiscParameterFrameRate*>(
                find_misc(picture.misc, VAEncMiscParameterTypeFrameRate))) {
            const unsigned num = frame->framerate & 0xffff;
            const unsigned den = frame->framerate >> 16;
            if (num && den) {
                out.frame_rate_num = num;
                out.frame_rate_den = den;
            }
        }
        const VAEncSliceParameterBufferH264* slice = picture.slices.empty()
            ? nullptr
            : static_cast<const VAEncSliceParameterBufferH264*>(picture.slices.front());
        // Deblocking lives in the slice header in VA and in a per-context
        // control in V4L2, so only the first slice can be forwarded.
        if (slice != nullptr) {
            require(slice->disable_deblocking_filter_idc <= 2, "invalid deblocking request",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            const bool disabled = slice->disable_deblocking_filter_idc == 1;
            out.deblock_mode = slice->disable_deblocking_filter_idc;
            out.deblock_alpha = disabled ? 0 : slice->slice_alpha_c0_offset_div2;
            out.deblock_beta = disabled ? 0 : slice->slice_beta_offset_div2;
            require(out.deblock_alpha >= -6 && out.deblock_alpha <= 6 && out.deblock_beta >= -6
                    && out.deblock_beta <= 6,
                "encode deblocking offsets are out of range", VA_STATUS_ERROR_INVALID_PARAMETER);
        }
        // Everything else the client can put in these buffers changes the
        // coded picture and has no control behind it: the firmware writes the
        // sequence headers, owns the reference structure and chooses the
        // quantisation offsets. Returning a stream that ignored the request
        // is worse than failing, which is also what the reference driver
        // (an upstream libva-v4l2 backend) does.
        require(!params->pic_fields.bits.weighted_pred_flag && !params->pic_fields.bits.weighted_bipred_idc,
            "weighted prediction is not supported", VA_STATUS_ERROR_UNIMPLEMENTED);
        require(!params->pic_fields.bits.pic_scaling_matrix_present_flag,
            "picture scaling lists are not supported", VA_STATUS_ERROR_UNIMPLEMENTED);
        require(params->chroma_qp_index_offset == 0 && params->second_chroma_qp_index_offset == 0,
            "chroma QP offsets are not supported", VA_STATUS_ERROR_UNIMPLEMENTED);
        require(params->num_ref_idx_l0_active_minus1 == 0 && params->num_ref_idx_l1_active_minus1 == 0,
            "multiple reference indexes are not supported", VA_STATUS_ERROR_UNIMPLEMENTED);
        require(picture.slices.size() <= 1, "multi-slice encode is not supported",
            VA_STATUS_ERROR_UNIMPLEMENTED);
        if (slice != nullptr) {
            require(slice->cabac_init_idc == 0, "a CABAC initialisation index is not supported",
                VA_STATUS_ERROR_UNIMPLEMENTED);
            require(slice->num_ref_idx_l0_active_minus1 == 0 && slice->num_ref_idx_l1_active_minus1 == 0,
                "multiple reference indexes are not supported", VA_STATUS_ERROR_UNIMPLEMENTED);
            // One slice has to cover the whole picture: the firmware decides
            // the slice layout, and a partial slice would code a partial
            // frame.
            require(slice->macroblock_address == 0
                    && slice->num_macroblocks
                        == unsigned(sequence->picture_width_in_mbs) * sequence->picture_height_in_mbs,
                "only one full-picture slice is supported", VA_STATUS_ERROR_UNIMPLEMENTED);
            require(intra_slice(slice->slice_type) || p_slice(slice->slice_type)
                    || b_slice(slice->slice_type),
                "only I, P and B slices are supported", VA_STATUS_ERROR_UNIMPLEMENTED);
            require(!b_slice(slice->slice_type) || b_frames != 0,
                "a B slice needs ip_period > 1", VA_STATUS_ERROR_INVALID_PARAMETER);
            require(!params->pic_fields.bits.idr_pic_flag || intra_slice(slice->slice_type),
                "an IDR needs an I slice", VA_STATUS_ERROR_INVALID_PARAMETER);
            out.intra = intra_slice(slice->slice_type);
            out.b_picture = b_slice(slice->slice_type);
            // An intra picture needs no reference; an inter picture must name
            // its past reference, and a B picture also its future one.
            if (!out.intra) {
                const VAPictureH264& ref = slice->RefPicList0[0];
                const bool valid = ref.picture_id != VA_INVALID_SURFACE
                    && !(ref.flags & (VA_PICTURE_H264_INVALID | VA_PICTURE_H264_LONG_TERM_REFERENCE));
                require(valid, "an inter picture needs a short-term reference",
                    VA_STATUS_ERROR_INVALID_PARAMETER);
                out.reference = ref.picture_id;
                if (out.b_picture) {
                    const VAPictureH264& future = slice->RefPicList1[0];
                    // No VA surface is 0, so a zero here is an entry the
                    // client never filled in.
                    require(future.picture_id != VA_INVALID_SURFACE && future.picture_id != 0
                            && !(future.flags
                                & (VA_PICTURE_H264_INVALID | VA_PICTURE_H264_LONG_TERM_REFERENCE)),
                        "a B picture needs a future short-term reference",
                        VA_STATUS_ERROR_INVALID_PARAMETER);
                }
            }
            // A non-IDR intra picture is a request the firmware cannot take
            // literally: Iris has only FORCE_KEY_FRAME, which emits an IDR.
            // Promote it, which is what the reference driver does. Dropping
            // the request would let the firmware code this picture as a P and
            // silently misorder the stream.
            if (out.intra) {
                out.promoted = !out.keyframe;
                out.keyframe = true;
            }
        } else {
            // No slice parameters: nothing says the picture is intra, so the
            // firmware codes it as a P. An IDR without a slice is the client
            // contradicting itself.
            require(!params->pic_fields.bits.idr_pic_flag, "an IDR needs an I slice",
                VA_STATUS_ERROR_INVALID_PARAMETER);
        }
        // With one B per P nothing predicts from that B, so a client marks it
        // non-reference. The firmware owns the structure and the flag does not
        // reach it; a non-reference P picture would still promise a stream the
        // firmware does not code, so only the B case is accepted.
        require(params->pic_fields.bits.reference_pic_flag != 0 || out.b_picture,
            "non-reference pictures are not supported", VA_STATUS_ERROR_UNIMPLEMENTED);
        // CBR names its target as the peak. VBR keeps the client's ceiling
        // when it gave one above the target; CQP has no rate control at all.
        if (out.rate_mode == RateMode::Cbr)
            out.peak_bitrate = out.bitrate;
        else if (out.rate_mode == RateMode::Cqp)
            out.peak_bitrate = 0;
        require(out.rate_mode != RateMode::Cbr || out.bitrate > 0, "CBR needs a bitrate",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        // CQP has no bitrate: the picture QP is what the bitstream carries,
        // pic_init_qp plus the first slice's delta (the firmware writes one
        // slice covering the picture).
        if (out.rate_mode == RateMode::Cqp) {
            int qp = params->pic_init_qp;
            if (slice != nullptr)
                qp += slice->slice_qp_delta;
            require(qp >= 1 && qp <= 51, "encode QP is out of range", VA_STATUS_ERROR_INVALID_PARAMETER);
            out.qp = qp;
        }
        return out;
    }

private:
    VAProfile profile_;
    RateMode rate_mode_;
    // The rate the client configured most recently. FFmpeg renders the
    // rate-control misc buffer once, with the first picture, so every later
    // picture arrives without it; falling back to the sequence field there
    // would reprogram the controller on each picture with a value that is a
    // rate rather than the client's target.
    uint32_t bitrate_ = 0;
    uint32_t peak_bitrate_ = 0;
};

} // namespace

std::unique_ptr<Encoder> make_h264_encoder(VAProfile profile, RateMode rate_mode)
{
    return std::make_unique<H264Encoder>(profile, rate_mode);
}

} // namespace iris::codec
