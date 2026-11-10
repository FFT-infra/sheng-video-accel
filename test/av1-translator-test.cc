// The AV1 translator holds one picture until its successor names the DPB
// slots it refreshed. Without a successor there is no VA flush entry point, so
// the held picture is released when the client retires the context. These
// tests lock that contract down without a device: a held picture is submitted
// once, its refresh flags are not guessed, and a stream that continues after
// a guessed release is refused.
#include "../src/codec/av1_obu.h"
#include "../src/codec/translator.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

VADecPictureParameterBufferAV1 key_frame(unsigned width = 1280, unsigned height = 720)
{
    VADecPictureParameterBufferAV1 picture = {};
    picture.profile = 0;
    auto& seq = picture.seq_info_fields.fields;
    seq.enable_order_hint = 1;
    seq.subsampling_x = 1;
    seq.subsampling_y = 1;
    picture.order_hint_bits_minus_1 = 6;
    picture.frame_width_minus1 = width - 1;
    picture.frame_height_minus1 = height - 1;
    auto& pic = picture.pic_info_fields.bits;
    pic.frame_type = 0; // KEY_FRAME
    pic.show_frame = 1;
    pic.disable_cdf_update = 1;
    pic.uniform_tile_spacing_flag = 1;
    picture.tile_cols = 1;
    picture.tile_rows = 1;
    picture.base_qindex = 54;
    picture.mode_control_fields.bits.tx_mode = 2;
    return picture;
}

void translate(const VADecPictureParameterBufferAV1& parameters, uint32_t surface,
    iris::codec::Translator& translator, iris::codec::AccessUnit& out)
{
    const uint8_t tile_data[8] = { 0xde, 0xad, 0xbe, 0xef, 0, 1, 2, 3 };
    VASliceParameterBufferAV1 slice = {};
    slice.slice_data_size = sizeof(tile_data);
    iris::codec::Picture picture;
    picture.parameters = &parameters;
    picture.slice_parameters = { &slice };
    picture.slice_data = { { tile_data, sizeof(tile_data) } };
    picture.surface_id = surface;
    translator.translate(picture, out);
}

// A shown key frame refreshes all eight slots by definition, so its tail can
// be released before any successor without inventing refresh flags.
void test_eos_tail_known_flags()
{
    auto translator = iris::codec::make_av1(VAProfileAV1Profile0);
    iris::codec::AccessUnit out;
    translate(key_frame(), 7, *translator, out);
    check(out.deferred, "first picture is held");
    check(translator->held_surface() == 7, "held surface is the submitted one");
    check(translator->held_flags_known(), "shown key frame has fixed refresh flags");

    check(translator->flush(out), "flush releases the held picture");
    check(!out.deferred, "flushed tail is ready to submit");
    check(!out.bytes.empty(), "flushed tail carries an access unit");
    check(out.released_surface == 7, "flushed tail belongs to the held surface");
    check(translator->held_surface() == VA_INVALID_ID, "flush clears the held surface");
    check(!translator->flush(out), "a second flush submits nothing");
}

// An inter frame's refresh slots are a choice the successor has to name. At
// EOS the translator submits it refreshing nothing and remembers that the
// guess was made, so a later picture cannot silently decode against the wrong
// references.
void test_eos_tail_unknown_flags()
{
    auto translator = iris::codec::make_av1(VAProfileAV1Profile0);
    auto inter = key_frame();
    inter.pic_info_fields.bits.frame_type = 1; // INTER_FRAME
    inter.current_frame = 9;
    iris::codec::AccessUnit out;
    translate(inter, 9, *translator, out);
    check(!translator->held_flags_known(), "inter frame needs its successor");
    check(translator->flush(out), "flush releases the inter frame tail");
    check(!out.bytes.empty(), "guessed tail still produces an access unit");

    bool refused = false;
    try {
        translate(inter, 10, *translator, out);
    } catch (const std::exception&) {
        refused = true;
    }
    check(refused, "a picture after a guessed tail is refused");

    // An intra picture that refreshes every slot can recover the stream.
    translate(key_frame(), 11, *translator, out);
}

} // namespace

int main()
{
    try {
        test_eos_tail_known_flags();
        test_eos_tail_unknown_flags();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " AV1 translator check(s) failed\n";
        return 1;
    }
    std::cout << "PASS AV1 translator held-picture release at EOS\n";
    return 0;
}
