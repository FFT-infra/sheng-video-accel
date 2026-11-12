// SPDX-License-Identifier: MIT
//
// Driver init/terminate, capability probe, and the helpers every other
// src/va file shares.

#include "driver.h"

#include "../util/error.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {
#include <linux/v4l2-controls.h>
#include <va/va_drmcommon.h>
}

namespace va {

VAStatus report(VADriverContextP ctx, const char* where, const std::exception& error)
{
    VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
    if (const auto* iris_error = dynamic_cast<const iris::Error*>(&error))
        status = iris_error->status;
    if (ctx && ctx->error_callback)
        ctx->error_callback(ctx, const_cast<char*>((std::string(where) + ": " + error.what() + "\n").c_str()));
    data(ctx).trace("va error where=%s status=%d: %s", where, status, error.what());
    return status;
}

std::shared_ptr<Surface> surface(DriverData& d, VASurfaceID id)
{
    auto it = d.surfaces.find(id);
    iris::require(it != d.surfaces.end(), "invalid surface", VA_STATUS_ERROR_INVALID_SURFACE);
    return it->second;
}

std::shared_ptr<Context> context(DriverData& d, VAContextID id)
{
    auto it = d.contexts.find(id);
    iris::require(it != d.contexts.end(), "invalid context", VA_STATUS_ERROR_INVALID_CONTEXT);
    return it->second;
}

std::shared_ptr<Buffer> buffer(DriverData& d, VABufferID id)
{
    auto it = d.buffers.find(id);
    iris::require(it != d.buffers.end(), "invalid buffer", VA_STATUS_ERROR_INVALID_BUFFER);
    return it->second;
}

std::shared_ptr<Context> owner_of(DriverData& d, const Surface& surface)
{
    if (surface.owner == VA_INVALID_ID)
        return nullptr;
    auto it = d.contexts.find(surface.owner);
    return it == d.contexts.end() ? nullptr : it->second;
}

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
    if (!surface.persistent_export)
        return iris::Publish::Direct;
    // A pre-exporting client (Chrome) gets its own buffer decoded into when
    // the kernel and the session allow it, and the GPU copy otherwise.
    return session.supports_import() ? iris::Publish::Import : iris::Publish::Copy;
}

iris::StableBuffer& ensure_stable(DriverData& d, Surface& surface)
{
    if (!surface.stable)
        surface.stable = iris::allocate_stable(d.render_fd, iris::stable_layout(surface.fourcc, surface.width, surface.height));
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

    // Profiles: every entry here is qualified in
    // data/iris-codec-capabilities.json; a codec is advertised only after its
    // hardware gate passed (architecture §9, Phase 5).
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
    // VP9 passed its Phase 5 gate on the decode-order module (matrix,
    // 100 context recreations, 100 resolution switches, Chrome 1080p with
    // alt-ref); see TEST-RESULTS.md.
    if (caps.output_formats.count(V4L2_PIX_FMT_VP9) && caps.nv12) {
        caps.profiles.insert(VAProfileVP9Profile0);
        if (caps.p010)
            caps.profiles.insert(VAProfileVP9Profile2);
    }
    // AV1 passed its first gate on 2026-10-22 (matrix, FFmpeg over alt-ref
    // and no-alt-ref streams bit-exact against libdav1d, Chrome 152). The
    // internal-DPB translator submits each picture at vaEndPicture, so a
    // client that syncs each surface never waits for a successor; its own
    // installed-package matrix, clip, DRC and browser gates are recorded in
    // TEST-RESULTS.md.
    if (caps.output_formats.count(V4L2_PIX_FMT_AV1) && caps.nv12)
        caps.profiles.insert(VAProfileAV1Profile0);
    // profile-qualified-end
    // Import is qualified per codec, separately from decoding.
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
    // VP9 qualified on 2026-11-12 once a released surface stopped costing its
    // picture: Chrome destroys a VP9 surface as soon as it has composited the
    // frame, and the session used to drop the still-in-flight completion.
    // The target now keeps the target and completes it (`target detached`),
    // and the 2026-11-12 runs in TEST-RESULTS.md record 48/48 four-codec
    // matrix, FFmpeg 515/515 with framemd5 equal to the software decode, and
    // two guarded Chrome windows at 1705/1705 and 1588/1588 with drops=0,
    // against a Copy baseline that dropped 3 of 1705. AV1's internal-DPB
    // translator submits at vaEndPicture; Import lends one client buffer per
    // completion, including the invisible half of a hidden install unit. The
    // 2026-11-11 installed-package runs qualified that default path: 48/48
    // four-codec matrix, 120/120 AV1 and 80/80 DRC exact framemd5 under
    // Import; the DRC clip switches to copy-gpu for its second half. Firefox
    // 158 and Chrome 156 both decode live AV1 pages with the selected
    // hardware decoder.

    // The encoder is a second iris_driver node. Its absence is not fatal:
    // the display still decodes, and no encode entrypoint is advertised.
    try {
        auto encoder = iris::open_device(options.encoder_path, iris::NodeRole::Encoder);
        caps.encoder_node = encoder->path();
        const bool encoder_nv12 = encoder->format_supported(iris::Queue::Output, V4L2_PIX_FMT_NV12);
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

    ctx->pDriverData = d;
    return VA_STATUS_SUCCESS;
}

// libva's loader has looked up the 1.0 entry point on some distributions;
// keep it so one binary works across libva 1.x.
extern "C" VAStatus __attribute__((visibility("default"))) __vaDriverInit_1_0(VADriverContextP ctx)
{
    return VA_DRIVER_INIT_FUNC(ctx);
}
