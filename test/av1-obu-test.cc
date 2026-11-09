// Walk the rebuilt AV1 frame header back with a spec-ordered reader and check
// the fields whose coding is not a plain copy of the VA value. No hardware:
// the OBU writer is a pure function of the VA picture buffer.
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

class Reader {
public:
    Reader(const std::vector<uint8_t>& bytes, size_t offset)
        : bytes_(bytes)
        , offset_(offset * 8)
    {
    }

    unsigned bit()
    {
        if (offset_ >= bytes_.size() * 8)
            throw std::runtime_error("frame header ended early");
        const unsigned value = (bytes_[offset_ / 8] >> (7 - offset_ % 8)) & 1u;
        ++offset_;
        return value;
    }

    unsigned bits(unsigned count)
    {
        unsigned value = 0;
        while (count--)
            value = (value << 1) | bit();
        return value;
    }

    // read_increment(): unary 1s from range_min, terminated by a 0 or by
    // reaching range_max. This is how lr_unit_shift and the tile log2 counts
    // are coded, and the whole point of the test.
    unsigned increment(unsigned range_min, unsigned range_max)
    {
        unsigned value = range_min;
        while (value < range_max) {
            if (!bit())
                break;
            ++value;
        }
        return value;
    }

    size_t bit_offset() const { return offset_; }

private:
    const std::vector<uint8_t>& bytes_;
    size_t offset_;
};

// Locate one OBU by type and return the byte offset of its payload.
size_t obu_payload(const std::vector<uint8_t>& bytes, unsigned type)
{
    size_t i = 0;
    while (i < bytes.size()) {
        const unsigned header_type = (bytes[i] >> 3) & 0xf;
        const bool ext = (bytes[i] >> 2) & 1;
        const bool has_size = (bytes[i] >> 1) & 1;
        if (!has_size)
            throw std::runtime_error("OBU without a size field");
        size_t cursor = i + 1 + (ext ? 1 : 0);
        size_t size = 0;
        unsigned shift = 0;
        while (true) {
            const uint8_t byte = bytes[cursor++];
            size |= static_cast<size_t>(byte & 0x7f) << shift;
            shift += 7;
            if (!(byte & 0x80))
                break;
        }
        if (header_type == type)
            return cursor;
        i = cursor + size;
    }
    throw std::runtime_error("OBU type not found in the rebuilt unit");
}

VADecPictureParameterBufferAV1 key_frame(bool superblocks_128, unsigned lr_unit_shift,
    unsigned width = 1280, unsigned height = 720)
{
    VADecPictureParameterBufferAV1 picture = {};
    picture.profile = 0;
    auto& seq = picture.seq_info_fields.fields;
    seq.use_128x128_superblock = superblocks_128 ? 1 : 0;
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
    picture.filter_level[0] = 13;
    picture.filter_level[1] = 14;
    picture.filter_level_u = 4;
    picture.filter_level_v = 7;
    picture.loop_filter_info_fields.bits.mode_ref_delta_enabled = 1;
    picture.loop_restoration_fields.bits.yframe_restoration_type = 3; // SWITCHABLE
    picture.mode_control_fields.bits.tx_mode = 2;
    picture.loop_restoration_fields.bits.lr_unit_shift = lr_unit_shift;
    return picture;
}

// Walk the header of one rebuilt key frame up to the loop restoration unit
// shift, checking each field as it goes. Returns the coded shift bits.
unsigned walk_key_header(bool superblocks_128, unsigned lr_unit_shift, std::vector<uint8_t>* bytes_out)
{
    const VADecPictureParameterBufferAV1 picture = key_frame(superblocks_128, lr_unit_shift);
    const AV1SequenceState sequence = av1_sequence_state(picture);
    const AV1ReferenceState references;
    const std::vector<uint8_t> tile_data = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22, 0x33 };
    const std::vector<AV1TileEntry> tiles = { { 0, 8 } };
    std::vector<uint8_t> out;
    const char* reject_reason = nullptr;
    if (!av1_build_temporal_unit(out, picture, sequence, references, false, 0xff, tile_data.data(),
            tiles, &reject_reason))
        throw std::runtime_error(reject_reason ? reject_reason : "writer rejected the frame");
    if (bytes_out)
        *bytes_out = out;

    Reader reader(out, obu_payload(out, 6));
    check(reader.bit() == 0, "show_existing_frame is not signalled");
    check(reader.bits(2) == 0, "frame_type is KEY_FRAME");
    check(reader.bit() == 1, "show_frame is set");
    // A shown key frame makes error_resilient_mode implicit; the bit is absent.
    check(reader.bit() == 1, "disable_cdf_update is forwarded");
    check(reader.bit() == 0, "allow_screen_content_tools is clear");
    check(reader.bit() == 0, "frame_size_override_flag is clear at the sequence size");
    check(reader.bits(7) == 0, "order_hint is forwarded");
    check(reader.bit() == 0, "use_superres is clear");
    check(reader.bit() == 0, "render_and_frame_size_different is clear");
    check(reader.bit() == 1, "uniform_tile_spacing_flag is forwarded");
    // One tile, so the increments end immediately and no context_update_tile_id
    // or tile_size_bytes field follows.
    const unsigned min_log2_tile_cols = 0;
    const unsigned min_log2_tile_rows = 0;
    check(reader.increment(min_log2_tile_cols, 4) == 0, "tile_cols_log2 is 0");
    check(reader.increment(min_log2_tile_rows, 4) == 0, "tile_rows_log2 is 0");
    check(reader.bits(8) == 54, "base_qindex is forwarded");
    check(reader.bit() == 0, "delta_q_y_dc is zero");
    check(reader.bit() == 0, "delta_q_u_dc is zero");
    check(reader.bit() == 0, "delta_q_u_ac is zero");
    check(reader.bit() == 0, "using_qmatrix is clear");
    check(reader.bit() == 0, "segmentation_enabled is clear");
    check(reader.bit() == 0, "delta_q_present is clear");
    check(reader.bits(6) == 13, "loop_filter_level[0] is forwarded");
    check(reader.bits(6) == 14, "loop_filter_level[1] is forwarded");
    check(reader.bits(6) == 4, "loop_filter_level[2] is forwarded");
    check(reader.bits(6) == 7, "loop_filter_level[3] is forwarded");
    check(reader.bits(3) == 0, "loop_filter_sharpness is forwarded");
    check(reader.bit() == 1, "loop_filter_delta_enabled is forwarded");
    check(reader.bit() == 0, "loop_filter_delta_update is clear");
    // lr_type is coded through Remap_Lr_Type; VA's SWITCHABLE is coded 1.
    check(reader.bits(2) == 1, "yframe_restoration_type is coded SWITCHABLE");
    check(reader.bits(2) == 0, "cbframe_restoration_type is NONE");
    check(reader.bits(2) == 0, "crframe_restoration_type is NONE");
    const unsigned coded = superblocks_128
        ? reader.increment(1, 2)
        : reader.increment(0, 2);
    check(reader.bit() == 1, "tx_mode is SELECT");
    check(reader.bit() == 0, "reduced_tx_set_used is clear");
    return coded;
}

// Walk to the frame_size_override_flag of a rebuilt key frame. The bits
// before it are fixed for these synthetic pictures: a shown key frame makes
// error_resilient_mode implicit and clears allow_screen_content_tools.
unsigned read_frame_size_override(const std::vector<uint8_t>& unit)
{
    Reader reader(unit, obu_payload(unit, 6));
    check(reader.bit() == 0, "show_existing_frame is not signalled");
    check(reader.bits(2) == 0, "frame_type is KEY_FRAME");
    check(reader.bit() == 1, "show_frame is set");
    check(reader.bit() == 1, "disable_cdf_update is forwarded");
    check(reader.bit() == 0, "allow_screen_content_tools is clear");
    return reader.bit();
}

// A held picture is released by the arrival of its successor, and it has to
// be written against the sequence it arrived in. Where the successor is a key
// frame at a new resolution (the shape a stream takes around a resolution
// change) the two sequences disagree, and taking the new one made the old
// frame announce a frame_size_override against the new width bits: the header
// stopped parsing and the firmware answered ERROR.
void test_held_picture_across_a_resolution_change()
{
    using iris::codec::AccessUnit;
    using iris::codec::Picture;

    const auto translate_key = [](const VADecPictureParameterBufferAV1& parameters, uint32_t surface,
                                   iris::codec::Translator& translator, AccessUnit& out) {
        const uint8_t tile_data[8] = { 0xde, 0xad, 0xbe, 0xef, 0, 1, 2, 3 };
        VASliceParameterBufferAV1 slice = {};
        slice.slice_data_offset = 0;
        slice.slice_data_size = sizeof(tile_data);
        Picture picture;
        picture.parameters = &parameters;
        picture.slice_parameters = { &slice };
        picture.slice_data = { { tile_data, sizeof(tile_data) } };
        picture.surface_id = surface;
        translator.translate(picture, out);
    };

    const VADecPictureParameterBufferAV1 hd = key_frame(true, 1, 1280, 720);
    const VADecPictureParameterBufferAV1 sd = key_frame(false, 0, 854, 480);
    auto translator = iris::codec::make_av1(VAProfileAV1Profile0);

    AccessUnit out;
    translate_key(hd, 1, *translator, out);
    check(out.deferred, "the first picture is held back");

    // The key frame at the new resolution releases the 1280x720 one.
    translate_key(sd, 2, *translator, out);
    check(!out.deferred, "the successor releases the held picture");
    check(out.sequence_start, "the released picture starts its own sequence");
    check(read_frame_size_override(out.bytes) == 0,
        "the released frame is coded at its own sequence size");
}

void test_lr_unit_shift()
{
    // 128x128 superblocks: the unit shift is coded as increments from 1, so
    // VA's 1 is one zero bit and 2 is one one bit. Encoding it as a boolean of
    // the value made 1 and 2 identical, which is what made a decoder apply
    // restoration over the wrong unit and reject the tile data.
    check(walk_key_header(true, 1, nullptr) == 1, "128x128: lr_unit_shift 1 round-trips");
    check(walk_key_header(true, 2, nullptr) == 2, "128x128: lr_unit_shift 2 round-trips");

    // 64x64 superblocks: the range is 0..2.
    check(walk_key_header(false, 0, nullptr) == 0, "64x64: lr_unit_shift 0 round-trips");
    check(walk_key_header(false, 1, nullptr) == 1, "64x64: lr_unit_shift 1 round-trips");
    check(walk_key_header(false, 2, nullptr) == 2, "64x64: lr_unit_shift 2 round-trips");

    // The two values a 128x128 stream can carry must not encode identically.
    std::vector<uint8_t> coarse;
    std::vector<uint8_t> fine;
    walk_key_header(true, 1, &coarse);
    walk_key_header(true, 2, &fine);
    check(coarse != fine, "128x128: shifts 1 and 2 produce different bitstreams");
}

} // namespace

int main()
{
    try {
        test_lr_unit_shift();
        test_held_picture_across_a_resolution_change();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " AV1 OBU check(s) failed\n";
        return 1;
    }
    std::cout << "PASS AV1 frame header rebuild (loop restoration unit shift, held picture across a size change)\n";
    return 0;
}
