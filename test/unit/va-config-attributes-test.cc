// SPDX-License-Identifier: MIT
//
// The encode and decode config attributes a client reads before it opens a
// context. These are the values the sibling backend answers and FFmpeg /
// GStreamer branch on: a NOT_SUPPORTED answer for a known encode attribute
// makes FFmpeg drop -quality and GStreamer clamp target-usage to zero. This
// links the real config.cc against a constructed DriverData, so a regression
// in the answer or in createConfig's value check fails on the host.

#include "../../src/va/driver.h"

#include <cstring>
#include <iostream>

extern "C" {
#include <va/va.h>
#include <va/va_backend.h>
}

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

VADriverContextP context(va::DriverData& d, VADriverContext& ctx)
{
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.pDriverData = &d;
    return &ctx;
}

VAConfigAttrib query(VADriverContextP ctx, VAProfile profile, VAEntrypoint entry, VAConfigAttribType type)
{
    VAConfigAttrib a { type, 0xdeadbeef };
    va::getConfigAttributes(ctx, profile, entry, &a, 1);
    return a;
}

} // namespace

int main()
{
    va::DriverData d;
    d.capabilities.encode_profiles = { VAProfileH264High, VAProfileHEVCMain, VAProfileHEVCMain10 };
    d.capabilities.profiles = { VAProfileAV1Profile0, VAProfileH264High,
        VAProfileHEVCMain10, VAProfileVP9Profile0, VAProfileVP9Profile2 };
    d.capabilities.p010 = true;
    VADriverContext ctx;
    VADriverContextP p = context(d, ctx);

    const auto enc = VAEntrypointEncSlice;
    const auto dec = VAEntrypointVLD;

    check(query(p, VAProfileH264High, enc, VAConfigAttribEncQualityRange).value == 1,
        "encode quality range is 1");
    check(query(p, VAProfileHEVCMain, enc, VAConfigAttribEncQualityRange).value == 1,
        "HEVC encode quality range is 1");
    check(query(p, VAProfileH264High, enc, VAConfigAttribEncInterlaced).value == VA_ENC_INTERLACED_NONE,
        "encode is progressive only");
    check(query(p, VAProfileH264High, enc, VAConfigAttribEncSliceStructure).value
            == VA_ENC_SLICE_STRUCTURE_EQUAL_ROWS,
        "encode has one equal-row slice");
    check(query(p, VAProfileH264High, enc, VAConfigAttribPredictionDirection).value
            == VA_PREDICTION_DIRECTION_PREVIOUS,
        "encode predicts from the previous picture");
    // B pictures are opt-in. FFmpeg reads the future direction as the licence
    // to code them, and the l1 reference as what it may predict from, so the
    // two answers must move together; HEVC stays P-only either way.
    d.options.b_frames = true;
    check(query(p, VAProfileH264High, enc, VAConfigAttribPredictionDirection).value
            == (VA_PREDICTION_DIRECTION_PREVIOUS | VA_PREDICTION_DIRECTION_FUTURE),
        "an opted-in H.264 encode reports a future prediction direction");
    check((query(p, VAProfileH264High, enc, VAConfigAttribEncMaxRefFrames).value >> 16) == 1,
        "an opted-in H.264 encode advertises one l1 reference");
    check(query(p, VAProfileHEVCMain, enc, VAConfigAttribPredictionDirection).value
            == VA_PREDICTION_DIRECTION_PREVIOUS,
        "an opted-in HEVC encode stays on the previous direction");
    check((query(p, VAProfileHEVCMain, enc, VAConfigAttribEncMaxRefFrames).value >> 16) == 0,
        "an opted-in HEVC encode advertises no l1 reference");
    d.options.b_frames = false;
    check(query(p, VAProfileH264High, enc, VAConfigAttribEncIntraRefresh).value
            == VA_ENC_INTRA_REFRESH_ROLLING_ROW,
        "rolling-row intra refresh stays advertised");
    // A raw packed header is the one the driver can carry: FFmpeg's HEVC
    // encoder sends its SEI that way, and MISC is the bit it checks before
    // calling write_extra_header at all.
    check(query(p, VAProfileHEVCMain, enc, VAConfigAttribEncPackedHeaders).value
            == VA_ENC_PACKED_HEADER_MISC,
        "encode advertises raw packed headers through MISC");
    check(query(p, VAProfileH264High, dec, VAConfigAttribEncPackedHeaders).value == VA_ATTRIB_NOT_SUPPORTED,
        "packed headers do not answer on decode");
    VAConfigID packed_id = 0;
    VAConfigAttrib packed_ok[] = { { VAConfigAttribEncPackedHeaders, VA_ENC_PACKED_HEADER_MISC } };
    check(va::createConfig(p, VAProfileHEVCMain, enc, packed_ok, 1, &packed_id) == VA_STATUS_SUCCESS,
        "a client that sends the advertised MISC bit opens a config");
    VAConfigAttrib packed_bad[] = { { VAConfigAttribEncPackedHeaders, VA_ENC_PACKED_HEADER_RAW_DATA } };
    check(va::createConfig(p, VAProfileHEVCMain, enc, packed_bad, 1, &packed_id)
            == VA_STATUS_ERROR_ATTR_NOT_SUPPORTED,
        "the RAW_DATA bit is not the one this driver advertises");
    check(query(p, VAProfileAV1Profile0, dec, VAConfigAttribDecAV1Features).value == 0,
        "AV1 features are known; large-scale tiles are not supported");
    check(query(p, VAProfileH264High, dec, VAConfigAttribDecAV1Features).value == VA_ATTRIB_NOT_SUPPORTED,
        "AV1 features do not answer on H.264 decode");
    check(query(p, VAProfileH264High, dec, VAConfigAttribEncQualityRange).value == VA_ATTRIB_NOT_SUPPORTED,
        "encode attributes do not answer on decode");
    // Temporal layers follow the node's hierarchical controls; without them
    // the attribute must not promise a hierarchy the session cannot write.
    check(query(p, VAProfileH264High, enc, VAConfigAttribEncRateControlExt).value
            == VA_ATTRIB_NOT_SUPPORTED,
        "temporal layers are not advertised without the hierarchical controls");
    d.capabilities.encode_temporal_layers = true;
    VAConfigAttribValEncRateControlExt extended;
    extended.value = query(p, VAProfileH264High, enc, VAConfigAttribEncRateControlExt).value;
    check(extended.bits.max_num_temporal_layers_minus1 == 5
            && extended.bits.temporal_layer_bitrate_control_flag == 0,
        "six temporal layers and no per-layer rate control");
    d.capabilities.encode_temporal_layers = false;

    // VP9 Profile 2 has no legal 8-bit stream; answer 10-bit only. HEVC
    // Main10 and AV1 Profile0 decode both depths and keep both bits.
    check(query(p, VAProfileVP9Profile2, dec, VAConfigAttribRTFormat).value == VA_RT_FORMAT_YUV420_10,
        "VP9 Profile 2 answers 10-bit only");
    check(query(p, VAProfileHEVCMain10, dec, VAConfigAttribRTFormat).value
            == (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10),
        "HEVC Main10 keeps both bit depths");
    check(query(p, VAProfileAV1Profile0, dec, VAConfigAttribRTFormat).value
            == (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10),
        "AV1 Profile 0 keeps both bit depths");
    check(query(p, VAProfileVP9Profile0, dec, VAConfigAttribRTFormat).value == VA_RT_FORMAT_YUV420,
        "VP9 Profile 0 stays 8-bit only");

    // A client (FFmpeg on a VP9 P2 clip) opens the config without an
    // RT-format attribute; the default must match what was advertised, or
    // the surface and the CAPTURE format disagree.
    VAConfigID p2_id = 0;
    check(va::createConfig(p, VAProfileVP9Profile2, dec, nullptr, 0, &p2_id) == VA_STATUS_SUCCESS,
        "VP9 Profile 2 opens a config with no attributes");
    VAConfigAttrib p2_query = { VAConfigAttribRTFormat, 0 };
    int p2_count = 1;
    va::queryConfigAttributes(p, p2_id, nullptr, nullptr, &p2_query, &p2_count);
    check(p2_query.value == VA_RT_FORMAT_YUV420_10,
        "VP9 Profile 2 config default is 10-bit only");

    // Same shape on the encode side: FFmpeg opens a probe config with no
    // attributes before it knows the surface format, so a Main 10 profile
    // must default to 10 bits instead of the 8-bit default.
    check(query(p, VAProfileHEVCMain10, enc, VAConfigAttribRTFormat).value == VA_RT_FORMAT_YUV420_10,
        "Main 10 encode answers YUV420_10 only");
    VAConfigID m10_id = 0;
    check(va::createConfig(p, VAProfileHEVCMain10, enc, nullptr, 0, &m10_id) == VA_STATUS_SUCCESS,
        "Main 10 encode opens a config with no attributes");
    VAConfigAttrib m10_query = { VAConfigAttribRTFormat, 0 };
    int m10_count = 1;
    va::queryConfigAttributes(p, m10_id, nullptr, nullptr, &m10_query, &m10_count);
    check(m10_query.value == VA_RT_FORMAT_YUV420_10, "Main 10 config default is 10-bit");
    VAConfigAttrib m10_8bit[] = { { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420 } };
    check(va::createConfig(p, VAProfileHEVCMain10, enc, m10_8bit, 1, &m10_id)
            == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
        "an 8-bit RT format is refused for Main 10 encode");
    VAConfigAttrib m10_10bit[] = { { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420_10 } };
    check(va::createConfig(p, VAProfileHEVCMain10, enc, m10_10bit, 1, &m10_id) == VA_STATUS_SUCCESS,
        "Main 10 encode accepts YUV420_10");

    // createConfig rejects a value outside getConfigAttributes' mask, but
    // still opens a config for the attributes a real client sends.
    VAConfigID id = 0;
    VAConfigAttrib good[] = { { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420 },
                              { VAConfigAttribRateControl, VA_RC_CBR } };
    check(va::createConfig(p, VAProfileH264High, enc, good, 2, &id) == VA_STATUS_SUCCESS
            && id != 0, "a real client's RTFormat and RateControl still open a config");

    VAConfigAttrib bad[] = { { VAConfigAttribEncMaxSlices, 2 } };
    check(va::createConfig(p, VAProfileH264High, enc, bad, 1, &id) == VA_STATUS_ERROR_ATTR_NOT_SUPPORTED,
        "two slices are refused at createConfig");
    bad[0] = { VAConfigAttribEncQualityRange, 2 };
    check(va::createConfig(p, VAProfileH264High, enc, bad, 1, &id) == VA_STATUS_ERROR_ATTR_NOT_SUPPORTED,
        "quality level 2 is refused at createConfig");
    bad[0] = { VAConfigAttribRateControl, VA_RC_CBR | 0x80000000u };
    check(va::createConfig(p, VAProfileH264High, enc, bad, 1, &id) == VA_STATUS_ERROR_ATTR_NOT_SUPPORTED,
        "an unadvertised rate-control bit is refused at createConfig");

    // An RT-format outside the advertised mask is a format refusal, not a
    // generic attribute one. The comparison backend answers the same status
    // for an 8-bit RT-format on VP9 Profile 2.
    VAConfigAttrib bad_rt[] = { { VAConfigAttribRTFormat,
        VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10 } };
    check(va::createConfig(p, VAProfileVP9Profile2, dec, bad_rt, 1, &id)
            == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
        "an unadvertised VP9 Profile 2 RT-format is a format refusal");

    // An unknown type stays accepted: a newer client must still be able to
    // open a config, which a strict whitelist would break.
    VAConfigAttrib unknown[] = { { static_cast<VAConfigAttribType>(60), 1 } };
    check(va::createConfig(p, VAProfileH264High, enc, unknown, 1, &id) == VA_STATUS_SUCCESS,
        "an unknown attribute type stays accepted");

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "va-config-attributes: all cases passed\n";
    return 0;
}
