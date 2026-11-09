// Exercise the emitted bitstream, not the order of source-code tokens.
#include "../src/codec/bitwriter.h"
#include "../src/codec/translator.h"

using iris::codec::hevc_sps;

#include <iostream>
#include <stdexcept>

namespace {

class SpsReader {
public:
    explicit SpsReader(const std::vector<uint8_t>& nal)
    {
        if (nal.size() < 7 || nal[0] != 0 || nal[1] != 0 || nal[2] != 0
            || nal[3] != 1 || (nal[4] >> 1) != 33)
            throw std::runtime_error("missing Annex-B HEVC SPS");
        unsigned zeros = 0;
        for (size_t i = 6; i < nal.size(); ++i) {
            if (zeros == 2 && nal[i] == 3) {
                zeros = 0;
                continue;
            }
            bytes_.push_back(nal[i]);
            zeros = nal[i] == 0 ? zeros + 1 : 0;
        }
    }

    unsigned bits(unsigned count)
    {
        if (count > 32 || offset_ + count > bytes_.size() * 8)
            throw std::runtime_error("truncated SPS");
        unsigned value = 0;
        while (count--) {
            value = (value << 1) | ((bytes_[offset_ / 8] >> (7 - offset_ % 8)) & 1);
            ++offset_;
        }
        return value;
    }

    void skip(unsigned count)
    {
        while (count > 32) {
            bits(32);
            count -= 32;
        }
        bits(count);
    }

    unsigned ue()
    {
        unsigned zeros = 0;
        while (bits(1) == 0) {
            if (++zeros >= 31)
                throw std::runtime_error("invalid Exp-Golomb value");
        }
        return (1u << zeros) - 1 + bits(zeros);
    }

    int se()
    {
        const unsigned value = ue();
        return value & 1 ? static_cast<int>((value + 1) / 2) : -static_cast<int>(value / 2);
    }

private:
    std::vector<uint8_t> bytes_;
    size_t offset_ = 0;
};

std::pair<unsigned, unsigned> transform_depths(const std::vector<uint8_t>& nal)
{
    SpsReader reader(nal);
    reader.bits(4); // sps_video_parameter_set_id
    const unsigned layers = reader.bits(3);
    reader.bits(1); // sps_temporal_id_nesting_flag
    reader.skip(96); // general_profile_tier_level and general_level_idc
    unsigned profiles[8] {}, levels[8] {};
    for (unsigned i = 0; i < layers; ++i) {
        profiles[i] = reader.bits(1);
        levels[i] = reader.bits(1);
    }
    if (layers)
        reader.skip(2 * (8 - layers));
    for (unsigned i = 0; i < layers; ++i) {
        if (profiles[i])
            reader.skip(88);
        if (levels[i])
            reader.skip(8);
    }
    reader.ue(); // sps_seq_parameter_set_id
    if (reader.ue() == 3)
        reader.bits(1); // separate_colour_plane_flag
    reader.ue(); // pic_width_in_luma_samples
    reader.ue(); // pic_height_in_luma_samples
    if (reader.bits(1))
        for (unsigned i = 0; i < 4; ++i)
            reader.ue(); // conformance window offsets
    reader.ue(); // bit_depth_luma_minus8
    reader.ue(); // bit_depth_chroma_minus8
    reader.ue(); // log2_max_pic_order_cnt_lsb_minus4
    const unsigned first_layer = reader.bits(1) ? 0 : layers;
    for (unsigned i = first_layer; i <= layers; ++i)
        for (unsigned j = 0; j < 3; ++j)
            reader.ue(); // sub-layer ordering information
    for (unsigned i = 0; i < 4; ++i)
        reader.ue(); // coding/transform block sizes
    // HEVC 7.3.2.2.1: inter precedes intra, unlike the VA structure layout.
    const unsigned inter = reader.ue();
    const unsigned intra = reader.ue();
    return { inter, intra };
}

void check_scaling_lists()
{
    VAIQMatrixBufferHEVC matrix {};
    for (unsigned size = 0; size < 4; ++size) {
        for (unsigned id = 0; id < (size == 3 ? 2u : 6u); ++id) {
            uint8_t* values = size == 0 ? matrix.ScalingList4x4[id]
                : size == 1 ? matrix.ScalingList8x8[id]
                : size == 2 ? matrix.ScalingList16x16[id] : matrix.ScalingList32x32[id];
            for (unsigned i = 0; i < (size == 0 ? 16u : 64u); ++i)
                values[i] = 1 + (i * 17 + id * 31 + size * 47) % 255;
        }
    }
    for (unsigned id = 0; id < 6; ++id)
        matrix.ScalingListDC16x16[id] = 16 + id;
    matrix.ScalingListDC32x32[0] = 31;
    matrix.ScalingListDC32x32[1] = 253;
    // The scaling lists are reachable only through the SPS; parse past the
    // fields that precede scaling_list_data().
    VAPictureParameterBufferHEVC picture {};
    picture.pic_width_in_luma_samples = 960;
    picture.pic_height_in_luma_samples = 720;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.pic_fields.bits.NoPicReorderingFlag = 1;
    picture.pic_fields.bits.scaling_list_enabled_flag = 1;
    picture.sps_max_dec_pic_buffering_minus1 = 8;
    for (auto& reference : picture.ReferenceFrames)
        reference.flags = VA_PICTURE_HEVC_INVALID;
    SpsReader reader(hevc_sps(VAProfileHEVCMain, picture, &matrix));
    reader.bits(4);
    const unsigned layers = reader.bits(3);
    reader.bits(1);
    reader.skip(96);
    reader.skip(2 * 8);
    reader.ue();
    reader.ue(); // chroma
    reader.ue(); reader.ue(); // width, height
    if (reader.bits(1))
        for (unsigned i = 0; i < 4; ++i)
            reader.ue();
    reader.ue(); reader.ue(); reader.ue(); // bit depths, poc lsb
    const unsigned first_layer = reader.bits(1) ? 0 : layers;
    for (unsigned i = first_layer; i <= layers; ++i)
        for (unsigned j = 0; j < 3; ++j)
            reader.ue();
    for (unsigned i = 0; i < 6; ++i)
        reader.ue(); // block sizes and transform depths
    if (reader.bits(1) != 1)
        throw std::runtime_error("scaling_list_enabled_flag not set");
    if (reader.bits(1) != 1)
        throw std::runtime_error("sps_scaling_list_data_present_flag not set");
    // Fixed HEVC diagonal tables, independent of the serializer's walk.
    const unsigned scan4[] = {0,4,1,8,5,2,12,9,6,3,13,10,7,14,11,15};
    const unsigned scan8[] = {
        0,8,1,16,9,2,24,17,10,3,32,25,18,11,4,40,
        33,26,19,12,5,48,41,34,27,20,13,6,56,49,42,35,
        28,21,14,7,57,50,43,36,29,22,15,58,51,44,37,30,
        23,59,52,45,38,31,60,53,46,39,61,54,47,62,55,63};
    for (unsigned size = 0; size < 4; ++size) {
        for (unsigned id = 0; id < (size == 3 ? 2u : 6u); ++id) {
            if (reader.bits(1) != 1)
                throw std::runtime_error("missing explicit HEVC scaling coefficients");
            int previous = 8;
            if (size > 1) {
                previous = reader.se() + 8;
                const int expected = size == 2 ? matrix.ScalingListDC16x16[id]
                                               : matrix.ScalingListDC32x32[id];
                if (previous != expected)
                    throw std::runtime_error("HEVC scaling DC coefficient changed");
            }
            const uint8_t* values = size == 0 ? matrix.ScalingList4x4[id]
                : size == 1 ? matrix.ScalingList8x8[id]
                : size == 2 ? matrix.ScalingList16x16[id] : matrix.ScalingList32x32[id];
            for (unsigned i = 0; i < (size == 0 ? 16u : 64u); ++i) {
                previous = (previous + reader.se() + 256) % 256;
                if (previous != values[size == 0 ? scan4[i] : scan8[i]])
                    throw std::runtime_error("HEVC scaling raster/diagonal round trip failed");
            }
        }
    }
}

} // namespace

namespace {

// BitReader::copy takes a whole-byte path when both sides are aligned. The
// slice rewrite depends on it for 4K payloads, so check it bit-for-bit
// against a copy that never leaves the per-bit loop.
void check_bitreader_copy()
{
    std::vector<uint8_t> nal(300);
    for (size_t i = 0; i < nal.size(); ++i)
        nal[i] = static_cast<uint8_t>(i * 37 + 1);
    nal[40] = 0;
    nal[41] = 0;
    nal[42] = 3;
    nal[43] = 7;

    BitReader probe(nal.data(), nal.size(), 0);
    const size_t total = probe.size_bits();
    const size_t begins[] = { 0, 8, 3 };
    const size_t ends[] = { total, 400, 1234 };
    unsigned cases = 0;
    for (size_t begin : begins) {
        for (size_t end : ends) {
            for (bool misaligned : { false, true }) {
                BitWriter fast;
                BitWriter slow;
                if (misaligned) {
                    fast.bit(1);
                    slow.bit(1);
                }
                BitReader r(nal.data(), nal.size(), 0);
                r.seek(begin);
                r.copy(fast, end);

                BitReader s(nal.data(), nal.size(), 0);
                s.seek(begin);
                while (s.position() < end)
                    slow.bit(s.bit());

                if (r.position() != s.position() || fast.data() != slow.data()) {
                    std::cerr << "FAIL BitReader::copy differs at begin=" << begin
                              << " end=" << end << " misaligned=" << misaligned << '\n';
                    std::exit(1);
                }
                ++cases;
            }
        }
    }
    std::cout << "PASS BitReader::copy: " << cases << " aligned and unaligned spans\n";
}

} // namespace

int main()
{
    try {
        VAPictureParameterBufferHEVC picture {};
        picture.pic_width_in_luma_samples = 960;
        picture.pic_height_in_luma_samples = 720;
        picture.pic_fields.bits.chroma_format_idc = 1;
        picture.pic_fields.bits.NoPicReorderingFlag = 1;
        picture.sps_max_dec_pic_buffering_minus1 = 8;
        picture.log2_max_pic_order_cnt_lsb_minus4 = 4;
        picture.log2_diff_max_min_luma_coding_block_size = 3;
        picture.log2_diff_max_min_transform_block_size = 3;
        for (auto& reference : picture.ReferenceFrames)
            reference.flags = VA_PICTURE_HEVC_INVALID;

        for (unsigned inter = 0; inter <= 3; ++inter) {
            for (unsigned intra = 0; intra <= 3; ++intra) {
                picture.max_transform_hierarchy_depth_inter = inter;
                picture.max_transform_hierarchy_depth_intra = intra;
                const auto actual = transform_depths(hevc_sps(VAProfileHEVCMain, picture, nullptr));
                if (actual != std::pair { inter, intra }) {
                    std::cerr << "FAIL HEVC SPS transform depths: expected inter=" << inter
                              << " intra=" << intra << ", got inter=" << actual.first
                              << " intra=" << actual.second << '\n';
                    return 1;
                }
            }
        }
        std::cout << "PASS HEVC SPS transform-depth round trip: 16 combinations\n";
        check_scaling_lists();
        std::cout << "PASS HEVC scaling lists: 20 matrices, 992 coefficients and 8 DC values\n";
        check_bitreader_copy();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
