// SPDX-License-Identifier: MIT
//
// AV1 film grain synthesis. VA-API names two surfaces for a grained picture:
// current_frame carries the ungrained reference, current_display_picture the
// grained picture the client shows. The decoder publishes one frame per
// access unit, so the second picture is built here from the first.

#pragma once

#include "../iris/slots.h"

#include <cstdint>

extern "C" {
#include <va/va.h>
#include <va/va_dec_av1.h>
}

namespace va {

// The film grain parameters of one picture, in the form the synthesis needs.
// VA carries two fields as "minus" values; those are reduced here once so the
// applier reads the same numbers the AV1 syntax gives.
struct GrainParams {
    unsigned bit_depth = 8;
    // matrix_coefficients == identity: the chroma clip ceiling stays at luma's.
    bool identity = false;
    unsigned seed = 0;
    unsigned scaling_shift = 11;
    unsigned ar_coeff_shift = 8;
    unsigned ar_coeff_lag = 0;
    unsigned grain_scale_shift = 0;
    bool overlap = false;
    bool clip_to_restricted_range = false;
    bool chroma_scaling_from_luma = false;
    unsigned num_y = 0, num_cb = 0, num_cr = 0;
    uint8_t y_value[14] = {}, y_scaling[14] = {};
    uint8_t cb_value[10] = {}, cb_scaling[10] = {};
    uint8_t cr_value[10] = {}, cr_scaling[10] = {};
    int8_t ar_y[24] = {}, ar_cb[25] = {}, ar_cr[25] = {};
    int cb_mult = 0, cb_luma_mult = 0, cb_offset = 0;
    int cr_mult = 0, cr_luma_mult = 0, cr_offset = 0;
};

// Reduce the VA structure for a picture of `bit_depth` bits. The caller has
// already checked apply_grain. The "minus" fields and the biased chroma
// weights are reduced to the signed values the AV1 synthesis uses; every
// other field is copied as the client sent it, because the client is the only
// source of these numbers.
GrainParams grain_params(const VAFilmGrainStructAV1& grain, unsigned bit_depth, unsigned matrix_coefficients);

// Blend the grain of `params` into `destination`, reading the ungrained
// picture from `source`. Both layouts describe the same NV12 or P010 picture;
// the source is not modified. Returns false when the pair is not a picture
// this driver decodes (only 4:2:0 NV12 and P010 exist here), in which case the
// caller refuses the picture rather than publishing ungrain pixels.
bool apply_grain(const GrainParams& params, const iris::Layout& source, const uint8_t* src,
    const iris::Layout& destination, uint8_t* dst);

} // namespace va
