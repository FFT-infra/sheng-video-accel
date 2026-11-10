// Host-only regression for the multi-frame temporal unit builder.
//
// Two independent properties are checked:
//
// 1. Refactoring safety: the shown frame built by the new two-frame builder
//    must be byte-identical to the frame av1_build_temporal_unit() emits for
//    the same parameters and tiles. Both go through the shared frame-OBU
//    helper, so the shipped single-frame path cannot silently diverge.
//
// 2. Structure: a two-frame temporal unit has one temporal delimiter, one
//    sequence header, and two OBU_FRAMEs; the hidden frame's header carries
//    show_frame=0 and the shown frame's carries show_frame=1. libdav1d
//    decoding of a real two-frame fixture is checked by the shell half of
//    this test, because arbitrary tile bytes are not valid AV1 entropy data.

#include "../src/codec/av1_obu.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <va/va.h>
#include <va/va_dec_av1.h>
}

namespace {

int failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

struct Obu {
    unsigned type;
    std::vector<uint8_t> payload;
};

std::vector<Obu> parse(const std::vector<uint8_t>& data)
{
    std::vector<Obu> out;
    size_t i = 0;
    while (i < data.size()) {
        const uint8_t h = data[i];
        const unsigned type = (h >> 3) & 0xf;
        const bool ext = (h >> 2) & 1;
        const bool has_size = (h >> 1) & 1;
        if (!has_size)
            throw std::runtime_error("OBU without size field");
        size_t c = i + 1 + (ext ? 1 : 0);
        size_t size = 0;
        unsigned shift = 0;
        while (true) {
            if (c >= data.size())
                throw std::runtime_error("truncated OBU size");
            const uint8_t b = data[c++];
            size |= static_cast<size_t>(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80))
                break;
        }
        if (c + size > data.size())
            throw std::runtime_error("truncated OBU payload");
        out.push_back({ type, { data.begin() + c, data.begin() + c + size } });
        i = c + size;
    }
    return out;
}

VADecPictureParameterBufferAV1 key_frame(bool show, unsigned width, unsigned height)
{
    VADecPictureParameterBufferAV1 p = {};
    p.profile = 0;
    auto& seq = p.seq_info_fields.fields;
    seq.enable_order_hint = 0;
    seq.subsampling_x = 1;
    seq.subsampling_y = 1;
    p.order_hint_bits_minus_1 = 0;
    p.frame_width_minus1 = width - 1;
    p.frame_height_minus1 = height - 1;
    auto& pic = p.pic_info_fields.bits;
    pic.frame_type = 0; // KEY_FRAME
    pic.show_frame = show ? 1 : 0;
    pic.error_resilient_mode = 1;
    pic.disable_cdf_update = 1;
    pic.uniform_tile_spacing_flag = 1;
    p.tile_cols = 1;
    p.tile_rows = 1;
    p.base_qindex = 0;
    p.mode_control_fields.bits.tx_mode = 2;
    return p;
}

} // namespace

int main()
{
    try {
        const auto shown = key_frame(true, 64, 64);
        const auto hidden = key_frame(false, 64, 64);
        const AV1SequenceState seq = av1_sequence_state(shown);
        const AV1ReferenceState refs;
        const std::vector<uint8_t> tile(16, 0x00); // structure only, not decodable tiles
        const std::vector<AV1TileEntry> tiles = { { 0, 16 } };

        std::vector<uint8_t> single;
        const char* reject = nullptr;
        check(av1_build_temporal_unit(single, shown, seq, refs, true, 0xff,
            tile.data(), tiles, &reject), "single-frame temporal unit builds");
        const auto single_obus = parse(single);
        check(single_obus.size() == 3, "TD + SEQ + one FRAME");
        if (single_obus.size() == 3) {
            check(single_obus[0].type == 2 && single_obus[1].type == 1 && single_obus[2].type == 6,
                "single-frame OBU order");
            check((single_obus[2].payload[0] & 0x10) != 0, "single frame is shown");
        }

        std::vector<uint8_t> install;
        check(av1_build_install_temporal_unit(install,
            hidden, seq, refs, 1, tile.data(), tiles,
            shown, seq, refs, 0xff, tile.data(), tiles,
            true, &reject), "install temporal unit builds");
        const auto obus = parse(install);
        check(obus.size() == 4, "TD + SEQ + two FRAME OBUs");
        if (obus.size() == 4) {
            check(obus[0].type == 2 && obus[1].type == 1, "TD then SEQ once each");
            check(obus[2].type == 6 && obus[3].type == 6, "two FRAME OBUs");
            check((obus[2].payload[0] & 0x10) == 0, "hidden frame has show_frame=0");
            check((obus[3].payload[0] & 0x10) != 0, "shown frame has show_frame=1");
        }
        // The single-frame path must be unchanged by the split: compare the
        // shown frame bytes from both units, since both used identical
        // parameters and tiles.
        check(single_obus.size() == 3 && obus.size() == 4
            && single_obus[2].payload == obus[3].payload,
            "shown frame bytes identical between old and new builders");
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "PASS AV1 install temporal unit structure and single-frame byte parity\n";
    return 0;
}
