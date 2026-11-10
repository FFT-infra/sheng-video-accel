// Host-only tests for the experimental internal-DPB AV1 translator. Nothing
// here touches the shipped held-picture path selected by make_av1().
#include "../src/codec/av1_dpb.h"
#include "../src/codec/translator.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

constexpr unsigned kSlots = 8;

int failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

VADecPictureParameterBufferAV1 params(unsigned surface, bool key, unsigned hint)
{
    VADecPictureParameterBufferAV1 p = {};
    p.profile = 0;
    auto& seq = p.seq_info_fields.fields;
    seq.enable_order_hint = 0;
    seq.subsampling_x = 1;
    seq.subsampling_y = 1;
    p.order_hint_bits_minus_1 = 0;
    p.frame_width_minus1 = 63;
    p.frame_height_minus1 = 63;
    auto& pic = p.pic_info_fields.bits;
    pic.frame_type = key ? 0 : 1;
    pic.show_frame = 1;
    pic.error_resilient_mode = 1;
    pic.disable_cdf_update = 1;
    pic.uniform_tile_spacing_flag = 1;
    p.tile_cols = 1;
    p.tile_rows = 1;
    p.base_qindex = 54;
    p.mode_control_fields.bits.tx_mode = 2;
    p.order_hint = hint;
    p.primary_ref_frame = 7;
    p.current_frame = surface;
    return p;
}

struct Obu {
    unsigned type;
    size_t size;
};

std::vector<Obu> obus(const std::vector<uint8_t>& data)
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
        out.push_back({ type, size });
        i = c + size;
    }
    return out;
}

void translate(const VADecPictureParameterBufferAV1& p,
    iris::codec::Translator& t, iris::codec::AccessUnit& out)
{
    const uint8_t tile[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    VASliceParameterBufferAV1 slice = {};
    slice.slice_data_size = sizeof(tile);
    iris::codec::Picture picture;
    picture.parameters = &p;
    picture.slice_parameters = { &slice };
    picture.slice_data = { { tile, sizeof(tile) } };
    picture.surface_id = p.current_frame;
    t.translate(picture, out);
}

// Every vaEndPicture must produce a unit immediately: no hold, no deferred.
void test_no_hold()
{
    auto t = iris::codec::make_av1_dpb(VAProfileAV1Profile0);
    iris::codec::AccessUnit out;
    auto key = params(10, true, 0);
    for (unsigned i = 0; i < kSlots; ++i)
        key.ref_frame_map[i] = VA_INVALID_SURFACE;
    translate(key, *t, out);
    check(!out.deferred, "key frame is submitted immediately");
    check(!out.bytes.empty(), "key frame produces bytes");
    check(out.completions == 1, "key frame produces one completion");
    check(t->held_surface() == VA_INVALID_ID, "DPB translator holds no surface");
    check(!t->flush(out), "DPB translator has no tail to flush");
}

// Eight inter frames each take one slot; each one emits a unit and refreshes
// exactly one slot. The ninth frame finds all eight busy.
void test_slot_exhaustion_and_install()
{
    auto t = iris::codec::make_av1_dpb(VAProfileAV1Profile0);
    iris::codec::AccessUnit out;

    auto key = params(100, true, 0);
    for (unsigned i = 0; i < kSlots; ++i)
        key.ref_frame_map[i] = VA_INVALID_SURFACE;
    translate(key, *t, out);

    // Frame 2..8 reference every surface decoded so far, so each new frame
    // takes the next free slot and the DPB fills to eight slots.
    for (unsigned frame = 2; frame <= 8; ++frame) {
        auto p = params(100 + frame - 1, false, frame - 1);
        for (unsigned i = 0; i < kSlots; ++i)
            p.ref_frame_map[i] = i < frame - 1 ? static_cast<VASurfaceID>(100 + i)
                : VA_INVALID_SURFACE;
        for (unsigned i = 0; i < 7; ++i)
            p.ref_frame_idx[i] = 0;
        translate(p, *t, out);
        check(!out.deferred, "inter frame is submitted immediately");
        check(!out.bytes.empty(), "inter frame produces bytes");
    }

    // Frame 9 names all eight surfaces: no slot is free, so this picture is
    // decoded without refreshing anything and kept as the pending reference.
    auto full = params(108, false, 8);
    for (unsigned i = 0; i < kSlots; ++i)
        full.ref_frame_map[i] = 100 + i;
    for (unsigned i = 0; i < 7; ++i)
        full.ref_frame_idx[i] = 0;
    translate(full, *t, out);
    check(!out.deferred, "full-DPB frame is still submitted immediately");
    check(out.completions == 1, "full-DPB frame has no install replay yet");

    // Frame 10 drops its reference to slot 1 but still names the pending
    // frame's surface (108). The pending picture is replayed as a hidden
    // install frame in one two-FRAME temporal unit.
    auto p = params(109, false, 9);
    for (unsigned i = 0; i < kSlots; ++i)
        p.ref_frame_map[i] = 100 + i;
    p.ref_frame_map[1] = VA_INVALID_SURFACE;
    p.ref_frame_map[7] = 108;
    for (unsigned i = 0; i < 7; ++i)
        p.ref_frame_idx[i] = 0;
    translate(p, *t, out);
    check(!out.deferred, "full-DPB frame is submitted immediately");
    check(out.completions == 2, "install unit declares two completions");
    const auto u = obus(out.bytes);
    unsigned frames = 0;
    for (const auto& o : u)
        if (o.type == 6)
            ++frames;
    check(frames == 2, "install unit contains two FRAME OBUs");
    check(u.size() == 3 || u.size() == 4, "one TD, optional SEQ, two FRAMEs");
}

} // namespace

int main()
{
    try {
        test_no_hold();
        test_slot_exhaustion_and_install();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " AV1 DPB check(s) failed\n";
        return 1;
    }
    std::cout << "PASS AV1 internal-DPB translator (no hold, slot exhaustion, install unit)\n";
    return 0;
}
