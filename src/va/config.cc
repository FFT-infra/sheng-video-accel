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
    // VAProfileNone carries no bitstream; it exists only so a client can open
    // a VideoProc config. Advertised only when the backend is built in and
    // the client opted in.
    if (vpp_available(d))
        all.insert(VAProfileNone);
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
    if (profile == VAProfileNone) {
        if (vpp_available(d))
            entrypoints[n++] = VAEntrypointVideoProc;
        *count = n;
        return VA_STATUS_SUCCESS;
    }
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
    // The HEVC feature and block-size attributes describe the parameter sets
    // the firmware writes, so they answer only for a profile that HEVC encodes.
    const bool hevc_encode = encode
        && (profile == VAProfileHEVCMain || profile == VAProfileHEVCMain10);
    const bool vpp = profile == VAProfileNone && entrypoint == VAEntrypointVideoProc && vpp_available(d);
    for (int i = 0; i < count; ++i) {
        switch (attributes[i].type) {
        case VAConfigAttribRTFormat:
            if (decode) {
                attributes[i].value = VA_RT_FORMAT_YUV420;
                if (p010_decode_profile(profile) && d.capabilities.p010)
                    attributes[i].value |= VA_RT_FORMAT_YUV420_10;
            } else if (encode) {
                // The encoder's raw port is NV12 only.
                attributes[i].value = VA_RT_FORMAT_YUV420;
            } else if (vpp) {
                // The processor scales either width; a picture whose two ends
                // disagree on bit depth is refused at endPicture. RGB32 is the
                // blended overlay source, which is a surface of its own config.
                attributes[i].value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;
                if (d.capabilities.p010)
                    attributes[i].value |= VA_RT_FORMAT_YUV420_10;
            } else {
                attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
            }
            break;
        case VAConfigAttribDecSliceMode:
            attributes[i].value = decode ? VA_DEC_SLICE_MODE_NORMAL : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureWidth:
            attributes[i].value = (decode || encode || vpp) ? d.capabilities.max_width : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureHeight:
            attributes[i].value = (decode || encode || vpp) ? d.capabilities.max_height : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribRateControl:
            // CQP is what FFmpeg picks when the client gives no bitrate;
            // CBR when bitrate == maxrate, VBR otherwise.
            attributes[i].value = encode ? (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP) : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxRefFrames:
            // l0 in the low 16 bits, l1 in the high half. One reference in
            // the past; a future one only for an opted-in H.264 encode,
            // where it is what makes a client offer ip_period 2. A client
            // that sees l1 = 0 turns B pictures off, which is the shape this
            // driver's default client can drive: the firmware holds each
            // queued picture until the next one arrives, so B pictures need
            // a client pipeline one picture deeper than FFmpeg's default.
            // More than one reference per list stays refused; the picture
            // parameters cannot ask for it.
            attributes[i].value = encode
                ? (1u | (d.options.b_frames && !hevc_encode ? (1u << 16) : 0u))
                : VA_ATTRIB_NOT_SUPPORTED;
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
#if VA_CHECK_VERSION(1, 13, 0)
        case VAConfigAttribEncHEVCFeatures: {
            // The firmware writes the parameter sets, so the answer is what a
            // trace_headers dump of its own SPS/PPS says rather than what a
            // client asked for (TEST-RESULTS.md, "HEVC encode attributes").
            // SAO, temporal MVP and cu_qp_delta are on in every stream and no
            // picture parameter turns them off, which is REQUIRED; the slice's
            // deblocking-disable flag is the one the encoder acts on, which is
            // SUPPORTED. AMP, PCM, transform skip, scaling lists, sign data
            // hiding and the rest are zero in that SPS and stay unsupported.
            if (!hevc_encode) {
                attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
                break;
            }
            VAConfigAttribValEncHEVCFeatures features = {};
            features.bits.sao = VA_FEATURE_REQUIRED;
            features.bits.temporal_mvp = VA_FEATURE_REQUIRED;
            features.bits.cu_qp_delta = VA_FEATURE_REQUIRED;
            features.bits.deblocking_filter_disable = VA_FEATURE_SUPPORTED;
            attributes[i].value = features.value;
            break;
        }
        case VAConfigAttribEncHEVCBlockSizes: {
            // log2_min_luma_coding_block_size_minus3 = 0, log2_diff_max_min =
            // 2, log2_min_luma_transform_block_size_minus2 = 0 and
            // log2_diff_max_min = 3: one 32x32 CTB size, 8x8 smallest coding
            // block, 4x4 to 32x32 transforms, no transform hierarchy. FFmpeg
            // otherwise guesses 32/16 and logs that it guessed.
            if (!hevc_encode) {
                attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
                break;
            }
            VAConfigAttribValEncHEVCBlockSizes sizes = {};
            sizes.bits.log2_max_coding_tree_block_size_minus3 = 2;
            sizes.bits.log2_min_coding_tree_block_size_minus3 = 2;
            sizes.bits.log2_min_luma_coding_block_size_minus3 = 0;
            sizes.bits.log2_max_luma_transform_block_size_minus2 = 3;
            sizes.bits.log2_min_luma_transform_block_size_minus2 = 0;
            sizes.bits.max_max_transform_hierarchy_depth_inter = 0;
            sizes.bits.max_max_transform_hierarchy_depth_intra = 0;
            attributes[i].value = sizes.value;
            break;
        }
#endif
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
        iris::require(entrypoint == VAEntrypointVLD || entrypoint == VAEntrypointEncSlice
                || entrypoint == VAEntrypointVideoProc,
            "unsupported entrypoint", VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
        if (entrypoint == VAEntrypointVideoProc) {
            iris::require(profile == VAProfileNone && vpp_available(d), "VPP not advertised",
                VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        } else if (entrypoint == VAEntrypointVLD) {
            iris::require(d.capabilities.profiles.count(profile) > 0, "profile not advertised",
                VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        } else {
            iris::require(d.capabilities.encode_profiles.count(profile) > 0, "encode profile not advertised",
                VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        }
        uint32_t rt_format = VA_RT_FORMAT_YUV420;
        if (entrypoint == VAEntrypointVLD && d.capabilities.p010 && p010_decode_profile(profile))
            rt_format |= VA_RT_FORMAT_YUV420_10;
        if (entrypoint == VAEntrypointVideoProc && d.capabilities.p010)
            rt_format |= VA_RT_FORMAT_YUV420_10;
        uint32_t rate_control = VA_RC_VBR;
        for (int i = 0; i < count; ++i) {
            if (attributes[i].type == VAConfigAttribRTFormat && attributes[i].value != VA_ATTRIB_NOT_SUPPORTED)
                rt_format = attributes[i].value;
            if (attributes[i].type == VAConfigAttribRateControl && attributes[i].value != VA_ATTRIB_NOT_SUPPORTED)
                rate_control = attributes[i].value;
        }
        if (entrypoint == VAEntrypointVideoProc) {
            // One bit per config: the client names the depth of the surfaces it
            // will create, and the picture refuses a mismatch.
            iris::require((rt_format & (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10)) != 0
                    && !(rt_format & ~(VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10 | VA_RT_FORMAT_RGB32)),
                "VPP needs YUV420 or YUV420_10", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        } else if (entrypoint == VAEntrypointEncSlice) {
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
