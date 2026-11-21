// SPDX-License-Identifier: MIT
//
// Driver init/terminate, capability probe, and the helpers every other
// src/va file shares.

#include "driver.h"

#include "../iris/vpp/scale.h"
#include "../util/error.h"
#include "../util/prime.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include <linux/v4l2-controls.h>
#include <va/va_drmcommon.h>
}

namespace va {

bool p010_decode_profile(VAProfile profile)
{
    // AV1 signals 8- and 10-bit inside Profile 0, so the format follows the
    // stream, not the profile. The firmware picks P010 from the bitstream
    // itself (iris_hfi_gen2_set_bit_depth always sends BIT_DEPTH_8) and
    // refuses to grant NV12 afterwards, so the surfaces have to be P010
    // before the first access unit.
    return profile == VAProfileHEVCMain10 || profile == VAProfileVP9Profile2 || profile == VAProfileAV1Profile0;
}

iris::Publish publish_policy(const DriverData& d, const Surface& surface, const iris::Session& session)
{
    // A UBWC surface is compressed memory: only Direct (the surface takes the
    // slot) and Import (the client's own compressed buffer) have a reader.
    // Copy would have to decompress, which no engine here does.
    if (surface.modifier == DRM_FORMAT_MOD_QCOM_COMPRESSED)
        return session.supports_import() && surface.persistent_export ? iris::Publish::Import
                                                                      : iris::Publish::Direct;
    switch (d.options.publish) {
    case iris::PublishOverride::Copy:
        return iris::Publish::Copy;
    case iris::PublishOverride::Direct:
        return iris::Publish::Direct;
    case iris::PublishOverride::Import:
        return session.supports_import() ? iris::Publish::Import : iris::Publish::Copy;
    case iris::PublishOverride::Auto:
        break;
    }
    if (!surface.persistent_export) {
        // Direct pins a CAPTURE frame until the client consumes the picture.
        // Where the memory budget leaves the pool no headroom over the
        // firmware minimum, a client whose consumer lags can pin every slot
        // and stall the session (8K, FFmpeg's hwdownload backlog); Copy
        // releases the slot at the completion.
        return session.can_publish_direct() ? iris::Publish::Direct : iris::Publish::Copy;
    }
    // A pre-exporting client (Chrome) gets its own buffer decoded into when
    // the kernel and the session allow it, and the GPU copy otherwise.
    return session.supports_import() ? iris::Publish::Import : iris::Publish::Copy;
}

bool vpp_available(const DriverData& d)
{
    return d.options.vpp_enabled && iris::vpp::compiled_in();
}

void require_linear(DriverData& d, const Surface& surface, const char* operation)
{
    if (surface.modifier != DRM_FORMAT_MOD_QCOM_COMPRESSED)
        return;
    d.trace("va ubwc refused operation=%s", operation);
    throw iris::Error(VA_STATUS_ERROR_UNIMPLEMENTED,
        std::string("a UBWC surface has no linear pixels for ") + operation);
}

iris::StableBuffer& ensure_stable(DriverData& d, Surface& surface)
{
    // An imported UBWC surface already owns its buffer: the client's
    // compressed memory, which Import publish decodes into. Only the
    // allocation is refused — there is no linear layout to allocate.
    if (!surface.stable) {
        require_linear(d, surface, "the stable buffer");
        surface.stable = iris::allocate_stable(d.render_fd, iris::stable_layout(surface.fourcc, surface.width, surface.height));
    }
    return *surface.stable;
}

Capabilities probe_capabilities(const iris::Options& options)
{
    Capabilities caps;
    auto device = iris::open_device(options.video_path);
    caps.node = device->path();
    for (uint32_t format : { V4L2_PIX_FMT_H264, V4L2_PIX_FMT_HEVC, V4L2_PIX_FMT_VP9, V4L2_PIX_FMT_AV1 })
        if (device->format_supported(iris::Queue::Output, format))
            caps.output_formats.insert(format);
    caps.nv12 = device->format_supported(iris::Queue::Capture, V4L2_PIX_FMT_NV12);
    caps.p010 = device->format_supported(iris::Queue::Capture, V4L2_PIX_FMT_P010);
    // UBWC is only offered when the node can actually be asked for the Q08C
    // CAPTURE format; V4L2_VA_UBWC=0 turns it off.
    caps.ubwc = options.ubwc && device->format_supported(iris::Queue::Capture, V4L2_PIX_FMT_QC08C);

    // Profiles: every entry here is qualified in
    // data/iris-codec-capabilities.json; a codec is advertised only after its
    // hardware gate passed (docs/architecture.md §9).
    // profile-qualified-begin
    if (caps.output_formats.count(V4L2_PIX_FMT_H264) && caps.nv12) {
        caps.profiles.insert(VAProfileH264ConstrainedBaseline);
        caps.profiles.insert(VAProfileH264Main);
        caps.profiles.insert(VAProfileH264High);
    }
    if (caps.output_formats.count(V4L2_PIX_FMT_HEVC) && caps.nv12) {
        caps.profiles.insert(VAProfileHEVCMain);
        if (caps.p010)
            caps.profiles.insert(VAProfileHEVCMain10);
    }
    // VP9 is qualified on the decode-order module (matrix, 100 context
    // recreations, 100 resolution switches, Chrome 1080p with alt-ref); see
    // TEST-RESULTS.md.
    if (caps.output_formats.count(V4L2_PIX_FMT_VP9) && caps.nv12) {
        caps.profiles.insert(VAProfileVP9Profile0);
        if (caps.p010)
            caps.profiles.insert(VAProfileVP9Profile2);
    }
    if (caps.output_formats.count(V4L2_PIX_FMT_AV1) && caps.nv12)
        caps.profiles.insert(VAProfileAV1Profile0);
    // profile-qualified-end
    // import-qualified-begin
    if (caps.output_formats.count(V4L2_PIX_FMT_H264) && caps.nv12)
        caps.import_formats.insert(V4L2_PIX_FMT_H264);
    if (caps.output_formats.count(V4L2_PIX_FMT_HEVC) && caps.nv12)
        caps.import_formats.insert(V4L2_PIX_FMT_HEVC);
    if (caps.output_formats.count(V4L2_PIX_FMT_VP9) && caps.nv12)
        caps.import_formats.insert(V4L2_PIX_FMT_VP9);
    if (caps.output_formats.count(V4L2_PIX_FMT_AV1) && caps.nv12)
        caps.import_formats.insert(V4L2_PIX_FMT_AV1);
    // import-qualified-end
    // VP9 and AV1 qualify through Import only on the decode-order kernel: a
    // surface Chrome releases after compositing must still be completed
    // (`target detached`), and AV1's internal-DPB translator lends one client
    // buffer per completion, including the invisible half of a hidden install
    // unit. Runs, counters and refusals: TEST-RESULTS.md, "VP9 Import
    // qualified" and "AV1 Import qualification".

    // The encoder is a second iris_driver node. Its absence is not fatal:
    // the display still decodes, and no encode entrypoint is advertised.
    try {
        auto encoder = iris::open_device(options.encoder_path, iris::NodeRole::Encoder);
        caps.encoder_node = encoder->path();
        const bool encoder_nv12 = encoder->format_supported(iris::Queue::Output, V4L2_PIX_FMT_NV12);
        // The multi-slice controls arrive with the kernel patch. Probing the
        // menu is enough: all three are registered together, and a module
        // without the patch answers no control at all.
        caps.encode_multi_slice
            = encoder->get_control(V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE).has_value();
        caps.encode_p010 = encoder->format_supported(iris::Queue::Output, V4L2_PIX_FMT_P010);
        // The hierarchical controls are one set; the H.264 enable and the
        // HEVC layer count are the two switches the session writes.
        caps.encode_temporal_layers
            = encoder->get_control(V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING).has_value()
            && encoder->get_control(V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_LAYER).has_value();
        // The ROI map is the kernel patch's own control, written as a dynamic
        // s32 array. A module without it answers no control, and a control of
        // any other shape is not the one the session writes: advertising ROI
        // then would fail at the first picture. Advertising also needs the
        // opt-in: the qualified firmware completes the metadata buffer but
        // codes the same picture either way, so a client would see regions
        // accepted and ignored (TEST-RESULTS.md).
        const auto roi = encoder->query_control(iris::kRoiDeltaQpControl);
        if (options.roi && roi && roi->type == V4L2_CTRL_TYPE_INTEGER
            && roi->elem_size == sizeof(int32_t) && (roi->flags & V4L2_CTRL_FLAG_DYNAMIC_ARRAY)
            && roi->dims[0] > 0)
            caps.encode_roi_regions = kMaxRoiRegions;
        for (uint32_t format : { V4L2_PIX_FMT_H264, V4L2_PIX_FMT_HEVC })
            if (encoder->format_supported(iris::Queue::Capture, format))
                caps.encoder_formats.insert(format);
        // encode-qualified-begin
        if (caps.encoder_formats.count(V4L2_PIX_FMT_H264) && encoder_nv12) {
            caps.encode_profiles.insert(VAProfileH264ConstrainedBaseline);
            caps.encode_profiles.insert(VAProfileH264Main);
            caps.encode_profiles.insert(VAProfileH264High);
        }
        if (caps.encoder_formats.count(V4L2_PIX_FMT_HEVC) && encoder_nv12)
            caps.encode_profiles.insert(VAProfileHEVCMain);
        // Main 10 is a second profile on the same codec, and the kernel
        // module decides whether its raw port takes P010.
        if (caps.encoder_formats.count(V4L2_PIX_FMT_HEVC) && caps.encode_p010)
            caps.encode_profiles.insert(VAProfileHEVCMain10);
        // encode-qualified-end
    } catch (const std::exception&) {
        // An explicit override that does not name an encoder is a
        // misconfiguration, not an absent node.
        if (!options.encoder_path.empty())
            throw;
    }
    return caps;
}

namespace {

int render_fd_of(VADriverContextP ctx)
{
    if (!ctx->drm_state)
        return -1;
    const auto* state = static_cast<const drm_state*>(ctx->drm_state);
    return (ctx->display_type & VA_DISPLAY_DRM) ? state->fd : -1;
}

} // namespace

void fill_context(VADriverContextP ctx, DriverData& d)
{
    ctx->version_major = VA_MAJOR_VERSION;
    ctx->version_minor = VA_MINOR_VERSION;
    ctx->max_profiles = kMaxProfiles;
    ctx->max_entrypoints = kMaxEntrypoints;
    ctx->max_attributes = kMaxAttributes;
    ctx->max_image_formats = kMaxImageFormats;
    ctx->max_subpic_formats = kMaxSubpictureFormats;
    ctx->max_display_attributes = kMaxDisplayAttributes;
    ctx->str_vendor = kVendor;

    VADriverVTable* vtable = ctx->vtable;
    vtable->vaTerminate = terminate;
    vtable->vaQueryConfigProfiles = queryConfigProfiles;
    vtable->vaQueryConfigEntrypoints = queryConfigEntrypoints;
    vtable->vaQueryConfigAttributes = queryConfigAttributes;
    vtable->vaGetConfigAttributes = getConfigAttributes;
    vtable->vaCreateConfig = createConfig;
    vtable->vaDestroyConfig = destroyConfig;
    vtable->vaQueryDisplayAttributes = queryDisplayAttributes;
    vtable->vaGetDisplayAttributes = getDisplayAttributes;
    vtable->vaSetDisplayAttributes = setDisplayAttributes;
    vtable->vaCreateSurfaces = createSurfaces;
    vtable->vaCreateSurfaces2 = createSurfaces2;
    vtable->vaDestroySurfaces = destroySurfaces;
    vtable->vaSyncSurface = syncSurface;
    vtable->vaSyncBuffer = syncBuffer;
    vtable->vaQuerySurfaceStatus = querySurfaceStatus;
    vtable->vaQuerySurfaceError = querySurfaceError;
    vtable->vaQuerySurfaceAttributes = querySurfaceAttributes;
    vtable->vaExportSurfaceHandle = exportSurfaceHandle;
    vtable->vaPutSurface = putSurface;
    vtable->vaLockSurface = lockSurface;
    vtable->vaUnlockSurface = unlockSurface;
    vtable->vaCreateContext = createContext;
    vtable->vaDestroyContext = destroyContext;
    vtable->vaBeginPicture = beginPicture;
    vtable->vaRenderPicture = renderPicture;
    vtable->vaEndPicture = endPicture;
    vtable->vaCreateBuffer = createBuffer;
    vtable->vaDestroyBuffer = destroyBuffer;
    vtable->vaMapBuffer = mapBuffer;
    vtable->vaUnmapBuffer = unmapBuffer;
    vtable->vaBufferSetNumElements = bufferSetNumElements;
    vtable->vaBufferInfo = bufferInfo;
    vtable->vaAcquireBufferHandle = acquireBufferHandle;
    vtable->vaReleaseBufferHandle = releaseBufferHandle;
    vtable->vaQueryImageFormats = queryImageFormats;
    vtable->vaCreateImage = createImage;
    vtable->vaDeriveImage = deriveImage;
    vtable->vaDestroyImage = destroyImage;
    vtable->vaSetImagePalette = setImagePalette;
    vtable->vaGetImage = getImage;
    vtable->vaPutImage = putImage;
    vtable->vaQuerySubpictureFormats = querySubpictureFormats;
    vtable->vaCreateSubpicture = createSubpicture;
    vtable->vaDestroySubpicture = destroySubpicture;
    vtable->vaSetSubpictureImage = setSubpictureImage;
    vtable->vaSetSubpictureChromakey = setSubpictureChromakey;
    vtable->vaSetSubpictureGlobalAlpha = setSubpictureGlobalAlpha;
    vtable->vaAssociateSubpicture = associateSubpicture;
    vtable->vaDeassociateSubpicture = deassociateSubpicture;

    // The VPP vtable is separate and may be absent on an older libva. Only
    // fill it in when this build and the client both opted in; otherwise a
    // client that probes VAProfileNone gets nothing to call.
    if (ctx->vtable_vpp && vpp_available(d)) {
        ctx->vtable_vpp->version = VA_DRIVER_VTABLE_VPP_VERSION;
        ctx->vtable_vpp->vaQueryVideoProcFilters = queryVideoProcFilters;
        ctx->vtable_vpp->vaQueryVideoProcFilterCaps = queryVideoProcFilterCaps;
        ctx->vtable_vpp->vaQueryVideoProcPipelineCaps = queryVideoProcPipelineCaps;
    }
}

VAStatus terminate(VADriverContextP ctx)
{
    auto* d = static_cast<DriverData*>(ctx->pDriverData);
    if (!d)
        return VA_STATUS_SUCCESS;
    // Contexts first: their sessions drain and close before surfaces drop
    // the frames they hold.
    while (!d->contexts.empty())
        destroyContext(ctx, d->contexts.begin()->first);
    d->configs.clear();
    d->images.clear();
    d->buffers.clear();
    d->surfaces.clear();
    d->copier.reset();
    delete d;
    ctx->pDriverData = nullptr;
    return VA_STATUS_SUCCESS;
}

} // namespace va

using namespace va;

extern "C" VAStatus __attribute__((visibility("default"))) VA_DRIVER_INIT_FUNC(VADriverContextP ctx)
{
    auto* d = new DriverData;
    d->options = iris::Options::from_environment();
    d->trace = iris::Trace(d->options.trace);
    try {
        d->capabilities = probe_capabilities(d->options);
    } catch (const std::exception& error) {
        if (ctx->error_callback)
            ctx->error_callback(ctx, const_cast<char*>((std::string("iris: ") + error.what() + "\n").c_str()));
        delete d;
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    d->render_fd = render_fd_of(ctx);
    // Defer copy-engine creation until context creation so capability probing
    // stays thread-free for Chromium GPU sandbox initialization.
    d->trace("va init node=%s encoder=%s render_fd=%d copy=deferred profiles=%zu encode_profiles=%zu",
        d->capabilities.node.c_str(),
        d->capabilities.encoder_node.empty() ? "-" : d->capabilities.encoder_node.c_str(), d->render_fd,
        d->capabilities.profiles.size(), d->capabilities.encode_profiles.size());

    fill_context(ctx, *d);

    ctx->pDriverData = d;
    return VA_STATUS_SUCCESS;
}

// libva's loader has looked up the 1.0 entry point on some distributions;
// keep it so one binary works across libva 1.x.
extern "C" VAStatus __attribute__((visibility("default"))) __vaDriverInit_1_0(VADriverContextP ctx)
{
    return VA_DRIVER_INIT_FUNC(ctx);
}
