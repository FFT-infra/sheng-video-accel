// A field picture is half a frame; the node takes whole frames only. The
// translator refuses one before submission, so the client does not pay a
// started session and a DQBUF error before falling back to software.
//
// A context opened in the middle of a GOP is the other refusal: the firmware
// completes nothing until an intra picture, so the translator refuses what
// comes before one instead of letting the client wait for it.
#include "../../src/codec/translator.h"
#include "../../src/util/error.h"

#include <initializer_list>
#include <iostream>
#include <memory>
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

VAPictureParameterBufferH264 progressive()
{
    VAPictureParameterBufferH264 picture = {};
    picture.picture_width_in_mbs_minus1 = 1920 / 16 - 1;
    picture.picture_height_in_mbs_minus1 = 1080 / 16 - 1;
    picture.seq_fields.bits.chroma_format_idc = 1;
    picture.seq_fields.bits.frame_mbs_only_flag = 1;
    picture.seq_fields.bits.direct_8x8_inference_flag = 1;
    picture.seq_fields.bits.pic_order_cnt_type = 0;
    picture.num_ref_frames = 1;
    for (auto& reference : picture.ReferenceFrames)
        reference.flags = VA_PICTURE_H264_INVALID;
    return picture;
}

// Translate one picture and report the status of the refusal, or "accepted".
int status_of(const VAPictureParameterBufferH264& parameters)
{
    VASliceParameterBufferH264 slice = {};
    slice.slice_data_size = 4;
    slice.slice_type = 2; // I slice
    const uint8_t data[4] = { 0x65, 0x88, 0x84, 0x00 };
    iris::codec::Picture picture;
    picture.parameters = &parameters;
    picture.slice_parameters = { &slice };
    picture.slice_data = { { data, sizeof(data) } };
    picture.surface_width = 1920;
    picture.surface_height = 1080;
    auto translator = iris::codec::make_h264(VAProfileH264High);
    iris::codec::AccessUnit out;
    try {
        translator->translate(picture, out);
    } catch (const iris::Error& error) {
        return error.status;
    }
    return VA_STATUS_SUCCESS;
}

// Feed one translator pictures whose slices have the given types.
int status_of(iris::codec::Translator& translator, std::initializer_list<uint8_t> slice_types, bool idr = false)
{
    const auto parameters = progressive();
    std::vector<VASliceParameterBufferH264> slices;
    for (uint8_t type : slice_types) {
        VASliceParameterBufferH264 slice = {};
        slice.slice_data_size = 4;
        slice.slice_type = type;
        slices.push_back(slice);
    }
    const uint8_t data[4] = { static_cast<uint8_t>(idr ? 0x65 : 0x41), 0x88, 0x84, 0x00 };
    iris::codec::Picture picture;
    picture.parameters = &parameters;
    for (const auto& slice : slices) {
        picture.slice_parameters.push_back(&slice);
        picture.slice_data.push_back({ data, sizeof(data) });
    }
    picture.surface_width = 1920;
    picture.surface_height = 1080;
    iris::codec::AccessUnit out;
    try {
        translator.translate(picture, out);
    } catch (const iris::Error& error) {
        return error.status;
    }
    return VA_STATUS_SUCCESS;
}

} // namespace

int main()
{
    {
        auto translator = iris::codec::make_h264(VAProfileH264High);
        check(status_of(*translator, { 0 }) == VA_STATUS_ERROR_DECODING_ERROR, "a P picture cannot open a context");
        check(status_of(*translator, { 1 }) == VA_STATUS_ERROR_DECODING_ERROR, "a B picture cannot open a context");
        check(status_of(*translator, { 2, 5 }) == VA_STATUS_ERROR_DECODING_ERROR,
            "one intra slice does not make the picture a start");
        check(status_of(*translator, { 2, 7 }) == VA_STATUS_SUCCESS, "a non-IDR I picture opens a context");
        check(status_of(*translator, { 0 }) == VA_STATUS_SUCCESS, "a P picture follows the start");
        check(status_of(*translator, { 1, 6 }) == VA_STATUS_SUCCESS, "a B picture follows the start");
    }
    {
        auto translator = iris::codec::make_h264(VAProfileH264High);
        check(status_of(*translator, { 7 }, true) == VA_STATUS_SUCCESS, "an IDR opens a context");
    }

    check(status_of(progressive()) == VA_STATUS_SUCCESS, "a progressive picture is accepted");

    auto frame_only_clear = progressive();
    frame_only_clear.seq_fields.bits.frame_mbs_only_flag = 0;
    check(status_of(frame_only_clear) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "frame_mbs_only_flag=0 is refused");

    auto top_field = progressive();
    top_field.CurrPic.flags = VA_PICTURE_H264_TOP_FIELD;
    check(status_of(top_field) == VA_STATUS_ERROR_UNIMPLEMENTED, "a top field picture is refused");

    auto bottom_field = progressive();
    bottom_field.CurrPic.flags = VA_PICTURE_H264_BOTTOM_FIELD;
    check(status_of(bottom_field) == VA_STATUS_ERROR_UNIMPLEMENTED, "a bottom field picture is refused");

    auto reference = progressive();
    reference.CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    check(status_of(reference) == VA_STATUS_SUCCESS, "a reference flag alone is not a field");

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS H.264 translator refuses field pictures and a start before an intra picture\n";
    return 0;
}
