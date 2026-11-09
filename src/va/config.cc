// SPDX-License-Identifier: MIT

#include "driver.h"

#include "../util/error.h"

#include <algorithm>
#include <cstring>

namespace va {

namespace {

VAStatus guarded(VADriverContextP ctx, const char* where, auto&& body)
{
    try {
        return body();
    } catch (const std::exception& error) {
        return report(ctx, where, error);
    }
}

} // namespace

VAStatus queryConfigProfiles(VADriverContextP ctx, VAProfile* profiles, int* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    std::set<VAProfile> all = d.capabilities.profiles;
    all.insert(d.capabilities.encode_profiles.begin(), d.capabilities.encode_profiles.end());
    int n = 0;
    for (VAProfile profile : all)
        if (n < static_cast<int>(kMaxProfiles))
            profiles[n++] = profile;
    *count = n;
    return VA_STATUS_SUCCESS;
}

VAStatus queryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint* entrypoints, int* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    // Decode first: FFmpeg's encode path walks the list and skips VLD, so
    // the order is only about what a human reads in vainfo.
    int n = 0;
    if (d.capabilities.profiles.count(profile))
        entrypoints[n++] = VAEntrypointVLD;
    if (d.capabilities.encode_profiles.count(profile) && n < static_cast<int>(kMaxEntrypoints))
        entrypoints[n++] = VAEntrypointEncSlice;
    *count = n;
    return VA_STATUS_SUCCESS;
}

VAStatus getConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
    VAConfigAttrib* attributes, int count)
{
    auto& d = data(ctx);
    const bool decode = d.capabilities.profiles.count(profile) && entrypoint == VAEntrypointVLD;
    const bool encode = d.capabilities.encode_profiles.count(profile) && entrypoint == VAEntrypointEncSlice;
    for (int i = 0; i < count; ++i) {
        switch (attributes[i].type) {
        case VAConfigAttribRTFormat:
            if (decode) {
                attributes[i].value = VA_RT_FORMAT_YUV420;
                if (p010_decode_profile(profile) && d.capabilities.p010)
                    attributes[i].value |= VA_RT_FORMAT_YUV420_10;
            } else if (encode) {
                // The encoder's raw port is NV12; P010 is not an option.
                attributes[i].value = VA_RT_FORMAT_YUV420;
            } else {
                attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
            }
            break;
        case VAConfigAttribDecSliceMode:
            attributes[i].value = decode ? VA_DEC_SLICE_MODE_NORMAL : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureWidth:
            attributes[i].value = (decode || encode) ? d.capabilities.max_width : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureHeight:
            attributes[i].value = (decode || encode) ? d.capabilities.max_height : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribRateControl:
            // CQP is what FFmpeg picks when the client gives no bitrate;
            // CBR when bitrate == maxrate, VBR otherwise.
            attributes[i].value = encode ? (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP) : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxRefFrames:
            // l0 in the low 16 bits, l1 in the high half. One L0 reference,
            // none ahead of the current picture: the firmware keeps the
            // previous reconstructed frame and nothing else, which is why
            // the encoder refuses num_ref_idx_l0_active_minus1 != 0 and
            // there are no B-frames. Advertising 2 here would promise a
            // second reference the picture parameters cannot ask for.
            attributes[i].value = encode ? 1 : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncPackedHeaders:
            // NONE, not NOT_SUPPORTED. The firmware writes SPS/PPS itself, so
            // there is no packed header to accept, but "not supported" tells a
            // client the attribute is unknown and GStreamer's vah264enc /
            // vah265enc refuse to open a driver that answers that way. Value
            // zero means "known, and no packed header is supported", which is
            // exactly the truth.
            attributes[i].value = encode ? VA_ENC_PACKED_HEADER_NONE : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxSlices:
            // One slice per picture: the firmware decides the slice layout.
            // GStreamer reads this and clamps its own slice count to it, so a
            // NOT_SUPPORTED answer makes it compute -1.
            attributes[i].value = encode ? 1 : VA_ATTRIB_NOT_SUPPORTED;
            break;
        default:
            // Slice structure, ROI, quality range and tiles stay unsupported:
            // the firmware decides the layout.
            attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus createConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attributes,
    int count, VAConfigID* id)
{
    return guarded(ctx, "createConfig", [&] {
        auto& d = data(ctx);
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        iris::require(entrypoint == VAEntrypointVLD || entrypoint == VAEntrypointEncSlice, "unsupported entrypoint",
            VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
        if (entrypoint == VAEntrypointVLD) {
            iris::require(d.capabilities.profiles.count(profile) > 0, "profile not advertised",
                VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        } else {
            iris::require(d.capabilities.encode_profiles.count(profile) > 0, "encode profile not advertised",
                VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        }
        uint32_t rt_format = VA_RT_FORMAT_YUV420;
        if (entrypoint == VAEntrypointVLD && d.capabilities.p010 && p010_decode_profile(profile))
            rt_format |= VA_RT_FORMAT_YUV420_10;
        uint32_t rate_control = VA_RC_VBR;
        for (int i = 0; i < count; ++i) {
            if (attributes[i].type == VAConfigAttribRTFormat && attributes[i].value != VA_ATTRIB_NOT_SUPPORTED)
                rt_format = attributes[i].value;
            if (attributes[i].type == VAConfigAttribRateControl && attributes[i].value != VA_ATTRIB_NOT_SUPPORTED)
                rate_control = attributes[i].value;
        }
        if (entrypoint == VAEntrypointEncSlice) {
            // One bit, chosen by the client from what getConfigAttributes
            // advertised.
            iris::require((rate_control & (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP)) != 0,
                "unsupported rate control mode", VA_STATUS_ERROR_INVALID_PARAMETER);
            iris::require(rt_format & VA_RT_FORMAT_YUV420, "encode needs YUV420",
                VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        } else {
            iris::require(rt_format & (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10), "unsupported RT format",
                VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        }
        *id = d.next_id++;
        d.configs[*id] = Config { profile, entrypoint, rt_format, rate_control };
        return VA_STATUS_SUCCESS;
    });
}

VAStatus destroyConfig(VADriverContextP ctx, VAConfigID id)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    return d.configs.erase(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONFIG;
}

VAStatus queryConfigAttributes(VADriverContextP ctx, VAConfigID id, VAProfile* profile, VAEntrypoint* entrypoint,
    VAConfigAttrib* attributes, int* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.configs.find(id);
    if (it == d.configs.end())
        return VA_STATUS_ERROR_INVALID_CONFIG;
    if (profile)
        *profile = it->second.profile;
    if (entrypoint)
        *entrypoint = it->second.entrypoint;
    if (count)
        *count = 1;
    if (attributes) {
        attributes[0].type = VAConfigAttribRTFormat;
        attributes[0].value = it->second.rt_format;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus queryDisplayAttributes(VADriverContextP, VADisplayAttribute*, int* count)
{
    *count = 0;
    return VA_STATUS_SUCCESS;
}
VAStatus getDisplayAttributes(VADriverContextP, VADisplayAttribute*, int) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus setDisplayAttributes(VADriverContextP, VADisplayAttribute*, int) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

} // namespace va
