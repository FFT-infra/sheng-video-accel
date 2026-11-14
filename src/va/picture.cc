// SPDX-License-Identifier: MIT
//
// Contexts and the picture pipeline. vaBeginPicture releases the target's
// previous frame, vaRenderPicture collects VA buffers, vaEndPicture runs
// the translator and submits one access unit to the session.

#include "driver.h"

#include "../codec/av1_dpb.h"
#include "../iris/vpp/scale.h"
#include "../util/error.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <sys/stat.h>

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

std::unique_ptr<iris::codec::Translator> make_translator(VAProfile profile)
{
    switch (profile) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    case VAProfileH264Baseline:
#pragma GCC diagnostic pop
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        return iris::codec::make_h264(profile);
    case VAProfileHEVCMain:
    case VAProfileHEVCMain10:
        return iris::codec::make_hevc(profile);
    case VAProfileVP9Profile0:
    case VAProfileVP9Profile2:
        return iris::codec::make_vp9(profile);
    case VAProfileAV1Profile0:
        return iris::codec::make_av1_dpb(profile);
    default:
        throw iris::Error(VA_STATUS_ERROR_UNSUPPORTED_PROFILE, "no translator for profile");
    }
}

std::unique_ptr<iris::codec::Encoder> make_encoder(VAProfile profile, iris::RateMode rate_mode)
{
    switch (profile) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    case VAProfileH264Baseline:
#pragma GCC diagnostic pop
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        return iris::codec::make_h264_encoder(profile, rate_mode);
    case VAProfileHEVCMain:
        return iris::codec::make_hevc_encoder(profile, rate_mode);
    default:
        throw iris::Error(VA_STATUS_ERROR_UNSUPPORTED_PROFILE, "no encoder for profile");
    }
}

// Submit one AU for `surface` through `context`'s session. Sets the target's
// publish policy from the surface's export history.
void submit(DriverData& d, Context& c, Surface& s, const iris::codec::AccessUnit& au)
{
    s.target.publish = publish_policy(d, s, *c.session);
    if (s.target.publish != iris::Publish::Direct)
        s.target.destination = &ensure_stable(d, s);
    else
        s.target.destination = nullptr;
    s.target.width = s.width;
    s.target.height = s.height;
    s.target.fourcc = s.fourcc;
    s.target.hidden = au.hidden;
    s.target.completions = au.completions;
    s.failed = false;
    c.session->submit(au.bytes.data(), au.bytes.size(), s.target);
}

// The buffer that holds a surface's current pixels: a completed decode frame
// when it has one, else the stable DMA-BUF (an hwupload target, or a previous
// VideoProc picture's output).
struct Pixels {
    int fd = -1;
    unsigned size = 0;
    iris::Layout layout;
};

Pixels pixels_of(DriverData& d, Surface& s)
{
    Pixels p;
    if (s.target.frame) {
        p.fd = s.target.frame->fd();
        p.size = s.target.frame->length();
        p.layout = s.target.frame->layout();
    } else {
        iris::StableBuffer& stable = ensure_stable(d, s);
        p.fd = stable.fd();
        p.size = stable.size();
        p.layout = stable.layout();
    }
    return p;
}

// Two VA surfaces can wrap one DMA-BUF: a client that imports the same fd
// twice gets two surface ids over the same bytes, and a picture that writes
// into one while reading the other would corrupt itself. The ids differ, so
// the identity has to come from the kernel.
bool same_buffer(const Pixels& a, const Pixels& b)
{
    struct stat sa = {}, sb = {};
    if (a.fd < 0 || b.fd < 0 || fstat(a.fd, &sa) != 0 || fstat(b.fd, &sb) != 0)
        return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

// Wait for a surface's decode to land before the kernel reads it. A client is
// supposed to sync first, and the gates do; FFmpeg's vaapi hwaccel does not —
// it issues the filter picture straight after the decode and relies on one
// command stream ordering the two. Here the decode belongs to the V4L2 node
// and the kernel to Vulkan, so the order is enforced at this boundary. The
// caller must not hold the driver mutex: this waits on a session.
void wait_for_pixels(const std::shared_ptr<Surface>& s, const std::shared_ptr<iris::Session>& session)
{
    if (!session || !s->target.pending)
        return;
    try {
        session->wait(s->target);
    } catch (const iris::Error&) {
        s->failed = true;
        throw;
    }
}

// How many blending passes one picture may carry. VA puts no limit on the
// count and neither does the kernel — each pass is one more dispatch — so this
// is a bound on the work a single vaEndPicture can be asked for, not a
// capability. Sixteen is the sibling driver's figure.
constexpr size_t kMaxOverlays = 16;

// Copy the client's pipeline parameters and refuse every operation the
// compute kernel does not run. The struct's pointers are valid only until
// vaEndPicture, so the regions are dereferenced here rather than kept.
void accept_vpp_pipeline(Context& c, const Buffer& b)
{
    // A picture runs one main pass and any number of blending passes on top of
    // it. The order is part of the contract: each pass that names a blend
    // state blends onto everything before it, so the first one rendered is the
    // bottom layer. Dropping any of them would publish a picture the client
    // did not ask for.
    iris::require(b.size >= sizeof(VAProcPipelineParameterBuffer), "pipeline buffer too small",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    VAProcPipelineParameterBuffer p = {};
    std::memcpy(&p, b.data.data(), sizeof(p));
    const bool blending = p.blend_state != nullptr;
    iris::require(blending ? c.vpp.has_value() : !c.vpp.has_value() && c.vpp_overlays.empty(),
        "the first VideoProc pipeline buffer is the main pass and every later one blends",
        VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    iris::require(c.vpp_overlays.size() < kMaxOverlays, "too many blending passes",
        VA_STATUS_ERROR_UNSUPPORTED_FILTER);

    // One filter is supported, the HDR10 tone mapper. Its parameters travel in
    // their own buffer, so nothing about them is read here.
    iris::require(p.num_filters <= 1, "one VideoProc filter per picture is supported",
        VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    iris::require(p.num_filters == 0 || p.filters != nullptr, "VPP filter list is null",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    iris::require(p.forward_references == nullptr && p.num_forward_references == 0,
        "VPP forward references are not supported", VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    iris::require(p.backward_references == nullptr && p.num_backward_references == 0,
        "VPP backward references are not supported", VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    iris::require(p.additional_outputs == nullptr && p.num_additional_outputs == 0,
        "VPP additional outputs are not supported", VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    iris::require(p.output_hdr_metadata == nullptr, "VPP HDR metadata is not supported",
        VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    // The blend state is copied: the client's pointer dies with vaEndPicture.
    // A global alpha and a premultiplied source are the two blend flags the
    // kernels implement; luma keying is a third mode that does not exist here,
    // so naming it is refused rather than ignored.
    VABlendState blend = {};
    if (blending) {
        blend = *p.blend_state;
        iris::require((blend.flags
                          & ~static_cast<unsigned>(VA_BLEND_GLOBAL_ALPHA | VA_BLEND_PREMULTIPLIED_ALPHA))
                == 0,
            "only global and premultiplied alpha blending is supported", VA_STATUS_ERROR_UNSUPPORTED_FILTER);
        // A blend state without VA_BLEND_GLOBAL_ALPHA uses no global alpha at
        // all, so its alpha field is not read.
        iris::require((blend.flags & VA_BLEND_GLOBAL_ALPHA) == 0
                || (blend.global_alpha >= 0.0f && blend.global_alpha <= 1.0f),
            "blend alpha is outside 0..1", VA_STATUS_ERROR_INVALID_PARAMETER);
    }
    iris::require(p.rotation_state <= VA_ROTATION_270, "invalid VPP rotation",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    iris::require((p.mirror_state & ~(VA_MIRROR_HORIZONTAL | VA_MIRROR_VERTICAL)) == 0,
        "unsupported VPP mirror", VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    iris::require((p.pipeline_flags & ~VA_PROC_PIPELINE_FAST) == 0, "unsupported VPP pipeline flag",
        VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    // Scaling mode is only a hint, so every VA_FILTER_SCALING_* value is
    // accepted. All three interpolation methods have a kernel now: the
    // unspecified one is this driver's default (the point map), the
    // nearest-neighbour one is the same point map under its own name,
    // bilinear is the four-tap blend, and "advanced" is the driver's own
    // pick — the area average when an axis shrinks, where a two-tap filter
    // aliases, and the cubic when it grows.
    const unsigned interpolation = p.filter_flags & VA_FILTER_INTERPOLATION_MASK;
    iris::require(interpolation == 0 || interpolation == VA_FILTER_INTERPOLATION_NEAREST_NEIGHBOR
            || interpolation == VA_FILTER_INTERPOLATION_BILINEAR
            || interpolation == VA_FILTER_INTERPOLATION_ADVANCED,
        "VPP interpolation method is not supported", VA_STATUS_ERROR_UNSUPPORTED_FILTER);

    // No colour conversion: input and output must name the same standard,
    // and when the client supplies explicit properties they must match.
    const int input_standard = static_cast<int>(p.surface_color_standard);
    const int output_standard = static_cast<int>(p.output_color_standard);
    iris::require(input_standard >= 0 && input_standard < VAProcColorStandardCount,
        "invalid input colour standard", VA_STATUS_ERROR_INVALID_PARAMETER);
    iris::require(output_standard >= 0 && output_standard < VAProcColorStandardCount,
        "invalid output colour standard", VA_STATUS_ERROR_INVALID_PARAMETER);
    iris::require(p.input_color_properties.color_range <= VA_SOURCE_RANGE_FULL,
        "invalid input colour range", VA_STATUS_ERROR_INVALID_PARAMETER);
    iris::require(p.output_color_properties.color_range <= VA_SOURCE_RANGE_FULL,
        "invalid output colour range", VA_STATUS_ERROR_INVALID_PARAMETER);
    // Whether the two ends may name different standards depends on whether a
    // tone-mapping filter is in the picture, and that buffer is not required to
    // arrive before this one; run_vpp judges it once both are in.
    const bool colour_properties_equal
        = std::memcmp(&p.input_color_properties, &p.output_color_properties, sizeof(VAProcColorProperties)) == 0;

    Context::VppJob job;
    job.source = p.surface;
    job.rotation = p.rotation_state;
    job.mirror = p.mirror_state;
    job.interpolation = interpolation == VA_FILTER_INTERPOLATION_BILINEAR      ? 1u
        : interpolation == VA_FILTER_INTERPOLATION_ADVANCED                    ? 4u
                                                                              : 0u;
    job.filters = p.num_filters;
    job.input_standard = input_standard;
    job.output_standard = output_standard;
    job.explicit_colour_properties_equal = colour_properties_equal;
    job.input_range_unknown = p.input_color_properties.color_range == VA_SOURCE_RANGE_UNKNOWN;
    job.output_range_unknown = p.output_color_properties.color_range == VA_SOURCE_RANGE_UNKNOWN;
    job.input_full_range = p.input_color_properties.color_range == VA_SOURCE_RANGE_FULL;
    job.output_full_range = p.output_color_properties.color_range == VA_SOURCE_RANGE_FULL;
    job.global_alpha = (blend.flags & VA_BLEND_GLOBAL_ALPHA) ? blend.global_alpha : 1.0f;
    job.premultiplied = (blend.flags & VA_BLEND_PREMULTIPLIED_ALPHA) != 0;
    if (p.surface_region) {
        job.region = *p.surface_region;
        job.region_set = true;
    }
    if (p.output_region) {
        job.output_region = *p.output_region;
        job.output_region_set = true;
    }
    if (blending)
        c.vpp_overlays.push_back(job);
    else
        c.vpp = job;
}

// One HDR10 tone-mapping filter buffer. The kernel's curve needs one number
// from it — the mastering display peak — and the rest of the metadata describes
// primaries this driver assumes (BT.2020/PQ) rather than reads, so a picture
// that names different primaries would be tone mapped with the wrong matrix.
// Refuse that instead of producing plausible colours.
void accept_vpp_filter(Context& c, const Buffer& b)
{
    iris::require(!c.vpp_peak_nits.has_value(), "one VideoProc filter per picture is supported",
        VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    iris::require(b.size >= sizeof(VAProcFilterParameterBufferHDRToneMapping),
        "filter parameters too small", VA_STATUS_ERROR_INVALID_PARAMETER);
    VAProcFilterParameterBufferHDRToneMapping f = {};
    std::memcpy(&f, b.data.data(), sizeof(f));
    iris::require(f.type == VAProcFilterHighDynamicRangeToneMapping, "unsupported VPP filter type",
        VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    iris::require(f.data.metadata_type == VAProcHighDynamicRangeMetadataHDR10,
        "only HDR10 tone mapping is supported", VA_STATUS_ERROR_UNSUPPORTED_FILTER);

    float peak = 1000.0f;
    if (f.data.metadata != nullptr && f.data.metadata_size >= sizeof(VAHdrMetaDataHDR10)) {
        VAHdrMetaDataHDR10 m = {};
        std::memcpy(&m, f.data.metadata, sizeof(m));
        if (m.max_display_mastering_luminance)
            peak = static_cast<float>(m.max_display_mastering_luminance) / 10000.0f;
    }
    iris::require(peak >= 100.0f && peak <= 10000.0f,
        "HDR mastering peak is outside the 100..10000 cd/m2 range", VA_STATUS_ERROR_INVALID_PARAMETER);
    c.vpp_peak_nits = peak;
}

// Run one VideoProc picture: the source comes from the pipeline buffer, the
// target is the surface vaBeginPicture named. Synchronous — scale_nv12_or_p010 waits
// for its fence, so a successful vaEndPicture means the pixels are there.
VAStatus run_vpp(DriverData& d, Context& c, VAContextID ctx_id, VASurfaceID dst_id,
    const std::shared_ptr<Surface>& target)
{
    iris::require(c.vpp.has_value(), "VideoProc picture has no pipeline parameters",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    const Context::VppJob job = *c.vpp;
    // The blending passes, bottom first: the composite runs them in this
    // order, so a later one covers an earlier one.
    const std::vector<Context::VppJob>& blends = c.vpp_overlays;
    const bool blended = !blends.empty();

    // One filter is supported, the HDR10 tone mapper, and its parameters
    // arrive as their own buffer in whichever order the client renders them.
    // The picture is judged only once both are in.
    const bool tone_map = job.filters == 1;
    iris::require(tone_map == c.vpp_peak_nits.has_value(),
        tone_map ? "the HDR10 tone mapper needs its parameter buffer"
                 : "VideoProc filter parameters without a filter",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    for (const Context::VppJob& blend : blends) {
        // Compositing is a copy of the main surface plus a blend into one
        // rectangle. Rotating or mirroring the main pass is the scale kernel's
        // job and a blended picture does not run it; the tone mapper is
        // allowed because it converts at the source's own size, so the blend
        // still reads a same-geometry main surface.
        iris::require(blend.filters == 0, "the blending pass carries no filter",
            VA_STATUS_ERROR_UNSUPPORTED_FILTER);
        iris::require(blend.rotation == VA_ROTATION_NONE, "the overlay does not rotate",
            VA_STATUS_ERROR_UNSUPPORTED_FILTER);
        iris::require(blend.mirror == VA_MIRROR_NONE, "the overlay does not mirror",
            VA_STATUS_ERROR_UNSUPPORTED_FILTER);
        // The overlay is mixed in the main pass's coded values, so naming a
        // different standard (or different explicit properties) would claim a
        // conversion the kernel does not do.
        iris::require(blend.input_standard == job.input_standard
                && blend.output_standard == job.output_standard && blend.explicit_colour_properties_equal
                && blend.input_full_range == job.input_full_range
                && blend.output_full_range == job.output_full_range,
            "the blending pass shares the colour standards of the pass it blends onto",
            VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    }
    if (blended) {
        iris::require(job.rotation == VA_ROTATION_NONE, "a blended picture does not rotate",
            VA_STATUS_ERROR_UNSUPPORTED_FILTER);
        iris::require(job.mirror == VA_MIRROR_NONE, "a blended picture does not mirror",
            VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    }
    if (!tone_map) {
        iris::require(job.input_standard == job.output_standard, "VPP colour conversion is not supported",
            VA_STATUS_ERROR_UNSUPPORTED_FILTER);
        if (job.input_standard == VAProcColorStandardExplicit)
            iris::require(job.explicit_colour_properties_equal, "VPP colour conversion is not supported",
                VA_STATUS_ERROR_UNSUPPORTED_FILTER);
        // The kernels move codes through unchanged, so a picture that names
        // one range at the input and the other at the output is claiming a
        // range conversion the driver does not do. Naming nothing is fine.
        iris::require(job.input_range_unknown || job.output_range_unknown
                || job.input_full_range == job.output_full_range,
            "VPP range conversion is not supported", VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    } else {
        // The kernel's matrices are fixed: BT.2020 primaries and the PQ
        // transfer in, BT.709 primaries and the BT.709 OETF out. A client
        // naming something else would get plausible colours from the wrong
        // conversion, so refuse it; naming nothing is fine, the metadata that
        // selected this filter already says HDR10.
        iris::require(job.input_standard == VAProcColorStandardNone
                || job.input_standard == VAProcColorStandardBT2020,
            "a tone-mapped source must be BT.2020 or unnamed", VA_STATUS_ERROR_UNSUPPORTED_FILTER);
        iris::require(job.output_standard == VAProcColorStandardNone
                || job.output_standard == VAProcColorStandardBT709,
            "a tone-mapped output must be BT.709 or unnamed", VA_STATUS_ERROR_UNSUPPORTED_FILTER);
    }

    Pixels src, dst;
    VARectangle crop = {};
    // One entry per blending pass: the surface it reads, the rectangle of the
    // output it lands in, and the rectangle of its own surface it samples.
    struct Layer {
        std::shared_ptr<Surface> surface;
        std::shared_ptr<iris::Session> session;
        Pixels pixels;
        VARectangle rect = {};
        VARectangle crop = {};
        // Either the output's Y'CbCr format or one of the packed RGB formats
        // the processor converts while it mixes.
        bool rgb = false;
    };
    std::vector<Layer> layers(blends.size());
    // Keep the sources alive across the dispatch: the fd captured below is
    // closed when its Surface dies, and a client that destroys a source
    // while this picture is running must not pull the buffer out from under
    // the import.
    std::shared_ptr<Surface> source;
    std::shared_ptr<iris::Session> source_session;
    {
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        source = surface(d, job.source);
        iris::require(source.get() != target.get(), "VPP source and output are the same surface",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        // The tone mapper is fixed: P010 in, NV12 out. Everything else is an
        // NV12 or P010 surface on each end, at either depth, because the scale
        // kernel converts bit depth with the same exact shift swscale uses.
        if (tone_map)
            iris::require(source->fourcc == VA_FOURCC_P010 && target->fourcc == VA_FOURCC_NV12,
                "tone mapping needs a P010 source and an NV12 output", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        else
            iris::require((source->fourcc == VA_FOURCC_NV12 || source->fourcc == VA_FOURCC_P010)
                    && (target->fourcc == VA_FOURCC_NV12 || target->fourcc == VA_FOURCC_P010),
                "VPP input and output must each be NV12 or P010", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        // A pending decode has neither a frame nor a stable buffer yet; its
        // pixels arrive at the wait below.
        iris::require(source->target.pending || source->target.frame || source->stable,
            "VPP source holds no pixels", VA_STATUS_ERROR_OPERATION_FAILED);

        // A surface region is the crop: the kernel samples inside that
        // rectangle and scales it to the whole output. Its origin and size
        // must be even, because a 4:2:0 chroma sample covers a 2x2 luma block.
        crop = { 0, 0, static_cast<uint16_t>(source->width), static_cast<uint16_t>(source->height) };
        if (job.region_set)
            crop = job.region;
        iris::require(crop.x >= 0 && crop.y >= 0 && crop.width > 0 && crop.height > 0
                && static_cast<uint32_t>(crop.x) + static_cast<uint32_t>(crop.width) <= source->width
                && static_cast<uint32_t>(crop.y) + static_cast<uint32_t>(crop.height) <= source->height,
            "VPP source region is outside the surface", VA_STATUS_ERROR_INVALID_PARAMETER);
        iris::require(!((crop.x | crop.y | crop.width | crop.height) & 1),
            "VPP source region must be even", VA_STATUS_ERROR_INVALID_PARAMETER);
        if (job.output_region_set)
            iris::require(job.output_region.x == 0 && job.output_region.y == 0
                    && job.output_region.width == target->width && job.output_region.height == target->height,
                "VPP fills the whole output surface", VA_STATUS_ERROR_INVALID_PARAMETER);
        // 4:2:0 subsampling makes both geometries even on either format. The
        // ratio itself is free: the kernel samples at swscale's point positions.
        iris::require(source->width && source->height && target->width && target->height
                && !((source->width | source->height | target->width | target->height) & 1u),
            "VPP needs an even, non-empty geometry", VA_STATUS_ERROR_INVALID_PARAMETER);
        // The tone mapper converts per pixel at the source's own size. A crop,
        // a scale, a rotation or a mirror in the same picture is the scale
        // kernel's job on the converted surface, so the picture runs it into a
        // scratch surface first (the pass chain below).
        if (blended) {
            // The main pass is copied at its own scale, so the rectangle the
            // client names is the output's size. FFmpeg names it on every
            // pass: the picture inside a decode surface that is wider than it.
            // A smaller rectangle would be a scale, which the composite does
            // not do, and an odd origin cannot land on a chroma cell.
            iris::require(static_cast<uint32_t>(crop.width) == target->width
                    && static_cast<uint32_t>(crop.height) == target->height,
                "a blended picture copies its main rectangle at its own scale",
                VA_STATUS_ERROR_UNSUPPORTED_FILTER);
        }
        for (size_t i = 0; i < blends.size(); ++i) {
            const Context::VppJob& blend = blends[i];
            Layer& layer = layers[i];
            iris::require(blend.output_region_set, "the blending pass needs an output region",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            layer.surface = surface(d, blend.source);
            iris::require(layer.surface.get() != target.get() && layer.surface.get() != source.get(),
                "the overlay is a third surface", VA_STATUS_ERROR_INVALID_PARAMETER);
            // Two layers may read the same surface — a client can stack one
            // image at two positions — but the output may not be one of them.
            const uint32_t ov_fourcc = layer.surface->fourcc;
            layer.rgb = ov_fourcc == VA_FOURCC_BGRA || ov_fourcc == VA_FOURCC_RGBA;
            iris::require(layer.rgb ? target->fourcc == VA_FOURCC_NV12 || target->fourcc == VA_FOURCC_P010
                                    : ov_fourcc == target->fourcc,
                "the overlay is the output's format or a packed RGBA/BGRA surface",
                VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
            iris::require(layer.surface->target.pending || layer.surface->target.frame || layer.surface->stable,
                "the overlay surface holds no pixels", VA_STATUS_ERROR_OPERATION_FAILED);
            layer.rect = blend.output_region;
            const VARectangle& rect = layer.rect;
            iris::require(rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0
                    && static_cast<uint32_t>(rect.x) + static_cast<uint32_t>(rect.width) <= target->width
                    && static_cast<uint32_t>(rect.y) + static_cast<uint32_t>(rect.height) <= target->height,
                "the overlay region is outside the output surface", VA_STATUS_ERROR_INVALID_PARAMETER);
            // An even rectangle puts every 2x2 chroma block wholly inside it
            // or wholly outside it; an odd one would need a per-pixel mix on
            // each chroma cell, which this kernel does not do.
            iris::require(!((rect.x | rect.y | rect.width | rect.height) & 1),
                "the overlay region must be even", VA_STATUS_ERROR_INVALID_PARAMETER);
            // A packed RGB overlay is sampled per pixel, so no subsampling
            // constraint reaches its own geometry; the rectangle still has
            // one, because the output's chroma cells do.
            iris::require(layer.surface->width && layer.surface->height
                    && (layer.rgb || !((layer.surface->width | layer.surface->height) & 1u)),
                "the overlay needs an even, non-empty geometry", VA_STATUS_ERROR_INVALID_PARAMETER);
            // The overlay's own region is what gets scaled into the output
            // rectangle. FFmpeg leaves it null, which means the whole surface;
            // a client that names one composites part of a surface. On a
            // Y'CbCr overlay it has to be even on both axes for the same
            // reason the output rectangle does.
            layer.crop = { 0, 0, static_cast<uint16_t>(layer.surface->width),
                static_cast<uint16_t>(layer.surface->height) };
            if (blend.region_set) {
                layer.crop = blend.region;
                const VARectangle& oc = layer.crop;
                iris::require(oc.x >= 0 && oc.y >= 0 && oc.width > 0 && oc.height > 0
                        && static_cast<uint32_t>(oc.x) + static_cast<uint32_t>(oc.width) <= layer.surface->width
                        && static_cast<uint32_t>(oc.y) + static_cast<uint32_t>(oc.height) <= layer.surface->height,
                    "the overlay region is outside the overlay surface", VA_STATUS_ERROR_INVALID_PARAMETER);
                iris::require(layer.rgb || !((oc.x | oc.y | oc.width | oc.height) & 1),
                    "the overlay region must be even", VA_STATUS_ERROR_INVALID_PARAMETER);
            }
        }

        // Captured under the lock; the waits themselves happen below, outside
        // it (architecture §7: never hold the mutex across a session wait).
        const auto source_owner = owner_of(d, *source);
        source_session = source_owner ? source_owner->session : nullptr;
        for (Layer& layer : layers) {
            const auto overlay_owner = owner_of(d, *layer.surface);
            layer.session = overlay_owner ? overlay_owner->session : nullptr;
        }
    }

    wait_for_pixels(source, source_session);
    for (Layer& layer : layers)
        wait_for_pixels(layer.surface, layer.session);
    {
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        src = pixels_of(d, *source);
        dst = pixels_of(d, *target);
        for (Layer& layer : layers)
            layer.pixels = pixels_of(d, *layer.surface);
    }

    // The kernels read and write the same words. Two surfaces over one buffer
    // pass every check above — different ids, different objects — and would
    // make the picture race itself.
    iris::require(!same_buffer(src, dst), "VPP output aliases its input", VA_STATUS_ERROR_INVALID_SURFACE);
    for (const Layer& layer : layers)
        iris::require(!same_buffer(layer.pixels, dst), "the overlay aliases the output",
            VA_STATUS_ERROR_INVALID_SURFACE);

    // The tone mapper converts the whole source at its own size; the scale
    // and the blend move pixels afterwards. A picture that asks for both runs
    // the conversion into a scratch surface and the next pass reads that, so
    // the passes stay the same kernels and only their inputs change.
    const bool geometry_changes = crop.x != 0 || crop.y != 0
        || static_cast<uint32_t>(crop.width) != target->width
        || static_cast<uint32_t>(crop.height) != target->height || job.rotation != VA_ROTATION_NONE
        || job.mirror != VA_MIRROR_NONE;
    const bool tonemap_to_scratch = tone_map && (blended || geometry_changes);
    std::unique_ptr<iris::StableBuffer> scratch;
    Pixels mid;
    if (tonemap_to_scratch) {
        scratch = iris::allocate_stable(d.render_fd,
            iris::stable_layout(VA_FOURCC_NV12, source->width, source->height));
        mid.fd = scratch->fd();
        mid.size = scratch->size();
        mid.layout = scratch->layout();
    }
    // Whichever buffer holds the pixels the next pass reads: the source
    // surface, or the tone mapper's scratch output.
    const Pixels& base = tonemap_to_scratch ? mid : src;

    bool ran = true;
    if (tone_map) {
        iris::vpp::ToneMapJob tj;
        const Pixels& out = tonemap_to_scratch ? mid : dst;
        tj.src_fd = src.fd;
        tj.dst_fd = out.fd;
        tj.src_size = src.size;
        tj.dst_size = out.size;
        tj.src_stride = src.layout.stride;
        tj.dst_stride = out.layout.stride;
        tj.src_uv = src.layout.chroma_offset();
        tj.dst_uv = out.layout.chroma_offset();
        tj.src_y = src.layout.data_offset;
        tj.dst_y = out.layout.data_offset;
        tj.width = tonemap_to_scratch ? source->width : target->width;
        tj.height = tonemap_to_scratch ? source->height : target->height;
        tj.source_peak_nits = *c.vpp_peak_nits;
        ran = iris::vpp::tonemap_hdr10_to_sdr(tj);
    }
    if (ran && blended) {
        // The main pass is copied into the output first — the composite is an
        // offset copy plus a mix per layer, not a scale — and then every layer
        // is mixed onto the pixels before it. Reading the output back between
        // dispatches is what lets one kernel serve any number of layers: layer
        // i covers layers 0..i-1 exactly as it would cover the main surface,
        // because the kernel only ever sees one "main" buffer.
        //
        // The copy is the scale kernel at 1:1, the tone mapper already having
        // put its result in `base`. That is the same path a picture with no
        // blend takes when it only crops, so there is one copy implementation
        // and not two.
        {
            iris::vpp::ScaleJob sj;
            sj.src_fd = base.fd;
            sj.dst_fd = dst.fd;
            sj.src_size = base.size;
            sj.dst_size = dst.size;
            sj.src_stride = base.layout.stride;
            sj.dst_stride = dst.layout.stride;
            sj.src_uv = base.layout.chroma_offset();
            sj.dst_uv = dst.layout.chroma_offset();
            sj.src_y = base.layout.data_offset;
            sj.dst_y = dst.layout.data_offset;
            sj.src_x = static_cast<uint32_t>(crop.x);
            sj.src_y_offset = static_cast<uint32_t>(crop.y);
            sj.src_width = static_cast<uint32_t>(crop.width);
            sj.src_height = static_cast<uint32_t>(crop.height);
            sj.width = target->width;
            sj.height = target->height;
            sj.src_bytes = base.layout.fourcc == VA_FOURCC_P010 ? 2 : 1;
            sj.dst_bytes = dst.layout.fourcc == VA_FOURCC_P010 ? 2 : 1;
            // A blended picture copies its main rectangle at its own scale, so
            // the point map is the identity here; naming it keeps the copy
            // independent of the client's interpolation choice.
            sj.interpolation = 0;
            ran = iris::vpp::scale_nv12_or_p010(sj);
        }
        for (size_t i = 0; ran && i < layers.size(); ++i) {
            const Context::VppJob& blend = blends[i];
            const Layer& layer = layers[i];
            const VARectangle& rect = layer.rect;
            const VARectangle& ov_crop = layer.crop;
            const Pixels& ov = layer.pixels;
            if (layer.rgb) {
                iris::vpp::OverlayRgbJob oj;
                oj.main_fd = dst.fd;
                oj.overlay_fd = ov.fd;
                oj.dst_fd = dst.fd;
                oj.main_size = dst.size;
                oj.overlay_size = ov.size;
                oj.dst_size = dst.size;
                oj.main_stride = dst.layout.stride;
                oj.overlay_stride = ov.layout.stride;
                oj.dst_stride = dst.layout.stride;
                oj.main_uv = dst.layout.chroma_offset();
                oj.dst_uv = dst.layout.chroma_offset();
                oj.main_y = dst.layout.data_offset;
                oj.main_x = 0;
                oj.main_row = 0;
                oj.overlay_y = ov.layout.data_offset;
                oj.dst_y = dst.layout.data_offset;
                oj.overlay_width = layer.surface->width;
                oj.overlay_height = layer.surface->height;
                oj.overlay_x = static_cast<uint32_t>(ov_crop.x);
                oj.overlay_row = static_cast<uint32_t>(ov_crop.y);
                oj.overlay_crop_width = ov_crop.width;
                oj.overlay_crop_height = ov_crop.height;
                oj.x = static_cast<uint32_t>(rect.x);
                oj.y = static_cast<uint32_t>(rect.y);
                oj.width = rect.width;
                oj.height = rect.height;
                oj.dst_width = target->width;
                oj.dst_height = target->height;
                oj.bytes = dst.layout.fourcc == VA_FOURCC_P010 ? 2 : 1;
                oj.bgra = layer.surface->fourcc == VA_FOURCC_BGRA;
                oj.premultiplied = blend.premultiplied;
                // The matrix is the output frame's. A tone-mapped picture has
                // already converted to BT.709 and names that or nothing, so
                // this is the same standard the main surface carries in every
                // supported picture.
                if (job.output_standard == VAProcColorStandardBT601)
                    oj.standard = 1;
                else if (job.output_standard == VAProcColorStandardBT2020)
                    oj.standard = 2;
                else
                    oj.standard = 0;
                // The overlay is sampled with the method the pass that blends
                // it names, over that pass's own region rather than the whole
                // source.
                oj.interpolation = blend.interpolation;
                // The composite encodes its result, so it has to know which
                // range the client means; the scale carries codes through and
                // does not.
                oj.full_range = job.output_full_range;
                oj.alpha = blend.global_alpha;
                ran = iris::vpp::overlay_rgb(oj);
            } else {
                iris::vpp::OverlayJob oj;
                oj.main_fd = dst.fd;
                oj.overlay_fd = ov.fd;
                oj.dst_fd = dst.fd;
                oj.main_size = dst.size;
                oj.overlay_size = ov.size;
                oj.dst_size = dst.size;
                oj.main_stride = dst.layout.stride;
                oj.overlay_stride = ov.layout.stride;
                oj.dst_stride = dst.layout.stride;
                oj.main_uv = dst.layout.chroma_offset();
                oj.overlay_uv = ov.layout.chroma_offset();
                oj.dst_uv = dst.layout.chroma_offset();
                oj.main_y = dst.layout.data_offset;
                oj.main_x = 0;
                oj.main_row = 0;
                // The main term is the output itself, so this is an identity
                // copy of the whole surface: the kernel reads and writes the
                // same word, and the layer rectangle is what changes. The
                // kernel's own rule — the main rectangle is the destination's
                // size — is what the whole-surface geometry satisfies.
                oj.main_width = target->width;
                oj.main_height = target->height;
                oj.overlay_y = ov.layout.data_offset;
                oj.dst_y = dst.layout.data_offset;
                oj.overlay_width = layer.surface->width;
                oj.overlay_height = layer.surface->height;
                oj.overlay_x = static_cast<uint32_t>(ov_crop.x);
                oj.overlay_row = static_cast<uint32_t>(ov_crop.y);
                oj.overlay_crop_width = ov_crop.width;
                oj.overlay_crop_height = ov_crop.height;
                oj.x = static_cast<uint32_t>(rect.x);
                oj.y = static_cast<uint32_t>(rect.y);
                oj.width = rect.width;
                oj.height = rect.height;
                oj.dst_width = target->width;
                oj.dst_height = target->height;
                // Both sides of the mix are the same format: the output holds
                // the pixels the previous layer left, and this layer was
                // checked against that format.
                oj.bytes = dst.layout.fourcc == VA_FOURCC_P010 ? 2 : 1;
                oj.premultiplied = blend.premultiplied;
                oj.interpolation = blend.interpolation;
                oj.full_range = job.output_full_range;
                oj.alpha = blend.global_alpha;
                ran = iris::vpp::overlay(oj);
            }
        }
    } else if (ran && (!tone_map || tonemap_to_scratch)) {
        iris::vpp::ScaleJob sj;
        sj.src_fd = base.fd;
        sj.dst_fd = dst.fd;
        sj.src_size = base.size;
        sj.dst_size = dst.size;
        sj.src_stride = base.layout.stride;
        sj.dst_stride = dst.layout.stride;
        sj.src_uv = base.layout.chroma_offset();
        sj.dst_uv = dst.layout.chroma_offset();
        sj.src_y = base.layout.data_offset;
        sj.dst_y = dst.layout.data_offset;
        sj.src_x = static_cast<uint32_t>(crop.x);
        sj.src_y_offset = static_cast<uint32_t>(crop.y);
        sj.src_width = static_cast<uint32_t>(crop.width);
        sj.src_height = static_cast<uint32_t>(crop.height);
        sj.width = target->width;
        sj.height = target->height;
        // `base` is the surface the pass reads: the source, or the tone
        // mapper's NV12 scratch output. The two ends are named separately, so
        // a picture that changes bit depth scales and converts in one pass.
        sj.src_bytes = base.layout.fourcc == VA_FOURCC_P010 ? 2 : 1;
        sj.dst_bytes = dst.layout.fourcc == VA_FOURCC_P010 ? 2 : 1;
        // The rotation happens before the scale, so a quarter turn swaps the
        // sampled rectangle's dimensions and the point map runs on that.
        sj.rotation = job.rotation;
        // Mirroring is the step after the rotation and before the scale.
        sj.mirror = job.mirror;
        sj.interpolation = job.interpolation;
        ran = iris::vpp::scale_nv12_or_p010(sj);
    }
    if (!ran) {
        target->failed = true;
        throw iris::Error(VA_STATUS_ERROR_OPERATION_FAILED,
            blended ? "Vulkan VPP blending failed"
                    : tone_map ? "Vulkan VPP tone mapping failed" : "Vulkan VPP scale failed");
    }
    target->vpp_pixels = true;
    target->failed = false;
    // The trailing field is the trace vocabulary docs/architecture.md §8
    // defines: which operation this picture ran beyond the scale. A stacked
    // picture names its bottom layer, which is the one a single-layer client
    // names too; the count is what tells the two apart.
    char extra[160] = "";
    if (!layers.empty()) {
        const Context::VppJob& bottom = blends[0];
        const Layer& first = layers[0];
        const VARectangle& rect = first.rect;
        std::snprintf(extra, sizeof(extra), " overlay=%u %ux%u+%d,%d alpha=%.2f%s%s", bottom.source,
            rect.width, rect.height, rect.x, rect.y, bottom.global_alpha,
            bottom.premultiplied ? " premultiplied" : "",
            first.rgb ? (first.surface->fourcc == VA_FOURCC_BGRA ? " source=bgra" : " source=rgba") : "");
        if (layers.size() > 1) {
            const size_t used = std::strlen(extra);
            std::snprintf(extra + used, sizeof(extra) - used, " layers=%zu", layers.size());
        }
    } else if (tone_map)
        std::snprintf(extra, sizeof(extra), " tonemap=hdr10");
    d.trace("va end_picture ctx=%u surface=%u entrypoint=videoproc src=%u %ux%u->%ux%u%s", ctx_id, dst_id,
        job.source, src.layout.width, src.layout.height, target->width, target->height, extra);
    return VA_STATUS_SUCCESS;
}

} // namespace

VAStatus createContext(VADriverContextP ctx, VAConfigID config, int width, int height, int flags,
    VASurfaceID* render_targets, int count, VAContextID* id)
{
    return guarded(ctx, "createContext", [&] {
        auto& d = data(ctx);
        (void)flags;
        iris::require(width > 0 && height > 0, "invalid context geometry", VA_STATUS_ERROR_INVALID_PARAMETER);
        Config cfg;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            auto it = d.configs.find(config);
            iris::require(it != d.configs.end(), "invalid config", VA_STATUS_ERROR_INVALID_CONFIG);
            cfg = it->second;
            // The copy engine is built on the first copy, not here: GPU copy
            // initialises EGL, worth about 0.1 s per process, and Direct and
            // Import clients never copy. A VideoProc context never copies at
            // all — its pixels arrive from the compute kernel.
            if (!d.copier && cfg.entrypoint != VAEntrypointVideoProc)
                d.copier = iris::make_deferred_copy(d.render_fd, d.options.copy_mode);
        }
        auto c = std::make_shared<Context>();
        c->profile = cfg.profile;
        c->entrypoint = cfg.entrypoint;
        c->width = static_cast<unsigned>(width);
        c->height = static_cast<unsigned>(height);

        if (cfg.entrypoint == VAEntrypointEncSlice) {
            const iris::RateMode rate_mode = (cfg.rate_control & VA_RC_CQP)  ? iris::RateMode::Cqp
                : (cfg.rate_control & VA_RC_CBR)                             ? iris::RateMode::Cbr
                                                                             : iris::RateMode::Vbr;
            c->encoder = make_encoder(cfg.profile, rate_mode);
            iris::EncodeConfig ec;
            ec.codec = c->encoder->v4l2_pixelformat();
            ec.width = c->width;
            ec.height = c->height;
            // Open the node here so a missing encoder surfaces at
            // vaCreateContext rather than at the first vaEndPicture.
            c->encode_session = std::make_shared<iris::EncoderSession>(
                iris::open_device(d.options.encoder_path, iris::NodeRole::Encoder), ec, d.options, d.trace);
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            *id = d.next_id++;
            d.contexts[*id] = c;
            d.trace("va create_context id=%u profile=%d %ux%u entrypoint=encslice codec=%.4s", *id, cfg.profile,
                c->width, c->height, reinterpret_cast<const char*>(&ec.codec));
            return VA_STATUS_SUCCESS;
        }

        if (cfg.entrypoint == VAEntrypointVideoProc) {
            // No node, no translator, no session: vaEndPicture runs the
            // compute kernel. The render targets carry no meaning here, so
            // the context keeps only the geometry the client declared.
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            *id = d.next_id++;
            d.contexts[*id] = c;
            d.trace("va create_context id=%u profile=-1 %ux%u entrypoint=videoproc", *id, c->width, c->height);
            return VA_STATUS_SUCCESS;
        }

        c->translator = make_translator(cfg.profile);

        iris::SessionConfig sc;
        sc.codec_pixelformat = c->translator->v4l2_pixelformat();
        sc.capture_fourcc = (cfg.rt_format & VA_RT_FORMAT_YUV420_10) ? VA_FOURCC_P010 : VA_FOURCC_NV12;
        sc.width = c->width;
        sc.height = c->height;
        sc.surface_count = static_cast<unsigned>(std::max(count, 0));
        sc.import_allowed = d.capabilities.import_formats.count(sc.codec_pixelformat) != 0;
        // Opening the node and OUTPUT STREAMON happen here, outside the
        // table lock. SOURCE_CHANGE and CAPTURE follow the first AU.
        c->session = std::make_shared<iris::Session>(iris::open_device(d.options.video_path), sc, d.options,
            d.copier.get(), d.trace);

        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        for (int i = 0; i < count; ++i) {
            auto it = d.surfaces.find(render_targets[i]);
            if (it == d.surfaces.end())
                continue;
            c->render_targets.push_back(render_targets[i]);
            if (it->second->owner == VA_INVALID_ID)
                it->second->owner = d.next_id; // assigned below
        }
        *id = d.next_id++;
        d.contexts[*id] = c;
        d.trace("va create_context id=%u profile=%d %ux%u targets=%d mode=%s", *id, cfg.profile, c->width,
            c->height, count, c->session->mode() == iris::SessionMode::DecodeOrder ? "decode-order" : "display-order");
        return VA_STATUS_SUCCESS;
    });
}

VAStatus destroyContext(VADriverContextP ctx, VAContextID id)
{
    return guarded(ctx, "destroyContext", [&] {
        auto& d = data(ctx);
        std::shared_ptr<Context> c;
        std::vector<std::shared_ptr<Surface>> owned;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            c = context(d, id);
            // The context stays in the table until its session has drained:
            // a vaSyncSurface arriving from another thread (FFmpeg's filter
            // thread downloads while the decoder thread tears down) must
            // still find the session and wait on it.
            for (auto& [sid, s] : d.surfaces)
                if (s->owner == id)
                    owned.push_back(s);
        }
        if (c->encoding()) {
            d.trace("va destroy_context id=%u entrypoint=encslice frames=%llu", id,
                static_cast<unsigned long long>(c->enc_frames));
            // finish() drains the encoder and tears the queues down; it is
            // safe when no picture was ever submitted.
            try {
                c->encode_session->finish();
            } catch (const std::exception& error) {
                d.trace("va destroy_context encode finish failed: %s", error.what());
            }
            {
                std::lock_guard<std::recursive_mutex> lock(d.mutex);
                d.contexts.erase(id);
            }
            for (auto& s : owned)
                if (s->owner == id)
                    s->owner = VA_INVALID_ID;
            c->encode_session.reset();
            c->encoder.reset();
            return VA_STATUS_SUCCESS;
        }
        if (c->video_proc()) {
            // Nothing to drain: every VPP picture completed before its
            // vaEndPicture returned.
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            d.contexts.erase(id);
            for (auto& s : owned)
                if (s->owner == id)
                    s->owner = VA_INVALID_ID;
            d.trace("va destroy_context id=%u entrypoint=videoproc", id);
            return VA_STATUS_SUCCESS;
        }
        unsigned pending = 0;
        for (auto& s : owned)
            pending += s->rendering() ? 1 : 0;
        d.trace("va destroy_context id=%u pending=%u", id, pending);
        // finish() drains so every pending target completes or fails, then
        // every target lets go of its frame so REQBUFS(0) can run when the
        // session is destroyed.
        c->session->finish();
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            d.contexts.erase(id);
        }
        for (auto& s : owned) {
            if (s->rendering())
                c->session->release(s->target);
            // A completed Direct frame stays with its surface: the Frame
            // owns an exported fd, and vb2 orphans exported buffers when
            // the queue is freed, so the pixels remain readable after the
            // session closes. FFmpeg downloads the last pictures of a
            // resolution segment after destroying that segment's context.
            // The frame is simply the last reference to its slot now;
            // nothing can requeue it, and it is dropped when the surface is
            // reused or destroyed.
            s->owner = VA_INVALID_ID;
        }
        c->session.reset();
        return VA_STATUS_SUCCESS;
    });
}

VAStatus beginPicture(VADriverContextP ctx, VAContextID id, VASurfaceID surface_id)
{
    return guarded(ctx, "beginPicture", [&] {
        auto& d = data(ctx);
        std::shared_ptr<Context> c;
        std::shared_ptr<Surface> s;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            c = context(d, id);
            s = surface(d, surface_id);
            iris::require(c->current == VA_INVALID_ID, "picture already in progress", VA_STATUS_ERROR_OPERATION_FAILED);
            iris::require(s->fourcc == VA_FOURCC_NV12 || s->fourcc == VA_FOURCC_P010, "surface format",
                VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
            // An encode-only refusal is checked before the context takes
            // ownership of the surface. Setting c->current first would leave
            // it set after a rejected call, and every later vaBeginPicture on
            // this context would fail with "picture already in progress".
            if (c->encoding()) {
                iris::require(s->fourcc == VA_FOURCC_NV12, "encode input must be NV12",
                    VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
                // The picture the client asked for is checked against the
                // surface in vaEndPicture, where the sequence parameters that
                // narrow a macroblock-aligned context have arrived. Either
                // side may be the larger one: FFmpeg creates an aligned
                // context and uploads exact surfaces, and its transcode
                // creates an exact context over aligned decode surfaces.
            }
            // The processor scales either sample width; run_vpp checks that the
            // source and this target agree on it.
            // A surface may move between contexts (FFmpeg's pool); it just
            // becomes the new context's target. A pending picture on another
            // context is released there first.
            if (s->owner != VA_INVALID_ID && s->owner != id) {
                if (auto previous = owner_of(d, *s); previous && previous->session) {
                    if (c->encoding()) {
                        // A decoded surface handed straight to the encoder is
                        // a dependency, not a reuse: FFmpeg's transcode does
                        // not call vaSyncSurface between the two. Keep the
                        // picture and let endPicture wait for it; releasing
                        // here dropped the completion and encoded an unpainted
                        // buffer.
                        s->pixels_session = previous->session;
                    } else {
                        previous->session->release(s->target);
                    }
                }
            }
            s->owner = id;
            c->current = surface_id;
            // Whatever entrypoint starts the picture, the pixels the previous
            // one left on this surface are gone. A decode reused as a VPP
            // output (or the reverse) must not read the old frame back.
            s->vpp_pixels = false;
            // A grain job belongs to the picture that queued it on this
            // surface as its display target; the new picture replaces those
            // pixels, so the job must not run on them.
            s->grain.reset();
            s->grain_pixels = false;
            if (c->encoding()) {
                // Per-picture state; the sequence parameters survive, since
                // a client may render them with the IDR only.
                c->enc_picture.picture = nullptr;
                c->enc_picture.misc.clear();
                c->enc_picture.slices.clear();
                c->enc_picture.surface_width = s->width;
                c->enc_picture.surface_height = s->height;
            } else if (c->video_proc()) {
                // A new picture replaces whatever this surface held.
                c->vpp.reset();
                c->vpp_overlays.clear();
                c->vpp_peak_nits.reset();
                s->failed = false;
            } else {
                c->picture = {};
                c->picture.surface_width = s->width;
                c->picture.surface_height = s->height;
                c->picture.surface_id = surface_id;
            }
        }
        if (c->video_proc()) {
            d.trace("va begin_picture ctx=%u surface=%u entrypoint=videoproc", id, surface_id);
            return VA_STATUS_SUCCESS;
        }
        if (c->encoding()) {
            d.trace("va begin_picture ctx=%u surface=%u entrypoint=encslice", id, surface_id);
            return VA_STATUS_SUCCESS;
        }
        // Reusing the surface: whatever it held is released (the slot goes
        // back to the firmware; a still-pending token is invalidated).
        s->pixels_session.reset();
        c->session->release(s->target);
        s->failed = false;
        if (s->target.frame && d.copier)
            d.copier->forget(s->target.frame->id());
        s->target.frame.reset();
        d.trace("va begin_picture ctx=%u surface=%u", id, surface_id);
        return VA_STATUS_SUCCESS;
    });
}

VAStatus renderPicture(VADriverContextP ctx, VAContextID id, VABufferID* buffers, int count)
{
    return guarded(ctx, "renderPicture", [&] {
        auto& d = data(ctx);
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        auto c = context(d, id);
        iris::require(c->current != VA_INVALID_ID, "renderPicture outside a picture", VA_STATUS_ERROR_OPERATION_FAILED);
        if (c->encoding()) {
            for (int i = 0; i < count; ++i) {
                auto b = buffer(d, buffers[i]);
                switch (b->type) {
                case VAEncSequenceParameterBufferType:
                    c->enc_sequence = b;
                    c->enc_picture.sequence = b->data.data();
                    break;
                case VAEncPictureParameterBufferType:
                    c->enc_picture.picture = b->data.data();
                    break;
                case VAEncMiscParameterBufferType:
                    for (unsigned k = 0; k < b->count; ++k)
                        c->enc_picture.misc.push_back(b->data.data() + static_cast<size_t>(k) * b->size);
                    break;
                case VAEncSliceParameterBufferType:
                    // The firmware decides the slice layout; the client's
                    // slice parameters only describe what it expects. CQP
                    // reads the first slice's QP delta.
                    for (unsigned k = 0; k < b->count; ++k)
                        c->enc_picture.slices.push_back(b->data.data() + static_cast<size_t>(k) * b->size);
                    break;
                case VAEncPackedHeaderParameterBufferType:
                case VAEncPackedHeaderDataBufferType:
                    // Packed headers are not advertised. A client that
                    // sends them anyway must not lose the picture.
                    break;
                default:
                    throw iris::Error(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE,
                        "buffer type not accepted by an encode renderPicture");
                }
            }
            return VA_STATUS_SUCCESS;
        }
        if (c->video_proc()) {
            for (int i = 0; i < count; ++i) {
                auto b = buffer(d, buffers[i]);
                switch (b->type) {
                case VAProcPipelineParameterBufferType:
                    accept_vpp_pipeline(*c, *b);
                    break;
                case VAProcFilterParameterBufferType:
                    // Arrives before or after the pipeline buffer; both are
                    // judged together in run_vpp.
                    accept_vpp_filter(*c, *b);
                    break;
                default:
                    throw iris::Error(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE,
                        "buffer type not accepted by a VideoProc renderPicture");
                }
            }
            return VA_STATUS_SUCCESS;
        }
        for (int i = 0; i < count; ++i) {
            auto b = buffer(d, buffers[i]);
            switch (b->type) {
            case VAPictureParameterBufferType:
                c->picture.parameters = b->data.data();
                break;
            case VAIQMatrixBufferType:
                c->picture.iq_matrix = b->data.data();
                break;
            case VASliceParameterBufferType:
                for (unsigned k = 0; k < b->count; ++k)
                    c->picture.slice_parameters.push_back(b->data.data() + static_cast<size_t>(k) * b->size);
                break;
            case VASliceDataBufferType:
                c->picture.slice_data.emplace_back(b->data.data(), static_cast<size_t>(b->size) * b->count);
                break;
            default:
                throw iris::Error(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE, "buffer type not accepted by renderPicture");
            }
        }
        return VA_STATUS_SUCCESS;
    });
}

VAStatus endPicture(VADriverContextP ctx, VAContextID id)
{
    return guarded(ctx, "endPicture", [&] {
        auto& d = data(ctx);
        std::shared_ptr<Context> c;
        std::shared_ptr<Surface> s;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            c = context(d, id);
            iris::require(c->current != VA_INVALID_ID, "endPicture outside a picture", VA_STATUS_ERROR_OPERATION_FAILED);
            s = surface(d, c->current);
        }
        const VASurfaceID current = c->current;
        c->current = VA_INVALID_ID;
        if (c->encoding()) {
            // The client named the target buffer in its picture parameters;
            // nothing about the context decides it.
            iris::require(c->enc_picture.sequence != nullptr, "encode picture has no sequence parameters",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            iris::require(c->enc_picture.picture != nullptr, "encode picture has no picture parameters",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            const VABufferID coded_id = c->encoder->coded_buf(c->enc_picture);
            std::shared_ptr<Buffer> coded;
            {
                std::lock_guard<std::recursive_mutex> lock(d.mutex);
                iris::require(coded_id != VA_INVALID_ID, "encode picture names no coded buffer",
                    VA_STATUS_ERROR_INVALID_PARAMETER);
                coded = buffer(d, coded_id);
                iris::require(coded->type == VAEncCodedBufferType, "coded_buf is not a coded buffer",
                    VA_STATUS_ERROR_INVALID_PARAMETER);
            }
            // The input is whichever DMA-BUF holds this surface's pixels: the
            // decoded CAPTURE frame when the surface was published Direct, or
            // the stable buffer otherwise. Taking the stable buffer
            // unconditionally encoded an unpainted allocation for a surface
            // that had just been decoded Direct. The decode may still be in
            // flight, and a Direct frame carries no fence the encoder can
            // wait on, so wait here as the VideoProc path does.
            auto source_session = s->pixels_session ? s->pixels_session : session_of(d, *s);
            wait_for_pixels(s, source_session);
            Pixels pixels;
            {
                std::lock_guard<std::recursive_mutex> lock(d.mutex);
                pixels = pixels_of(d, *s);
                s->pixels_session.reset();
            }
            // Code the picture the client asked for, not the surface's
            // aligned allocation. The encode context's geometry is the
            // picture size the client declared, and H.264's context may be
            // macroblock-aligned (FFmpeg creates a 1920x1088 H.264 context for
            // a 1920x1080 picture), where the sequence's frame crop names the
            // picture instead. Coding the allocation hands the client a
            // stream whose geometry no longer matches the one it decoded. The
            // buffer's own layout still has to be the one the encoder will
            // read; when narrowing would move the chroma plane, the surface
            // geometry stays.
            unsigned coded_width = c->width;
            unsigned coded_height = c->height;
            unsigned sequence_width = 0;
            unsigned sequence_height = 0;
            if (c->encoder->coded_size(c->enc_picture, sequence_width, sequence_height)
                && sequence_width && sequence_height && sequence_width <= coded_width
                && sequence_height <= coded_height) {
                coded_width = sequence_width;
                coded_height = sequence_height;
            }
            iris::require(coded_width <= s->width && coded_height <= s->height,
                "encode surface is smaller than the picture", VA_STATUS_ERROR_INVALID_SURFACE);
            const iris::Layout expected = iris::stable_layout(s->fourcc, coded_width, coded_height);
            if (expected.fourcc != pixels.layout.fourcc || expected.stride != pixels.layout.stride
                || expected.storage_height != pixels.layout.storage_height) {
                coded_width = s->width;
                coded_height = s->height;
            }
            c->enc_picture.surface_width = coded_width;
            c->enc_picture.surface_height = coded_height;
            // Translation and the V4L2 queue run outside the table lock: the
            // submit blocks only when every input slot is still with the
            // firmware, and the access unit itself is waited for later.
            const iris::EncodeParams params = c->encoder->translate(c->enc_picture);
            if (!c->encode_session->configured()) {
                // The codec knows which units travel with its keyframes; the
                // session decides when one needs them.
                iris::codec::Encoder* encoder = c->encoder.get();
                c->encode_session->configure(params, coded_width, coded_height,
                    [encoder](const std::vector<uint8_t>& unit, std::vector<iris::codec::NalUnit>& last) {
                        return encoder->prepend_parameter_sets(unit, last);
                    });
            } else {
                // The encoder has no SOURCE_CHANGE: one geometry per context.
                iris::require(c->encode_session->config().width == coded_width
                        && c->encode_session->config().height == coded_height,
                    "encode surface changed size", VA_STATUS_ERROR_INVALID_SURFACE);
            }
            c->enc_frames++;
            // The surface's DMA-BUF has to outlive the submit: the firmware
            // reads it until it completes the picture. `s` holds it until the
            // surface does.
            const iris::EncodeTicket ticket
                = c->encode_session->submit(pixels.fd, pixels.size, params);
            {
                std::lock_guard<std::recursive_mutex> lock(d.mutex);
                coded->payload.clear();
                coded->segment.size = 0;
                coded->segment.bit_offset = 0;
                coded->segment.status = 0;
                coded->segment.buf = nullptr;
                coded->segment.next = nullptr;
                coded->ticket = ticket;
                coded->coded_session = c->encode_session;
                c->enc_submitted[current] = Context::EncSubmitted { ticket, c->encode_session };
            }
            d.trace("va end_picture ctx=%u surface=%u entrypoint=encslice submitted token=%llu", id, current,
                static_cast<unsigned long long>(ticket.timestamp_us));
            return VA_STATUS_SUCCESS;
        }
        if (c->video_proc())
            return run_vpp(d, *c, id, current, s);
        // The VA buffers referenced by c->picture stay valid until the
        // client destroys them, which VA forbids before vaEndPicture returns.
        try {
            std::lock_guard<std::mutex> translate(c->translate_mutex);
            c->translator->translate(c->picture, c->au);
        } catch (const iris::Error&) {
            s->failed = true;
            throw;
        }
        d.trace("va end_picture ctx=%u surface=%u bytes=%zu seq_start=%d", id, current, c->au.bytes.size(),
            c->au.sequence_start ? 1 : 0);
        // The display surface has to know a blend is coming before the
        // picture does, and the blend itself runs here: the client recycles
        // current_frame at its next submit, so the pixels are gone by then.
        const auto grain = queue_grain(d, *c, s);
        try {
            submit(d, *c, *s, c->au);
        } catch (...) {
            if (grain)
                abandon_grain(d, grain);
            throw;
        }
        if (grain)
            finish_grain(d, grain, c->session, s->target);
        return VA_STATUS_SUCCESS;
    });
}

} // namespace va
