// SPDX-License-Identifier: MIT
//
// AV1 film grain synthesis on the CPU. The algorithm is the one the AV1
// specification's "Film grain synthesis process" describes and dav1d runs in
// its output stage: a 32x32 block grid over a 73x82 grain texture per plane,
// an autoregressive filter that shapes the texture, twelve 32x32 blocks per
// row blended at their two-sample overlap, and a piecewise-linear scaling
// curve that turns the grain into a delta on the picture's own code values.
// The port is bit-exact against libdav1d for lag 2 and 3, overlapping and not,
// chroma scaled from luma or from its own curve, and both sample widths.

#include "grain.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace va {
namespace {

constexpr unsigned kGrainWidth = 82;
constexpr unsigned kGrainHeight = 73;
constexpr unsigned kSubGrainWidth = 44;
constexpr unsigned kSubGrainHeight = 38;
constexpr unsigned kBlock = 32;

// AV1's gaussian sequence, in the signed form dav1d stores it: mean 0 and a
// standard deviation of 512 over the [-2048, 2047] the syntax spans.
constexpr int16_t kGaussian[2048] = {
    56, 568, -180, 172, 124, -84, 172, -64, -900, 24, 820, 224, 1248, 996, 272, -8,
    -916, -388, -732, -104, -188, 800, 112, -652, -320, -376, 140, -252, 492, -168, 44, -788,
    588, -584, 500, -228, 12, 680, 272, -476, 972, -100, 652, 368, 432, -196, -720, -192,
    1000, -332, 652, -136, -552, -604, -4, 192, -220, -136, 1000, -52, 372, -96, -624, 124,
    -24, 396, 540, -12, -104, 640, 464, 244, -208, -84, 368, -528, -740, 248, -968, -848,
    608, 376, -60, -292, -40, -156, 252, -292, 248, 224, -280, 400, -244, 244, -60, 76,
    -80, 212, 532, 340, 128, -36, 824, -352, -60, -264, -96, -612, 416, -704, 220, -204,
    640, -160, 1220, -408, 900, 336, 20, -336, -96, -792, 304, 48, -28, -1232, -1172, -448,
    104, -292, -520, 244, 60, -948, 0, -708, 268, 108, 356, -548, 488, -344, -136, 488,
    -196, -224, 656, -236, -1128, 60, 4, 140, 276, -676, -376, 168, -108, 464, 8, 564,
    64, 240, 308, -300, -400, -456, -136, 56, 120, -408, -116, 436, 504, -232, 328, 844,
    -164, -84, 784, -168, 232, -224, 348, -376, 128, 568, 96, -1244, -288, 276, 848, 832,
    -360, 656, 464, -384, -332, -356, 728, -388, 160, -192, 468, 296, 224, 140, -776, -100,
    280, 4, 196, 44, -36, -648, 932, 16, 1428, 28, 528, 808, 772, 20, 268, 88,
    -332, -284, 124, -384, -448, 208, -228, -1044, -328, 660, 380, -148, -300, 588, 240, 540,
    28, 136, -88, -436, 256, 296, -1000, 1400, 0, -48, 1056, -136, 264, -528, -1108, 632,
    -484, -592, -344, 796, 124, -668, -768, 388, 1296, -232, -188, -200, -288, -4, 308, 100,
    -168, 256, -500, 204, -508, 648, -136, 372, -272, -120, -1004, -552, -548, -384, 548, -296,
    428, -108, -8, -912, -324, -224, -88, -112, -220, -100, 996, -796, 548, 360, -216, 180,
    428, -200, -212, 148, 96, 148, 284, 216, -412, -320, 120, -300, -384, -604, -572, -332,
    -8, -180, -176, 696, 116, -88, 628, 76, 44, -516, 240, -208, -40, 100, -592, 344,
    -308, -452, -228, 20, 916, -1752, -136, -340, -804, 140, 40, 512, 340, 248, 184, -492,
    896, -156, 932, -628, 328, -688, -448, -616, -752, -100, 560, -1020, 180, -800, -64, 76,
    576, 1068, 396, 660, 552, -108, -28, 320, -628, 312, -92, -92, -472, 268, 16, 560,
    516, -672, -52, 492, -100, 260, 384, 284, 292, 304, -148, 88, -152, 1012, 1064, -228,
    164, -376, -684, 592, -392, 156, 196, -524, -64, -884, 160, -176, 636, 648, 404, -396,
    -436, 864, 424, -728, 988, -604, 904, -592, 296, -224, 536, -176, -920, 436, -48, 1176,
    -884, 416, -776, -824, -884, 524, -548, -564, -68, -164, -96, 692, 364, -692, -1012, -68,
    260, -480, 876, -1116, 452, -332, -352, 892, -1088, 1220, -676, 12, -292, 244, 496, 372,
    -32, 280, 200, 112, -440, -96, 24, -644, -184, 56, -432, 224, -980, 272, -260, 144,
    -436, 420, 356, 364, -528, 76, 172, -744, -368, 404, -752, -416, 684, -688, 72, 540,
    416, 92, 444, 480, -72, -1416, 164, -1172, -68, 24, 424, 264, 1040, 128, -912, -524,
    -356, 64, 876, -12, 4, -88, 532, 272, -524, 320, 276, -508, 940, 24, -400, -120,
    756, 60, 236, -412, 100, 376, -484, 400, -100, -740, -108, -260, 328, -268, 224, -200,
    -416, 184, -604, -564, -20, 296, 60, 892, -888, 60, 164, 68, -760, 216, -296, 904,
    -336, -28, 404, -356, -568, -208, -1480, -512, 296, 328, -360, -164, -1560, -776, 1156, -428,
    164, -504, -112, 120, -216, -148, -264, 308, 32, 64, -72, 72, 116, 176, -64, -272,
    460, -536, -784, -280, 348, 108, -752, -132, 524, -540, -776, 116, -296, -1196, -288, -560,
    1040, -472, 116, -848, -1116, 116, 636, 696, 284, -176, 1016, 204, -864, -648, -248, 356,
    972, -584, -204, 264, 880, 528, -24, -184, 116, 448, -144, 828, 524, 212, -212, 52,
    12, 200, 268, -488, -404, -880, 824, -672, -40, 908, -248, 500, 716, -576, 492, -576,
    16, 720, -108, 384, 124, 344, 280, 576, -500, 252, 104, -308, 196, -188, -8, 1268,
    296, 1032, -1196, 436, 316, 372, -432, -200, -660, 704, -224, 596, -132, 268, 32, -452,
    884, 104, -1008, 424, -1348, -280, 4, -1168, 368, 476, 696, 300, -8, 24, 180, -592,
    -196, 388, 304, 500, 724, -160, 244, -84, 272, -256, -420, 320, 208, -144, -156, 156,
    364, 452, 28, 540, 316, 220, -644, -248, 464, 72, 360, 32, -388, 496, -680, -48,
    208, -116, -408, 60, -604, -392, 548, -840, 784, -460, 656, -544, -388, -264, 908, -800,
    -628, -612, -568, 572, -220, 164, 288, -16, -308, 308, -112, -636, -760, 280, -668, 432,
    364, 240, -196, 604, 340, 384, 196, 592, -44, -500, 432, -580, -132, 636, -76, 392,
    4, -412, 540, 508, 328, -356, -36, 16, -220, -64, -248, -60, 24, -192, 368, 1040,
    92, -24, -1044, -32, 40, 104, 148, 192, -136, -520, 56, -816, -224, 732, 392, 356,
    212, -80, -424, -1008, -324, 588, -1496, 576, 460, -816, -848, 56, -580, -92, -1372, -112,
    -496, 200, 364, 52, -140, 48, -48, -60, 84, 72, 40, 132, -356, -268, -104, -284,
    -404, 732, -520, 164, -304, -540, 120, 328, -76, -460, 756, 388, 588, 236, -436, -72,
    -176, -404, -316, -148, 716, -604, 404, -72, -88, -888, -68, 944, 88, -220, -344, 960,
    472, 460, -232, 704, 120, 832, -228, 692, -508, 132, -476, 844, -748, -364, -44, 1116,
    -1104, -1056, 76, 428, 552, -692, 60, 356, 96, -384, -188, -612, -576, 736, 508, 892,
    352, -1132, 504, -24, -352, 324, 332, -600, -312, 292, 508, -144, -8, 484, 48, 284,
    -260, -240, 256, -100, -292, -204, -44, 472, -204, 908, -188, -1000, -256, 92, 1164, -392,
    564, 356, 652, -28, -884, 256, 484, -192, 760, -176, 376, -524, -452, -436, 860, -736,
    212, 124, 504, -476, 468, 76, -472, 552, -692, -944, -620, 740, -240, 400, 132, 20,
    192, -196, 264, -668, -1012, -60, 296, -316, -828, 76, -156, 284, -768, -448, -832, 148,
    248, 652, 616, 1236, 288, -328, -400, -124, 588, 220, 520, -696, 1032, 768, -740, -92,
    -272, 296, 448, -464, 412, -200, 392, 440, -200, 264, -152, -260, 320, 1032, 216, 320,
    -8, -64, 156, -1016, 1084, 1172, 536, 484, -432, 132, 372, -52, -256, 84, 116, -352,
    48, 116, 304, -384, 412, 924, -300, 528, 628, 180, 648, 44, -980, -220, 1320, 48,
    332, 748, 524, -268, -720, 540, -276, 564, -344, -208, -196, 436, 896, 88, -392, 132,
    80, -964, -288, 568, 56, -48, -456, 888, 8, 552, -156, -292, 948, 288, 128, -716,
    -292, 1192, -152, 876, 352, -600, -260, -812, -468, -28, -120, -32, -44, 1284, 496, 192,
    464, 312, -76, -516, -380, -456, -1012, -48, 308, -156, 36, 492, -156, -808, 188, 1652,
    68, -120, -116, 316, 160, -140, 352, 808, -416, 592, 316, -480, 56, 528, -204, -568,
    372, -232, 752, -344, 744, -4, 324, -416, -600, 768, 268, -248, -88, -132, -420, -432,
    80, -288, 404, -316, -1216, -588, 520, -108, 92, -320, 368, -480, -216, -92, 1688, -300,
    180, 1020, -176, 820, -68, -228, -260, 436, -904, 20, 40, -508, 440, -736, 312, 332,
    204, 760, -372, 728, 96, -20, -632, -520, -560, 336, 1076, -64, -532, 776, 584, 192,
    396, -728, -520, 276, -188, 80, -52, -612, -252, -48, 648, 212, -688, 228, -52, -260,
    428, -412, -272, -404, 180, 816, -796, 48, 152, 484, -88, -216, 988, 696, 188, -528,
    648, -116, -180, 316, 476, 12, -564, 96, 476, -252, -364, -376, -392, 556, -256, -576,
    260, -352, 120, -16, -136, -260, -492, 72, 556, 660, 580, 616, 772, 436, 424, -32,
    -324, -1268, 416, -324, -80, 920, 160, 228, 724, 32, -516, 64, 384, 68, -128, 136,
    240, 248, -204, -68, 252, -932, -120, -480, -628, -84, 192, 852, -404, -288, -132, 204,
    100, 168, -68, -196, -868, 460, 1080, 380, -80, 244, 0, 484, -888, 64, 184, 352,
    600, 460, 164, 604, -196, 320, -64, 588, -184, 228, 12, 372, 48, -848, -344, 224,
    208, -200, 484, 128, -20, 272, -468, -840, 384, 256, -720, -520, -464, -580, 112, -120,
    644, -356, -208, -608, -528, 704, 560, -424, 392, 828, 40, 84, 200, -152, 0, -144,
    584, 280, -120, 80, -556, -972, -196, -472, 724, 80, 168, -32, 88, 160, -688, 0,
    160, 356, 372, -776, 740, -128, 676, -248, -480, 4, -364, 96, 544, 232, -1032, 956,
    236, 356, 20, -40, 300, 24, -676, -596, 132, 1120, -104, 532, -1096, 568, 648, 444,
    508, 380, 188, -376, -604, 1488, 424, 24, 756, -220, -192, 716, 120, 920, 688, 168,
    44, -460, 568, 284, 1144, 1160, 600, 424, 888, 656, -356, -320, 220, 316, -176, -724,
    -188, -816, -628, -348, -228, -380, 1012, -452, -660, 736, 928, 404, -696, -72, -268, -892,
    128, 184, -344, -780, 360, 336, 400, 344, 428, 548, -112, 136, -228, -216, -820, -516,
    340, 92, -136, 116, -300, 376, -244, 100, -316, -520, -284, -12, 824, 164, -548, -180,
    -128, 116, -924, -828, 268, -368, -580, 620, 192, 160, 0, -1676, 1068, 424, -56, -360,
    468, -156, 720, 288, -528, 556, -364, 548, -148, 504, 316, 152, -648, -620, -684, -24,
    -376, -384, -108, -920, -1032, 768, 180, -264, -508, -1268, -260, -60, 300, -240, 988, 724,
    -376, -576, -212, -736, 556, 192, 1092, -620, -880, 376, -56, -4, -216, -32, 836, 268,
    396, 1332, 864, -600, 100, 56, -412, -92, 356, 180, 884, -468, -436, 292, -388, -804,
    -704, -840, 368, -348, 140, -724, 1536, 940, 372, 112, -372, 436, -480, 1136, 296, -32,
    -228, 132, -48, -220, 868, -1016, -60, -1044, -464, 328, 916, 244, 12, -736, -296, 360,
    468, -376, -108, -92, 788, 368, -56, 544, 400, -672, -420, 728, 16, 320, 44, -284,
    -380, -796, 488, 132, 204, -596, -372, 88, -152, -908, -636, -572, -624, -116, -692, -200,
    -56, 276, -88, 484, -324, 948, 864, 1000, -456, -184, -276, 292, -296, 156, 676, 320,
    160, 908, -84, -1236, -288, -116, 260, -372, -644, 732, -756, -96, 84, 344, -520, 348,
    -688, 240, -84, 216, -1044, -136, -676, -396, -1500, 960, -40, 176, 168, 1516, 420, -504,
    -344, -364, -360, 1216, -940, -380, -212, 252, -660, -708, 484, -444, -152, 928, -120, 1112,
    476, -260, 560, -148, -344, 108, -196, 228, -288, 504, 560, -328, -88, 288, -1008, 460,
    -228, 468, -836, -196, 76, 388, 232, 412, -1168, -716, -644, 756, -172, -356, -504, 116,
    432, 528, 48, 476, -168, -608, 448, 160, -532, -272, 28, -676, -12, 828, 980, 456,
    520, 104, -104, 256, -344, -4, -28, -368, -52, -524, -572, -556, -200, 768, 1124, -208,
    -512, 176, 232, 248, -148, -888, 604, -600, -304, 804, -156, -212, 488, -192, -804, -256,
    368, -360, -916, -328, 228, -240, -448, -472, 856, -556, -364, 572, -12, -156, -368, -340,
    432, 252, -752, -152, 288, 268, -580, -848, -592, 108, -76, 244, 312, -716, 592, -80,
    436, 360, 4, -248, 160, 516, 584, 732, 44, -468, -280, -292, -156, -588, 28, 308,
    912, 24, 124, 156, 180, -252, 944, -924, -772, -520, -428, -624, 300, -212, -1144, 32,
    -724, 800, -1128, -212, -1288, -848, 180, -416, 440, 192, -576, -792, -76, -1080, 80, -532,
    -352, -132, 380, -820, 148, 1112, 128, 164, 456, 700, -924, 144, -668, -384, 648, -832,
    508, 552, -52, -100, -656, 208, -568, 748, -88, 680, 232, 300, 192, -408, -1012, -152,
    -252, -268, 272, -876, -664, -648, -332, -136, 16, 12, 1152, -28, 332, -536, 320, -672,
    -460, -316, 532, -260, 228, -40, 1052, -816, 180, 88, -496, -556, -672, -368, 428, 92,
    356, 404, -408, 252, 196, -176, -556, 792, 268, 32, 372, 40, 96, -332, 328, 120,
    372, -900, -40, 472, -264, -592, 952, 128, 656, 112, 664, -232, 420, 4, -344, -464,
    556, 244, -416, -32, 252, 0, -412, 188, -696, 508, -476, 324, -1096, 656, -312, 560,
    264, -136, 304, 160, -64, -580, 248, 336, -720, 560, -348, -288, -276, -196, -500, 852,
    -544, -236, -1128, -992, -776, 116, 56, 52, 860, 884, 212, -12, 168, 1020, 512, -552,
    924, -148, 716, 188, 164, -340, -520, -184, 880, -152, -680, -208, -1156, -300, -528, -472,
    364, 100, -744, -1056, -32, 540, 280, 144, -676, -32, -232, -280, -224, 96, 568, -76,
    172, 148, 148, 104, 32, -296, -32, 788, -80, 32, -16, 280, 288, 944, 428, -484,
};

// The xorshift the grain offsets and the texture seed share.
struct Rng {
    unsigned state = 0;
    unsigned get(unsigned bits)
    {
        const unsigned r = state;
        const unsigned bit = ((r >> 0) ^ (r >> 1) ^ (r >> 3) ^ (r >> 12)) & 1u;
        state = (r >> 1) | (bit << 15);
        return (state >> (16 - bits)) & ((1u << bits) - 1u);
    }
};

int rnd(int value, unsigned shift) { return (value + ((1 << shift) >> 1)) >> shift; }
int clip(int value, int lo, int hi) { return value < lo ? lo : value > hi ? hi : value; }

// One grain texture. The chroma textures fill the top 38 rows of the same
// 82-wide rows; the sampling code never reads past the columns they fill.
struct Lut {
    int16_t value[kGrainHeight * kGrainWidth] = {};
    int16_t& operator()(unsigned row, unsigned col) { return value[row * kGrainWidth + col]; }
    int operator()(unsigned row, unsigned col) const { return value[row * kGrainWidth + col]; }
};

void generate_y(Lut& lut, const GrainParams& p, unsigned bd8)
{
    const unsigned shift = 4 - bd8 + p.grain_scale_shift;
    const int ctr = 128 << bd8;
    const int lo = -ctr, hi = ctr - 1;
    Rng rng { p.seed };
    for (unsigned row = 0; row < kGrainHeight; ++row)
        for (unsigned col = 0; col < kGrainWidth; ++col)
            lut(row, col) = static_cast<int16_t>(rnd(kGaussian[rng.get(11)], shift));

    const int lag = static_cast<int>(p.ar_coeff_lag);
    for (unsigned row = 3; row < kGrainHeight; ++row) {
        for (unsigned col = 3; col < kGrainWidth - 3; ++col) {
            int sum = 0;
            int c = 0;
            for (int dy = -lag; dy <= 0; ++dy) {
                for (int dx = -lag; dx <= lag; ++dx) {
                    if (!dx && !dy)
                        break;
                    sum += p.ar_y[c++] * lut(row + dy, col + dx);
                }
            }
            lut(row, col) = static_cast<int16_t>(clip(lut(row, col) + rnd(sum, p.ar_coeff_shift), lo, hi));
        }
    }
}

// The chroma texture is seeded from a different constant per plane and, at
// the autoregressive filter's current sample, folds in the average of the
// luma texture it covers.
void generate_uv(Lut& lut, const Lut& luma, const GrainParams& p, unsigned bd8, bool v)
{
    const unsigned shift = 4 - bd8 + p.grain_scale_shift;
    const int ctr = 128 << bd8;
    const int lo = -ctr, hi = ctr - 1;
    Rng rng { p.seed ^ (v ? 0x49d8u : 0xb524u) };
    for (unsigned row = 0; row < kSubGrainHeight; ++row)
        for (unsigned col = 0; col < kSubGrainWidth; ++col)
            lut(row, col) = static_cast<int16_t>(rnd(kGaussian[rng.get(11)], shift));

    const int lag = static_cast<int>(p.ar_coeff_lag);
    const int8_t* coeffs = v ? p.ar_cr : p.ar_cb;
    for (unsigned row = 3; row < kSubGrainHeight; ++row) {
        for (unsigned col = 3; col < kSubGrainWidth - 3; ++col) {
            int sum = 0;
            int c = 0;
            for (int dy = -lag; dy <= 0; ++dy) {
                for (int dx = -lag; dx <= lag; ++dx) {
                    if (!dx && !dy) {
                        if (!p.num_y)
                            break;
                        int luma_sum = 0;
                        const unsigned luma_col = ((col - 3) << 1) + 3;
                        const unsigned luma_row = ((row - 3) << 1) + 3;
                        for (unsigned i = 0; i <= 1; ++i)
                            for (unsigned j = 0; j <= 1; ++j)
                                luma_sum += luma(luma_row + i, luma_col + j);
                        sum += rnd(luma_sum, 2) * coeffs[c];
                        break;
                    }
                    sum += coeffs[c++] * lut(row + dy, col + dx);
                }
            }
            lut(row, col) = static_cast<int16_t>(clip(lut(row, col) + rnd(sum, p.ar_coeff_shift), lo, hi));
        }
    }
}

// The piecewise-linear curve that scales grain by the picture's own code. The
// ten-bit form is the eight-bit curve stretched onto 1024 entries: every
// control point moves to its code << 2 and the gaps in between are filled by
// the same linear interpolation at the finer pitch.
std::vector<uint8_t> scaling_lut(const uint8_t* value, const uint8_t* scale, unsigned num, unsigned bit_depth)
{
    const unsigned size = 1u << bit_depth;
    std::vector<uint8_t> lut(size, 0);
    if (num == 0)
        return lut;
    // The curve is defined over the 8-bit sample range; a deeper picture
    // interpolates between neighbouring control points of that curve, it does
    // not run the same walk over a wider index. Running the walk in the wider
    // domain stretches every segment and rounds each step separately, which
    // lands up to one scaling step away from the curve the spec defines.
    uint8_t base[256] = {};
    std::memset(base, scale[0], value[0]);
    for (unsigned i = 0; i + 1 < num; ++i) {
        const int bx = value[i], by = scale[i];
        const int ex = value[i + 1], ey = scale[i + 1];
        const int dx = ex - bx, dy = ey - by;
        const int delta = dy * ((0x10000 + (dx >> 1)) / dx);
        for (int x = 0, d = 0x8000; x < dx; ++x) {
            base[bx + x] = static_cast<uint8_t>(by + (d >> 16));
            d += delta;
        }
    }
    std::memset(base + value[num - 1], scale[num - 1], sizeof base - value[num - 1]);

    const unsigned shift = bit_depth - 8;
    for (unsigned i = 0; i < size; ++i) {
        const unsigned x = i >> shift, rem = i - (x << shift);
        if (!shift || x == 255) {
            lut[i] = base[x];
            continue;
        }
        // The curve rises and falls, so the step is signed: an unsigned
        // difference turns a falling segment into a wrapped value.
        const int range = base[x + 1] - base[x];
        lut[i] = static_cast<uint8_t>(base[x] + ((range * static_cast<int>(rem) + (1 << (shift - 1))) >> shift));
    }
    return lut;
}

// The block's texture offset, chosen from the offsets the picture's row seeds
// produced for this block and the one before it.
int sample(const Lut& lut, const int (&offsets)[2][2], unsigned subx, unsigned suby, unsigned bx, unsigned by,
    unsigned x, unsigned y)
{
    const int randval = offsets[bx][by];
    const int offx = 3 + static_cast<int>(2u >> subx) * (3 + (randval >> 4));
    const int offy = 3 + static_cast<int>(2u >> suby) * (3 + (randval & 0xF));
    return lut(offy + y + ((kBlock >> suby) * by), offx + x + ((kBlock >> subx) * bx));
}

// How a sample sits in memory. P010 keeps its ten bits in the top of a 16-bit
// word, the layout V4L2 and VA name; NV12 is one byte per sample.
struct Nv12 {
    static constexpr unsigned kDepth = 8;
    static constexpr unsigned kBytes = 1;
    static unsigned load(const uint8_t* p) { return *p; }
    static void store(uint8_t* p, unsigned v) { *p = static_cast<uint8_t>(v); }
};

struct P010 {
    static constexpr unsigned kDepth = 10;
    static constexpr unsigned kBytes = 2;
    static unsigned load(const uint8_t* p)
    {
        uint16_t word;
        std::memcpy(&word, p, sizeof word);
        return word >> 6;
    }
    static void store(uint8_t* p, unsigned v)
    {
        const uint16_t word = static_cast<uint16_t>(v << 6);
        std::memcpy(p, &word, sizeof word);
    }
};

template <class S>
void blend_luma(const GrainParams& p, const Lut& lut, const std::vector<uint8_t>& scaling, const uint8_t* source,
    uint8_t* destination, unsigned source_stride, unsigned destination_stride, unsigned width, unsigned height)
{
    const unsigned bd8 = S::kDepth - 8;
    const int ctr = 128 << bd8;
    const int grain_min = -ctr, grain_max = ctr - 1;
    const int min_value = p.clip_to_restricted_range ? static_cast<int>(16 << bd8) : 0;
    const int max_value = p.clip_to_restricted_range ? static_cast<int>(235 << bd8)
                                                     : static_cast<int>((1u << S::kDepth) - 1);
    // Two blocks sharing a corner blend along both edges; the weights are the
    // AV1 syntax's 27/32 and 17/32 split in luma and 23/32 and 22/32 in chroma.
    constexpr int weight[2][2] = { { 27, 17 }, { 17, 27 } };
    const unsigned rows = (height + kBlock - 1) / kBlock;
    for (unsigned row = 0; row < rows; ++row) {
        const unsigned plane_rows = 1 + (p.overlap && row > 0);
        Rng rng[2];
        for (unsigned i = 0; i < plane_rows; ++i) {
            unsigned seed = p.seed;
            seed ^= ((((row - i) * 37) + 178) & 0xFFu) << 8;
            seed ^= (((row - i) * 173) + 105) & 0xFFu;
            rng[i].state = seed;
        }
        const unsigned bh = std::min(kBlock, height - row * kBlock);
        int offsets[2][2] = {};
        for (unsigned bx = 0; bx < width; bx += kBlock) {
            const unsigned bw = std::min(kBlock, width - bx);
            if (p.overlap && bx)
                for (unsigned i = 0; i < plane_rows; ++i)
                    offsets[1][i] = offsets[0][i];
            for (unsigned i = 0; i < plane_rows; ++i)
                offsets[0][i] = static_cast<int>(rng[i].get(8));
            const unsigned ystart = (p.overlap && row) ? std::min(2u, bh) : 0;
            const unsigned xstart = (p.overlap && bx) ? std::min(2u, bw) : 0;
            for (unsigned y = 0; y < bh; ++y) {
                const size_t row_offset = static_cast<size_t>(row * kBlock + y);
                const uint8_t* src = source + row_offset * source_stride + static_cast<size_t>(bx) * S::kBytes;
                uint8_t* dst = destination + row_offset * destination_stride + static_cast<size_t>(bx) * S::kBytes;
                for (unsigned x = 0; x < bw; ++x) {
                    int grain;
                    if (y < ystart) {
                        if (x < xstart) {
                            int top = sample(lut, offsets, 0, 0, 0, 1, x, y);
                            int old = sample(lut, offsets, 0, 0, 1, 1, x, y);
                            top = clip(rnd(old * weight[x][0] + top * weight[x][1], 5), grain_min, grain_max);
                            grain = sample(lut, offsets, 0, 0, 0, 0, x, y);
                            old = sample(lut, offsets, 0, 0, 1, 0, x, y);
                            grain = clip(rnd(old * weight[x][0] + grain * weight[x][1], 5), grain_min, grain_max);
                            grain = clip(rnd(top * weight[y][0] + grain * weight[y][1], 5), grain_min, grain_max);
                        } else {
                            grain = sample(lut, offsets, 0, 0, 0, 0, x, y);
                            const int old = sample(lut, offsets, 0, 0, 0, 1, x, y);
                            grain = clip(rnd(old * weight[y][0] + grain * weight[y][1], 5), grain_min, grain_max);
                        }
                    } else if (x < xstart) {
                        grain = sample(lut, offsets, 0, 0, 0, 0, x, y);
                        const int old = sample(lut, offsets, 0, 0, 1, 0, x, y);
                        grain = clip(rnd(old * weight[x][0] + grain * weight[x][1], 5), grain_min, grain_max);
                    } else {
                        grain = sample(lut, offsets, 0, 0, 0, 0, x, y);
                    }
                    const unsigned sample_value = S::load(src + static_cast<size_t>(x) * S::kBytes);
                    const int noise = rnd(scaling[sample_value] * grain, p.scaling_shift);
                    S::store(dst + static_cast<size_t>(x) * S::kBytes,
                        static_cast<unsigned>(clip(static_cast<int>(sample_value) + noise, min_value, max_value)));
                }
            }
        }
    }
}

template <class S>
void blend_chroma(const GrainParams& p, const Lut& cb_lut, const Lut& cr_lut,
    const std::vector<uint8_t>& scaling_luma, const std::vector<uint8_t>& scaling_cb,
    const std::vector<uint8_t>& scaling_cr, const uint8_t* source_luma, const uint8_t* source_uv,
    uint8_t* destination_uv, unsigned luma_stride, unsigned source_stride, unsigned destination_stride,
    unsigned width, unsigned height)
{
    const unsigned bd8 = S::kDepth - 8;
    const int ctr = 128 << bd8;
    const int grain_min = -ctr, grain_max = ctr - 1;
    constexpr int chroma_weight[2][2] = { { 23, 22 }, { 23, 22 } };
    const unsigned cpw = (width + 1) >> 1;
    const unsigned rows = (height + kBlock - 1) / kBlock;
    for (unsigned row = 0; row < rows; ++row) {
        const unsigned bh = (std::min(kBlock, height - row * kBlock) + 1) >> 1;
        for (unsigned plane = 0; plane < 2; ++plane) {
            const bool scaled_from_luma = p.chroma_scaling_from_luma;
            if (!scaled_from_luma && (plane ? p.num_cr : p.num_cb) == 0)
                continue;
            const Lut& lut = plane ? cr_lut : cb_lut;
            const std::vector<uint8_t>& scaling = scaled_from_luma ? scaling_luma : (plane ? scaling_cr : scaling_cb);
            const int mult = plane ? p.cr_mult : p.cb_mult;
            const int luma_mult = plane ? p.cr_luma_mult : p.cb_luma_mult;
            const int offset = plane ? p.cr_offset : p.cb_offset;
            const int min_value = p.clip_to_restricted_range ? static_cast<int>(16 << bd8) : 0;
            const int max_value = p.clip_to_restricted_range ? static_cast<int>((p.identity ? 235 : 240) << bd8)
                                                             : static_cast<int>((1u << S::kDepth) - 1);
            const unsigned chroma_rows = 1 + (p.overlap && row > 0);
            Rng rng[2];
            for (unsigned i = 0; i < chroma_rows; ++i) {
                unsigned seed = p.seed;
                seed ^= ((((row - i) * 37) + 178) & 0xFFu) << 8;
                seed ^= (((row - i) * 173) + 105) & 0xFFu;
                rng[i].state = seed;
            }
            int offsets[2][2] = {};
            for (unsigned bx = 0; bx < cpw; bx += kBlock >> 1) {
                const unsigned bw = std::min(kBlock >> 1, cpw - bx);
                if (p.overlap && bx)
                    for (unsigned i = 0; i < chroma_rows; ++i)
                        offsets[1][i] = offsets[0][i];
                for (unsigned i = 0; i < chroma_rows; ++i)
                    offsets[0][i] = static_cast<int>(rng[i].get(8));
                const unsigned ystart = (p.overlap && row) ? std::min(1u, bh) : 0;
                const unsigned xstart = (p.overlap && bx) ? std::min(1u, bw) : 0;
                for (unsigned y = 0; y < bh; ++y) {
                    const size_t chroma_row = static_cast<size_t>(row * (kBlock / 2) + y);
                    const size_t luma_row_index = static_cast<size_t>(row * kBlock + 2 * y);
                    const uint8_t* src = source_uv + chroma_row * source_stride
                        + static_cast<size_t>(bx) * 2 * S::kBytes;
                    uint8_t* dst = destination_uv + chroma_row * destination_stride
                        + static_cast<size_t>(bx) * 2 * S::kBytes;
                    const uint8_t* luma = source_luma + luma_row_index * luma_stride;
                    for (unsigned x = 0; x < bw; ++x) {
                        int grain;
                        if (y < ystart) {
                            if (x < xstart) {
                                int top = sample(lut, offsets, 1, 1, 0, 1, x, y);
                                int old = sample(lut, offsets, 1, 1, 1, 1, x, y);
                                top = clip(rnd(old * chroma_weight[x][0] + top * chroma_weight[x][1], 5),
                                    grain_min, grain_max);
                                grain = sample(lut, offsets, 1, 1, 0, 0, x, y);
                                old = sample(lut, offsets, 1, 1, 1, 0, x, y);
                                grain = clip(rnd(old * chroma_weight[x][0] + grain * chroma_weight[x][1], 5),
                                    grain_min, grain_max);
                                grain = clip(rnd(top * chroma_weight[y][0] + grain * chroma_weight[y][1], 5),
                                    grain_min, grain_max);
                            } else {
                                grain = sample(lut, offsets, 1, 1, 0, 0, x, y);
                                const int old = sample(lut, offsets, 1, 1, 0, 1, x, y);
                                grain = clip(rnd(old * chroma_weight[y][0] + grain * chroma_weight[y][1], 5),
                                    grain_min, grain_max);
                            }
                        } else if (x < xstart) {
                            grain = sample(lut, offsets, 1, 1, 0, 0, x, y);
                            const int old = sample(lut, offsets, 1, 1, 1, 0, x, y);
                            grain = clip(rnd(old * chroma_weight[x][0] + grain * chroma_weight[x][1], 5),
                                grain_min, grain_max);
                        } else {
                            grain = sample(lut, offsets, 1, 1, 0, 0, x, y);
                        }
                        // The scaling index of a chroma sample is its own code
                        // mixed with the luma the sample covers, both read
                        // ungrain: the spec blends noise with the original
                        // picture, and the luma plane is grained afterwards.
                        const unsigned col = 2 * (bx + x);
                        const unsigned col_next = std::min(col + 1, width - 1);
                        int average = S::load(luma + static_cast<size_t>(col) * S::kBytes);
                        average = (average + static_cast<int>(S::load(luma + static_cast<size_t>(col_next) * S::kBytes)) + 1) >> 1;
                        int index = average;
                        if (!scaled_from_luma) {
                            const unsigned code = S::load(src + static_cast<size_t>(2 * x + plane) * S::kBytes);
                            index = clip(((average * luma_mult + static_cast<int>(code) * mult) >> 6)
                                    + (offset << bd8),
                                0, static_cast<int>((1u << S::kDepth) - 1));
                        }
                        const unsigned source_value = S::load(src + static_cast<size_t>(2 * x + plane) * S::kBytes);
                        const int noise = rnd(scaling[index] * grain, p.scaling_shift);
                        S::store(dst + static_cast<size_t>(2 * x + plane) * S::kBytes,
                            static_cast<unsigned>(clip(static_cast<int>(source_value) + noise, min_value, max_value)));
                    }
                }
            }
        }
    }
}

template <class S>
void blend(const GrainParams& p, const iris::Layout& source, const uint8_t* src, const iris::Layout& destination,
    uint8_t* dst)
{
    const unsigned bd8 = S::kDepth - 8;
    Lut luma, cb, cr;
    generate_y(luma, p, bd8);
    std::vector<uint8_t> scaling_luma, scaling_cb, scaling_cr;
    if (p.num_cb || p.chroma_scaling_from_luma)
        generate_uv(cb, luma, p, bd8, false);
    if (p.num_cr || p.chroma_scaling_from_luma)
        generate_uv(cr, luma, p, bd8, true);
    if (p.num_y || p.chroma_scaling_from_luma)
        scaling_luma = scaling_lut(p.y_value, p.y_scaling, p.num_y, S::kDepth);
    if (p.num_cb)
        scaling_cb = scaling_lut(p.cb_value, p.cb_scaling, p.num_cb, S::kDepth);
    if (p.num_cr)
        scaling_cr = scaling_lut(p.cr_value, p.cr_scaling, p.num_cr, S::kDepth);

    const uint8_t* src_luma = src + source.data_offset;
    const uint8_t* src_uv = src + source.chroma_offset();
    uint8_t* dst_luma = dst + destination.data_offset;
    uint8_t* dst_uv = dst + destination.chroma_offset();
    if (p.num_y)
        blend_luma<S>(p, luma, scaling_luma, src_luma, dst_luma, source.stride, destination.stride, source.width,
            source.height);
    if (p.num_cb || p.num_cr || p.chroma_scaling_from_luma) {
        blend_chroma<S>(p, cb, cr, scaling_luma, scaling_cb, scaling_cr, src_luma, src_uv, dst_uv, source.stride,
            source.stride, destination.stride, source.width, source.height);
    } else {
        // No chroma grain: the chroma planes are the source's own.
        const unsigned bytes = S::kBytes;
        const unsigned rows = (source.height + 1) / 2;
        const unsigned row_bytes = ((source.width + 1) & ~1u) * bytes;
        for (unsigned row = 0; row < rows; ++row)
            std::memcpy(dst_uv + static_cast<size_t>(row) * destination.stride,
                src_uv + static_cast<size_t>(row) * source.stride, row_bytes);
    }
    if (!p.num_y) {
        const unsigned bytes = S::kBytes;
        const unsigned row_bytes = source.width * bytes;
        for (unsigned row = 0; row < source.height; ++row)
            std::memcpy(dst_luma + static_cast<size_t>(row) * destination.stride,
                src_luma + static_cast<size_t>(row) * source.stride, row_bytes);
    }
}

} // namespace

GrainParams grain_params(const VAFilmGrainStructAV1& grain, unsigned bit_depth, unsigned matrix_coefficients)
{
    const auto f = grain.film_grain_info_fields.bits;
    GrainParams p;
    p.bit_depth = bit_depth;
    p.identity = matrix_coefficients == 0;
    p.seed = grain.grain_seed;
    p.scaling_shift = f.grain_scaling_minus_8 + 8;
    p.ar_coeff_shift = f.ar_coeff_shift_minus_6 + 6;
    p.ar_coeff_lag = f.ar_coeff_lag;
    p.grain_scale_shift = f.grain_scale_shift;
    p.overlap = f.overlap_flag != 0;
    p.clip_to_restricted_range = f.clip_to_restricted_range != 0;
    p.chroma_scaling_from_luma = f.chroma_scaling_from_luma != 0;
    p.num_y = std::min<unsigned>(grain.num_y_points, 14);
    p.num_cb = std::min<unsigned>(grain.num_cb_points, 10);
    p.num_cr = std::min<unsigned>(grain.num_cr_points, 10);
    std::memcpy(p.y_value, grain.point_y_value, p.num_y);
    std::memcpy(p.y_scaling, grain.point_y_scaling, p.num_y);
    std::memcpy(p.cb_value, grain.point_cb_value, p.num_cb);
    std::memcpy(p.cb_scaling, grain.point_cb_scaling, p.num_cb);
    std::memcpy(p.cr_value, grain.point_cr_value, p.num_cr);
    std::memcpy(p.cr_scaling, grain.point_cr_scaling, p.num_cr);
    std::memcpy(p.ar_y, grain.ar_coeffs_y, sizeof p.ar_y);
    std::memcpy(p.ar_cb, grain.ar_coeffs_cb, sizeof p.ar_cb);
    std::memcpy(p.ar_cr, grain.ar_coeffs_cr, sizeof p.ar_cr);
    // The chroma mixing weights travel as biased unsigned codes: the syntax
    // stores mult + 128 and offset + 256, and the synthesis uses the signed
    // difference. Passing the code through unmoved shifts every chroma index
    // by a constant, which lands inside the clip range and produces plausible
    // but wrong noise, so it never shows up as an error until a hash does.
    p.cb_mult = static_cast<int>(grain.cb_mult) - 128;
    p.cb_luma_mult = static_cast<int>(grain.cb_luma_mult) - 128;
    p.cb_offset = static_cast<int>(grain.cb_offset) - 256;
    p.cr_mult = static_cast<int>(grain.cr_mult) - 128;
    p.cr_luma_mult = static_cast<int>(grain.cr_luma_mult) - 128;
    p.cr_offset = static_cast<int>(grain.cr_offset) - 256;
    return p;
}

bool apply_grain(const GrainParams& params, const iris::Layout& source, const uint8_t* src,
    const iris::Layout& destination, uint8_t* dst)
{
    if (source.fourcc != destination.fourcc || source.width != destination.width || source.height != destination.height)
        return false;
    if (source.width == 0 || source.height == 0)
        return false;
    if (source.stride < source.width * (source.fourcc == VA_FOURCC_P010 ? 2u : 1u))
        return false;
    if (destination.stride < source.stride)
        return false;
    if (source.chroma_offset() + static_cast<size_t>(source.stride) * ((source.height + 1) / 2) > source.size)
        return false;
    if (destination.chroma_offset() + static_cast<size_t>(destination.stride) * ((destination.height + 1) / 2)
        > destination.size)
        return false;
    if (source.fourcc == VA_FOURCC_NV12)
        blend<Nv12>(params, source, src, destination, dst);
    else if (source.fourcc == VA_FOURCC_P010)
        blend<P010>(params, source, src, destination, dst);
    else
        return false;
    return true;
}

} // namespace va
