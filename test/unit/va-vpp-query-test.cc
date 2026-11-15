// SPDX-License-Identifier: MIT
//
// The VideoProc query answers, without a VA display. What the processor
// advertises is a promise the picture then has to keep: P010 in and out only
// when the node reported a 10-bit capture format, the HDR10 lists only for a
// pipeline that names the tone mapper, and the tone mapper itself only on
// that node. These cases pin the four combinations and the error statuses
// the entry points rely on guarded() to turn into.

#include "../../src/va/driver.h"

#include "../../src/util/error.h"

#include <cstdio>
#include <iostream>
#include <memory>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// The driver tables a query needs: one VideoProc context, and the geometry
// the node reported. Everything else is left at its default.
void make_driver(va::DriverData& d, bool p010)
{
    d.capabilities.p010 = p010;
    d.capabilities.max_width = 8192;
    d.capabilities.max_height = 8192;
    auto c = std::make_shared<va::Context>();
    c->entrypoint = VAEntrypointVideoProc;
    d.contexts[1] = c;
}

template <typename Function> VAStatus status_of(Function&& body)
{
    try {
        body();
    } catch (const iris::Error& error) {
        return error.status;
    }
    return static_cast<VAStatus>(-1);
}

constexpr VAContextID kContext = 1;
constexpr VAContextID kMissing = 99;

void test_eight_bit_offers_no_filter()
{
    va::DriverData d;
    make_driver(d, false);
    unsigned count = 8;
    VAProcFilterType filters[2] = {};
    check(va::query_vpp_filters(d, kContext, filters, &count) == VA_STATUS_SUCCESS, "filters on 8-bit");
    check(count == 0, "8-bit node offers no filter");

    count = 0;
    check(va::query_vpp_filter_caps(d, kContext, VAProcFilterHighDynamicRangeToneMapping, nullptr, &count)
            == VA_STATUS_ERROR_UNSUPPORTED_FILTER,
        "8-bit node refuses the tone mapper");
    check(count == 0, "refused filter reports no caps");
}

void test_ten_bit_offers_the_tone_mapper()
{
    va::DriverData d;
    make_driver(d, true);
    unsigned count = 0;
    check(va::query_vpp_filters(d, kContext, nullptr, &count) == VA_STATUS_SUCCESS, "filter count query");
    check(count == 1, "10-bit node offers one filter");

    VAProcFilterType filter = VAProcFilterNone;
    count = 1;
    check(va::query_vpp_filters(d, kContext, &filter, &count) == VA_STATUS_SUCCESS, "filter list query");
    check(filter == VAProcFilterHighDynamicRangeToneMapping, "the filter is the HDR10 tone mapper");

    count = 0;
    check(va::query_vpp_filters(d, kContext, nullptr, &count) == VA_STATUS_SUCCESS && count == 1,
        "a caller with no array learns the size");
    VAProcFilterType one = VAProcFilterNone;
    count = 0;
    check(va::query_vpp_filters(d, kContext, &one, &count) == VA_STATUS_ERROR_MAX_NUM_EXCEEDED,
        "an array too small is refused");
    check(count == 1, "the refusal still reports the needed size");

    VAProcFilterCapHighDynamicRange hdr = {};
    count = 1;
    check(va::query_vpp_filter_caps(d, kContext, VAProcFilterHighDynamicRangeToneMapping, &hdr, &count)
            == VA_STATUS_SUCCESS,
        "tone mapper caps");
    check(hdr.metadata_type == VAProcHighDynamicRangeMetadataHDR10, "HDR10 input");
    check(hdr.caps_flag == VA_TONE_MAPPING_HDR_TO_SDR, "HDR to SDR output");
}

void test_pipeline_caps_follow_the_node()
{
    va::DriverData d;
    make_driver(d, true);
    VAProcPipelineCaps caps = {};
    check(va::query_vpp_pipeline_caps(d, kContext, nullptr, 0, &caps) == VA_STATUS_SUCCESS, "pipeline caps");
    check(caps.num_input_pixel_formats == 4, "10-bit node takes P010 and the overlay formats");
    check(caps.num_output_pixel_formats == 2, "10-bit node writes NV12 or P010");
    check(caps.rotation_flags == ((1u << VA_ROTATION_NONE) | (1u << VA_ROTATION_90) | (1u << VA_ROTATION_180)
                                     | (1u << VA_ROTATION_270)),
        "all four quarter turns");
    check(caps.mirror_flags == (VA_MIRROR_HORIZONTAL | VA_MIRROR_VERTICAL), "both mirror axes");
    check(caps.blend_flags == (VA_BLEND_GLOBAL_ALPHA | VA_BLEND_PREMULTIPLIED_ALPHA), "the blend modes run_vpp runs");
    check(caps.min_input_width == va::kVppMinGeometry && caps.min_input_height == va::kVppMinGeometry,
        "the processor owns its own minimum");
    check(caps.max_input_width == 8192 && caps.max_output_height == 8192, "the node's maximum");
}

void test_tone_map_pipeline_narrows_the_lists()
{
    va::DriverData d;
    make_driver(d, true);
    VABufferID filter = 1;
    VAProcPipelineCaps caps = {};
    check(va::query_vpp_pipeline_caps(d, kContext, &filter, 1, &caps) == VA_STATUS_SUCCESS, "filtered pipeline");
    check(caps.num_input_pixel_formats == 1 && caps.input_pixel_format[0] == VA_FOURCC_P010,
        "a tone-mapped picture reads P010");
    check(caps.num_output_pixel_formats == 1 && caps.output_pixel_format[0] == VA_FOURCC_NV12,
        "and writes NV12");
    check(caps.num_input_color_standards == 2, "BT.2020 or unstated in");
    check(caps.num_output_color_standards == 2, "BT.709 or unstated out");
    check(caps.output_color_standards[0] == VAProcColorStandardBT709, "the SDR target");

    va::DriverData eight;
    make_driver(eight, false);
    caps = {};
    check(va::query_vpp_pipeline_caps(eight, kContext, &filter, 1, &caps) == VA_STATUS_SUCCESS,
        "8-bit node with a filter name");
    check(caps.num_input_pixel_formats == 3, "no tone mapper on an 8-bit node, so no narrowing");
}

void test_bad_arguments_are_statuses()
{
    va::DriverData d;
    make_driver(d, true);
    unsigned count = 1;
    VAProcFilterType filter = VAProcFilterNone;
    check(status_of([&] { va::query_vpp_filters(d, kContext, &filter, nullptr); })
            == VA_STATUS_ERROR_INVALID_PARAMETER,
        "null filter count");
    check(status_of([&] { va::query_vpp_filters(d, kMissing, &filter, &count); })
            == VA_STATUS_ERROR_INVALID_CONTEXT,
        "unknown context");
    check(status_of([&] { va::query_vpp_filter_caps(d, kMissing, VAProcFilterHighDynamicRangeToneMapping, nullptr, &count); })
            == VA_STATUS_ERROR_INVALID_CONTEXT,
        "unknown context on the caps query");

    VAProcPipelineCaps caps = {};
    check(status_of([&] { va::query_vpp_pipeline_caps(d, kContext, nullptr, 0, nullptr); })
            == VA_STATUS_ERROR_INVALID_PARAMETER,
        "null pipeline caps");

    auto encode = std::make_shared<va::Context>();
    encode->entrypoint = VAEntrypointEncSlice;
    d.contexts[2] = encode;
    check(status_of([&] { va::query_vpp_pipeline_caps(d, 2, nullptr, 0, &caps); })
            == VA_STATUS_ERROR_INVALID_CONTEXT,
        "pipeline caps on a non-VideoProc context");
}

} // namespace

int main()
{
    test_eight_bit_offers_no_filter();
    test_ten_bit_offers_the_tone_mapper();
    test_pipeline_caps_follow_the_node();
    test_tone_map_pipeline_narrows_the_lists();
    test_bad_arguments_are_statuses();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "va-vpp-query: all cases passed\n";
    return 0;
}
