// Host-only tests for the default internal-DPB AV1 translator. make_av1()
// remains available as a one-line rollback path and has its own tests.
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
    std::vector<uint8_t> payload;
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
        out.push_back({ type, size, { data.begin() + static_cast<long>(c),
                                     data.begin() + static_cast<long>(c + size) } });
        i = c + size;
    }
    return out;
}

// MSB-first bit reader over the frame header. The tests below only read the
// fixed-position fields before any variable-length syntax.
class Bits {
public:
    explicit Bits(const std::vector<uint8_t>& data)
        : data_(data)
    {
    }
    unsigned get(unsigned count)
    {
        unsigned value = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (pos_ >= data_.size() * 8)
                throw std::runtime_error("frame header ended early");
            value = (value << 1) | ((data_[pos_ / 8] >> (7 - (pos_ % 8))) & 1u);
            ++pos_;
        }
        return value;
    }
    void skip(unsigned count) { get(count); }

private:
    const std::vector<uint8_t>& data_;
    size_t pos_ = 0;
};

// show_existing_frame(1), frame_type(2), show_frame(1), showable(1 when
// hidden), error_resilient(1 unless forced), disable_cdf(1),
// allow_screen_content(1), force_integer_mv(1 when allowed), frame_size_
// override(1), then refresh_frame_flags(8) for a non-reset inter frame.
struct FrameHeader {
    unsigned frame_type;
    bool show_frame;
    uint8_t refresh;
};

FrameHeader frame_header(const Obu& frame, bool allow_screen_content, bool error_resilient)
{
    Bits b(frame.payload);
    b.get(1); // show_existing_frame
    const unsigned type = b.get(2);
    const bool show = b.get(1) != 0;
    if (!show)
        b.get(1); // showable_frame
    b.get(1); // error_resilient_mode
    b.get(1); // disable_cdf_update
    if (allow_screen_content) {
        b.get(1); // allow_screen_content_tools
        b.get(1); // force_integer_mv
    }
    b.get(1); // frame_size_override_flag
    // The test fixtures disable order hints and set primary_ref_frame to
    // PRIMARY_REF_NONE, so no order_hint or primary bits precede refresh.
    const uint8_t refresh = static_cast<uint8_t>(b.get(8));
    (void)error_resilient;
    return { type, show, refresh };
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
    std::vector<FrameHeader> headers;
    for (const auto& o : u)
        if (o.type == 6)
            headers.push_back(frame_header(o, false, true));
    check(headers.size() == 2, "two frame headers are parseable");
    check(!headers[0].show_frame, "install frame is hidden");
    check(headers[1].show_frame, "shown frame is shown");
    check(headers[0].refresh != 0, "install frame refreshes a slot");
    check(headers[0].refresh != headers[1].refresh,
        "hidden install and shown frame must not refresh the same slot");
}

// The successor must be able to name the pending picture through a real
// ref_frame_idx value. The install has to register that picture in the
// internal DPB *before* the shown frame's references are resolved.
void test_install_resolves_references_to_pending_surface()
{
    auto t = iris::codec::make_av1_dpb(VAProfileAV1Profile0);
    iris::codec::AccessUnit out;

    auto key = params(100, true, 0);
    for (unsigned i = 0; i < kSlots; ++i)
        key.ref_frame_map[i] = VA_INVALID_SURFACE;
    translate(key, *t, out);

    for (unsigned frame = 2; frame <= 8; ++frame) {
        auto p = params(100 + frame - 1, false, frame - 1);
        for (unsigned i = 0; i < kSlots; ++i)
            p.ref_frame_map[i] = i < frame - 1 ? static_cast<VASurfaceID>(100 + i)
                : VA_INVALID_SURFACE;
        for (unsigned i = 0; i < 7; ++i)
            p.ref_frame_idx[i] = 0;
        translate(p, *t, out);
    }

    auto full = params(108, false, 8);
    for (unsigned i = 0; i < kSlots; ++i)
        full.ref_frame_map[i] = 100 + i;
    for (unsigned i = 0; i < 7; ++i)
        full.ref_frame_idx[i] = 0;
    translate(full, *t, out);

    // Slot 1 is released, but the pending surface 108 stays live in slot 7
    // and every reference name points at it.
    auto p = params(109, false, 9);
    for (unsigned i = 0; i < kSlots; ++i)
        p.ref_frame_map[i] = 100 + i;
    p.ref_frame_map[1] = VA_INVALID_SURFACE;
    p.ref_frame_map[7] = 108;
    for (unsigned i = 0; i < 7; ++i)
        p.ref_frame_idx[i] = 7;
    translate(p, *t, out);
    check(out.completions == 2, "install unit declares two completions");
    const auto u = obus(out.bytes);
    unsigned frames = 0;
    for (const auto& o : u)
        if (o.type == 6)
            ++frames;
    check(frames == 2, "install unit contains two FRAME OBUs");
}

// VA may submit a picture the stream never shows. It has no client pixels to
// return, but it still refreshes a DPB slot and its successor may name it.
void test_client_hidden_frame()
{
    auto t = iris::codec::make_av1_dpb(VAProfileAV1Profile0);
    iris::codec::AccessUnit out;

    auto key = params(100, true, 0);
    for (unsigned i = 0; i < kSlots; ++i)
        key.ref_frame_map[i] = VA_INVALID_SURFACE;
    translate(key, *t, out);
    check(!out.hidden, "shown key frame is not hidden");

    auto hidden = params(101, false, 1);
    hidden.pic_info_fields.bits.show_frame = 0;
    hidden.pic_info_fields.bits.showable_frame = 0;
    hidden.ref_frame_map[0] = 100;
    for (unsigned i = 0; i < 7; ++i)
        hidden.ref_frame_idx[i] = 0;
    translate(hidden, *t, out);
    check(out.hidden, "client show_frame=0 picture is marked hidden");
    check(!out.deferred, "client hidden picture is submitted immediately");
    check(out.completions == 1, "client hidden picture has one completion");

    auto shown = params(102, false, 2);
    shown.ref_frame_map[0] = 100;
    shown.ref_frame_map[1] = 101;
    for (unsigned i = 0; i < 7; ++i)
        shown.ref_frame_idx[i] = 1;
    translate(shown, *t, out);
    check(!out.hidden, "successor is shown");
    check(!out.bytes.empty(), "hidden reference still permits the next frame");
}

// A new sequence header must clear internal slot identities: a VA surface id
// from the old sequence is not a reference in the new one.
void test_sequence_restart_clears_slots()
{
    auto t = iris::codec::make_av1_dpb(VAProfileAV1Profile0);
    iris::codec::AccessUnit out;
    auto first = params(100, true, 0);
    for (unsigned i = 0; i < kSlots; ++i)
        first.ref_frame_map[i] = VA_INVALID_SURFACE;
    translate(first, *t, out);
    check(out.sequence_start, "first key frame starts a sequence");

    auto restart = params(200, true, 0);
    restart.frame_width_minus1 = 127;
    restart.frame_height_minus1 = 127;
    for (unsigned i = 0; i < kSlots; ++i)
        restart.ref_frame_map[i] = VA_INVALID_SURFACE;
    translate(restart, *t, out);
    check(out.sequence_start, "a second key frame restarts the sequence");
    check(out.completions == 1, "restart has no stale install replay");
    const auto u = obus(out.bytes);
    unsigned frames = 0, headers = 0;
    for (const auto& o : u) {
        if (o.type == 6)
            ++frames;
        if (o.type == 1)
            ++headers;
    }
    check(frames == 1, "restart emits one frame");
    check(headers == 1, "restart emits its own sequence header");

    auto next = params(201, false, 1);
    next.ref_frame_map[0] = 200;
    for (unsigned i = 0; i < 7; ++i)
        next.ref_frame_idx[i] = 0;
    translate(next, *t, out);
    check(!out.bytes.empty(), "successor resolves against the new sequence");
}

// The DPB holds no picture for a successor: EOS has no tail to guess and no
// sync bound to expire.
void test_flush_has_no_tail()
{
    auto t = iris::codec::make_av1_dpb(VAProfileAV1Profile0);
    iris::codec::AccessUnit out;
    auto key = params(100, true, 0);
    for (unsigned i = 0; i < kSlots; ++i)
        key.ref_frame_map[i] = VA_INVALID_SURFACE;
    translate(key, *t, out);
    check(!out.deferred, "key frame is already submitted");
    check(t->held_surface() == VA_INVALID_ID, "nothing is held at EOS");
    check(!t->flush(out), "flush submits no guessed tail");
    check(!out.deferred, "flush leaves no deferred unit");
    check(t->held_surface() == VA_INVALID_ID, "flush still holds nothing");
}

} // namespace

int main()
{
    try {
        test_no_hold();
        test_slot_exhaustion_and_install();
        test_install_resolves_references_to_pending_surface();
        test_client_hidden_frame();
        test_sequence_restart_clears_slots();
        test_flush_has_no_tail();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " AV1 DPB check(s) failed\n";
        return 1;
    }
    std::cout << "PASS AV1 internal-DPB translator (no hold, install, hidden, restart, EOS)\n";
    return 0;
}
