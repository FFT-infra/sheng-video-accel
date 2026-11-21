// SPDX-License-Identifier: MIT

#include "driver.h"

#include "../util/error.h"


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

// The RT-format mask a VLD config answers, and the default when a client
// opens one without an RT-format attribute (FFmpeg does this for VP9 P2).
// Both surfaces must agree, so one function computes them.
uint32_t decode_rt_format(const DriverData& d, VAProfile profile)
{
    // VP9 Profile 2 has no legal 8-bit stream: libvpx refuses to encode one
    // and src/codec/vp9.cc requires bit_depth == 10. HEVC Main10 and AV1
    // Profile0 also decode 8-bit streams, so they keep both bits.
    if (profile == VAProfileVP9Profile2 && d.capabilities.p010)
        return VA_RT_FORMAT_YUV420_10;
    uint32_t value = VA_RT_FORMAT_YUV420;
    if (p010_decode_profile(profile) && d.capabilities.p010)
        value |= VA_RT_FORMAT_YUV420_10;
    return value;
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
                attributes[i].value = decode_rt_format(d, profile);
            } else if (encode) {
                // The encoder's raw port is NV12, or P010 for Main 10.
                attributes[i].value = profile == VAProfileHEVCMain10 ? VA_RT_FORMAT_YUV420_10
                                                                    : VA_RT_FORMAT_YUV420;
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
            // l0 in the low 16 bits, l1 in the high half.
            attributes[i].value = encode
                ? (1u | (d.options.b_frames && !hevc_encode ? (1u << 16) : 0u))
                : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncPackedHeaders:
            // HEVC only: its raw path is the client's SEI. h264_vaapi's
            // buffering-period SEI names the SPS by id and FFmpeg writes no
            // parameter sets for MISC alone, so every CBR/VBR encode fails in
            // the client.
            attributes[i].value = !encode ? VA_ATTRIB_NOT_SUPPORTED
                : hevc_encode       ? VA_ENC_PACKED_HEADER_MISC
                                    : VA_ENC_PACKED_HEADER_NONE;
            break;
        case VAConfigAttribEncMaxSlices:
            // GStreamer clamps its own count to this; NOT_SUPPORTED makes it
            // compute -1. 128 is the kernel's ceiling.
            attributes[i].value = encode ? (d.capabilities.encode_multi_slice ? 128u : 1u)
                                         : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncInterlaced:
            // Progressive only. NOT_SUPPORTED would make a client guess.
            attributes[i].value = encode ? VA_ENC_INTERLACED_NONE : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncSliceStructure:
            // A slice may begin at any macroblock. Without the multi-slice
            // controls there is one full-picture slice, which EQUAL_ROWS is
            // the closest VA shape for.
            attributes[i].value = encode
                ? (d.capabilities.encode_multi_slice ? VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS
                                                     : VA_ENC_SLICE_STRUCTURE_EQUAL_ROWS)
                : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncQualityRange:
            // No speed/quality knob: one level, not an unknown attribute.
            attributes[i].value = encode ? 1 : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribPredictionDirection:
            // FFmpeg reads FUTURE as the licence to code B pictures; without
            // it `-bf` falls back to intra and P.
            attributes[i].value = encode
                ? (VA_PREDICTION_DIRECTION_PREVIOUS
                      | (d.options.b_frames && !hevc_encode ? VA_PREDICTION_DIRECTION_FUTURE : 0))
                : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribDecAV1Features:
            // Large-scale tiles are unsupported; 0 says "known, no features".
            attributes[i].value = (decode && profile == VAProfileAV1Profile0)
                ? 0 : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncRateControlExt: {
            // Six layers, no per-layer bitrates: the firmware ignores them
            // while a total is set.
            if (!encode || !d.capabilities.encode_temporal_layers) {
                attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
                break;
            }
            VAConfigAttribValEncRateControlExt extended = {};
            extended.bits.max_num_temporal_layers_minus1 = 5;
            extended.bits.temporal_layer_bitrate_control_flag = 0;
            attributes[i].value = extended.value;
            break;
        }
        case VAConfigAttribEncROI: {
            // The node's ROI control takes delta-QP per macroblock (0017), so
            // that is the only form advertised.
            if (!encode || !d.capabilities.encode_roi_regions) {
                attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
                break;
            }
            VAConfigAttribValEncROI roi = {};
            roi.bits.num_roi_regions = kMaxRoiRegions;
            roi.bits.roi_rc_priority_support = 0;
            roi.bits.roi_rc_qp_delta_support = 1;
            attributes[i].value = roi.value;
            break;
        }
        case VAConfigAttribEncIntraRefresh:
            // One band of rows per picture: rolling row only, CBR only.
            attributes[i].value = encode ? VA_ENC_INTRA_REFRESH_ROLLING_ROW : VA_ATTRIB_NOT_SUPPORTED;
            break;
#if VA_CHECK_VERSION(1, 13, 0)
        case VAConfigAttribEncHEVCFeatures: {
            // Answers the firmware's own SPS, not the client's request.
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
            // 32x32 CTB, 8x8 smallest block, 4x4..32x32 transforms, no
            // hierarchy. FFmpeg otherwise guesses and logs that it guessed.
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
            // ROI, quantization, JPEG, tiling and the extended rate-control
            // surface stay unsupported: the firmware decides the layout.
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
        if (entrypoint == VAEntrypointVLD)
            rt_format = decode_rt_format(d, profile);
        // FFmpeg probes a config with no attributes; a Main 10 probe must not
        // default to 8 bits.
        if (entrypoint == VAEntrypointEncSlice && profile == VAProfileHEVCMain10)
            rt_format = VA_RT_FORMAT_YUV420_10;
        if (entrypoint == VAEntrypointVideoProc && d.capabilities.p010)
            rt_format |= VA_RT_FORMAT_YUV420_10;
        uint32_t rate_control = VA_RC_VBR;
        for (int i = 0; i < count; ++i) {
            if (attributes[i].value == VA_ATTRIB_NOT_SUPPORTED)
                continue;
            // Refuse an unhonourable value now instead of at renderPicture;
            // an unknown type stays accepted for newer clients.
            VAConfigAttrib query = { attributes[i].type, 0 };
            getConfigAttributes(ctx, profile, entrypoint, &query, 1);
            if (query.value != VA_ATTRIB_NOT_SUPPORTED) {
                // An RT-format the driver cannot honour is a format refusal,
                // not a generic attribute one: clients key off this status to
                // pick another surface format.
                const VAStatus status = attributes[i].type == VAConfigAttribRTFormat
                    ? VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT : VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                iris::require((attributes[i].value & ~query.value) == 0,
                    "unsupported config attribute value", status);
            }
            if (attributes[i].type == VAConfigAttribRTFormat)
                rt_format = attributes[i].value;
            if (attributes[i].type == VAConfigAttribRateControl)
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
            if (profile == VAProfileHEVCMain10)
                iris::require(rt_format & VA_RT_FORMAT_YUV420_10, "Main 10 encode needs YUV420_10",
                    VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
            else
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
