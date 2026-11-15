// SPDX-License-Identifier: MIT
//
// The encode parameters the codec layer produces and the encoder session
// consumes. Plain fields only: util/ is the one place both iris/ and codec/
// may include, and neither may include the other (docs/architecture.md §5).

#pragma once

#include <cstdint>

namespace iris {

// Cqp is constant-quality: the firmware's rate control is off and the
// per-frame QP controls decide the size. FFmpeg picks it when the client
// gives no bitrate, so a driver that omits it cannot open the encoder.
enum class RateMode { Vbr, Cbr, Cqp };

struct EncodeParams {
    // V4L2 menu values (V4L2_MPEG_VIDEO_*_PROFILE and *_LEVEL). A negative
    // level means "leave the driver default".
    int profile = 0;
    int level = -1;
    RateMode rate_mode = RateMode::Vbr;
    uint32_t bitrate = 0; // bits per second
    // The rate controller's maximum. CBR without one names its target (0 is
    // filled in at the control write); VBR carries it only when the client's
    // maximum is above its target, because VA names the target as a
    // percentage of that maximum.
    uint32_t peak_bitrate = 0;
    uint32_t gop_size = 0; // 0 asks the firmware for intra only
    unsigned frame_rate_num = 30;
    unsigned frame_rate_den = 1;
    // CQP only: the picture's QP, already combined with its slice delta.
    // < 0 when the client runs rate control instead.
    int qp = -1;
    int min_qp = -1; // < 0 leaves the firmware default
    int max_qp = -1;
    bool cabac = true; // H.264 only; HEVC is always CABAC
    // Deblocking, in the numbering both codecs share: 0 filtering enabled,
    // 1 disabled, 2 disabled across slice boundaries. VA carries these in
    // the slice header and V4L2 in a per-context control, so only the first
    // slice can reach the kernel. `alpha` is H.264's alpha_c0 offset and
    // HEVC's tc offset; `beta` is beta in both. All three are the spec's
    // div2 values, and the offsets are 0 whenever filtering is off, because
    // the slice header does not carry them then.
    int deblock_mode = 0;
    int deblock_alpha = 0;
    int deblock_beta = 0;
    // This picture must be an IDR. True when the client set VA's
    // idr_pic_flag, and also when it asked for an intra picture without one
    // (a non-IDR I picture, or an HEVC CRA): Iris has only FORCE_KEY_FRAME,
    // which emits an IDR, so that request is promoted rather than ignored.
    // The firmware has no force-keyframe control, so the session uses the
    // intra period for one picture; see EncoderSession::submit().
    bool keyframe = false;
    // The picture became an IDR only because `intra` was set and the client
    // did not set idr_pic_flag. Trace-only: it is the difference between
    // honouring a request and promoting one, and the two produce the same
    // bitstream, so nothing but the trace can tell them apart.
    bool promoted = false;
    // The picture's slice is intra-coded (H.264 I slice, HEVC I slice). The
    // session refuses to start a stream on anything else.
    bool intra = false;
    // B pictures the client asked for between two references, from VA's
    // ip_period (one B picture is ip_period 2). The encoder node takes 0 or
    // 1. The firmware owns the GOP schedule once this is set, so it decides
    // which pictures are B and in what order they are coded; the client's
    // reference lists describe the same structure but nothing in the node
    // carries them. The count is a per-context control, fixed at configure().
    unsigned b_frames = 0;
    // This picture's slice is B-coded. Only reachable when b_frames is set.
    bool b_picture = false;
    // Where this picture sits in display order, from the codec's picture
    // order count (H.264 CurrPic's field order counts, HEVC pic_order_cnt).
    // VA clients submit in coding order, the firmware consumes display
    // order, so the session uses this to put each picture in its slot. A
    // client that never fills it in sends 0 for every picture, which the
    // session reads as submission order.
    int32_t display_order = 0;
    // VA surface ids from the client's picture buffer: the reconstruction
    // surface this picture will occupy, and the picture it predicts from.
    // The session enforces the one-reference contract with them. 0 is the
    // "none" value, because a VA surface id is never 0 and this header does
    // not include va.h.
    uint32_t reconstruction = 0;
    uint32_t reference = 0;
};

} // namespace iris
