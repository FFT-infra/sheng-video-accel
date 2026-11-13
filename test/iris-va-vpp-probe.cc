// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): one VideoProc picture, driven the way a
// client drives it.
//
//   iris-va-vpp-probe <render-node> <src.raw> <srcW> <srcH>
//                     <cropW> <cropH> <cropX> <cropY> <dstW> <dstH> <out.raw>
//                     [--expect-refusal]
//
// It uploads the NV12 frame through a derived image, creates a
// VAProfileNone / VAEntrypointVideoProc context, submits a
// VAProcPipelineParameterBuffer whose surface_region is the crop, ends the
// picture and downloads the result with vaGetImage. FFmpeg 8.1 has no
// crop_vaapi filter, so this is the only client that can ask for a region.
//
//   iris-va-vpp-probe tonemap <render-node> <W> <H> <peak-nits> <out.raw>
//                     [--source-nv12] [--no-filter] [--rotate]
//                     [--expect-refusal]
//
// The second mode submits VAProcFilterHighDynamicRangeToneMapping with HDR10
// metadata naming `peak-nits` as the mastering display peak, converts a
// synthetic HDR10 P010 surface to SDR NV12, and compares every output byte
// with the probe's own implementation of the pipeline documented in
// test/vulkan-vpp/tonemap.comp. FFmpeg's tonemap_vaapi refuses a raw P010
// frame (it demands mastering-display side data), so this is the only client
// that can drive the filter at all.
//
//   iris-va-vpp-probe overlay <render-node> <W> <H> <ovW> <ovH> <x> <y>
//                     <rw> <rh> <alpha> <out.raw>
//                     [--p010] [--premultiplied] [--luma-key] [--third]
//                     [--blend-first] [--main-crop] [--expect-refusal]
//
// The third mode submits two pipeline buffers in one picture, the way
// overlay_vaapi does: the main surface over the whole output, then the
// overlay surface into one rectangle with a VABlendState naming a global
// alpha. It compares the result with the mix documented in
// test/vulkan-vpp/overlay.comp, and checks that the caps advertise
// VA_BLEND_GLOBAL_ALPHA, which is what overlay_vaapi refuses to run without.
//
// `--expect-refusal` inverts the verdict: the picture must fail (an odd or
// out-of-bounds region, an NV12 source, a filter without its parameters, a
// rotation, premultiplied alpha, a third pass, all are the driver's to refuse
// rather than to round).
//
// Needs V4L2_VA_VPP=1, LIBVA_DRIVER_NAME=v4l2 and either the installed driver
// or LIBVA_DRIVERS_PATH pointing at a build tree.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_vpp.h>
}

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

bool read_file(const char* path, std::vector<uint8_t>& bytes, size_t expected)
{
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    bytes.resize(expected);
    const size_t got = std::fread(bytes.data(), 1, expected, f);
    const bool extra = std::fgetc(f) != EOF;
    std::fclose(f);
    return got == expected && !extra;
}

bool write_file(const char* path, const uint8_t* bytes, size_t size)
{
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(bytes, 1, size, f) == size;
    std::fclose(f);
    return ok;
}

// Packed NV12 (or P010, `bytes` = 2) rows into the surface's own layout,
// through a derived image.
bool upload(VADisplay display, VASurfaceID surface, const std::vector<uint8_t>& frame, unsigned width,
    unsigned height, unsigned bytes)
{
    VAImage image = {};
    if (vaDeriveImage(display, surface, &image) != VA_STATUS_SUCCESS)
        return false;
    void* mapped = nullptr;
    if (vaMapBuffer(display, image.buf, &mapped) != VA_STATUS_SUCCESS) {
        vaDestroyImage(display, image.image_id);
        return false;
    }
    auto* base = static_cast<uint8_t*>(mapped);
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(base + image.offsets[0] + static_cast<size_t>(y) * image.pitches[0],
            frame.data() + static_cast<size_t>(y) * width * bytes, width * bytes);
    for (unsigned y = 0; y < (height + 1) / 2; ++y)
        std::memcpy(base + image.offsets[1] + static_cast<size_t>(y) * image.pitches[1],
            frame.data() + static_cast<size_t>(width) * height * bytes + static_cast<size_t>(y) * width * bytes,
            width * bytes);
    vaUnmapBuffer(display, image.buf);
    vaDestroyImage(display, image.image_id);
    return true;
}

// The surface's visible pixels as packed rows, NV12 or P010.
bool download(VADisplay display, VASurfaceID surface, std::vector<uint8_t>& frame, unsigned width,
    unsigned height, uint32_t fourcc)
{
    const unsigned bytes = fourcc == VA_FOURCC_P010 ? 2 : 1;
    VAImageFormat format = {};
    format.fourcc = fourcc;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = bytes == 2 ? 24 : 12;
    VAImage image = {};
    if (vaCreateImage(display, &format, width, height, &image) != VA_STATUS_SUCCESS)
        return false;
    VAStatus status = vaGetImage(display, surface, 0, 0, width, height, image.image_id);
    if (status != VA_STATUS_SUCCESS) {
        vaDestroyImage(display, image.image_id);
        return false;
    }
    void* mapped = nullptr;
    if (vaMapBuffer(display, image.buf, &mapped) != VA_STATUS_SUCCESS) {
        vaDestroyImage(display, image.image_id);
        return false;
    }
    const auto* base = static_cast<const uint8_t*>(mapped);
    frame.resize(static_cast<size_t>(width) * height * bytes * 3 / 2);
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(frame.data() + static_cast<size_t>(y) * width * bytes,
            base + image.offsets[0] + static_cast<size_t>(y) * image.pitches[0], width * bytes);
    for (unsigned y = 0; y < (height + 1) / 2; ++y)
        std::memcpy(frame.data() + static_cast<size_t>(width) * height * bytes
                + static_cast<size_t>(y) * width * bytes,
            base + image.offsets[1] + static_cast<size_t>(y) * image.pitches[1], width * bytes);
    vaUnmapBuffer(display, image.buf);
    vaDestroyImage(display, image.image_id);
    return true;
}

// ---- HDR10 to SDR tone mapping ---------------------------------------------
//
// The pipeline test/vulkan-vpp/tonemap.comp documents, in double precision.
// The kernel runs it in float32, so agreement is a tolerance, not a hash:
// byte-exactness is not defined for a tone curve, but the curve is.

double pq_encode(double nits)
{
    const double m1 = 0.1593017578125, m2 = 78.84375;
    const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const double t = std::pow(std::max(nits, 0.0) / 10000.0, m1);
    return std::pow((c1 + c2 * t) / (1.0 + c3 * t), m2);
}

double pq_decode(double code)
{
    const double m1 = 0.1593017578125, m2 = 78.84375;
    const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const double t = std::pow(std::min(std::max(code, 0.0), 1.0), 1.0 / m2);
    return 10000.0 * std::pow(std::max(t - c1, 0.0) / (c2 - c3 * t), 1.0 / m1);
}

double bt709_oetf(double x)
{
    return x < 0.018 ? 4.5 * x : 1.099 * std::pow(x, 0.45) - 0.099;
}

struct ToneParams {
    double src_pq = 0.0, knee = 0.0, max_lum = 0.0;
};

// ITU-R BT.2390 Annex 2's Hermite shoulder, on PQ codes normalised by the
// source peak.
double eetf(double e, const ToneParams& p)
{
    e = std::min(std::max(e, 0.0), 1.0);
    if (e <= p.knee)
        return e;
    const double t = (e - p.knee) / (1.0 - p.knee);
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * p.knee + (t3 - 2.0 * t2 + t) * (1.0 - p.knee)
        + (-2.0 * t3 + 3.0 * t2) * p.max_lum;
}

ToneParams tone_params(double peak_nits)
{
    ToneParams p;
    p.src_pq = pq_encode(std::min(std::max(peak_nits, 100.0), 10000.0));
    p.max_lum = std::min(pq_encode(100.0) / p.src_pq, 1.0);
    p.knee = std::min(std::max(1.5 * p.max_lum - 0.5, 0.0), 0.999);
    return p;
}

// One pixel: PQ codes in, tone-mapped BT.709 OETF values out.
void tone_pixel(double y10, double u10, double v10, const ToneParams& p, double rgb[3])
{
    const double y = (y10 - 64.0) / 876.0;
    const double cb = (u10 - 512.0) / 896.0;
    const double cr = (v10 - 512.0) / 896.0;
    const double r = y + 1.4746 * cr;
    const double b = y + 1.8814 * cb;
    const double g = (y - 0.2627 * r - 0.0593 * b) / 0.6780;

    double nits[3] = { pq_decode(r), pq_decode(g), pq_decode(b) };
    const double peak = std::max(std::max(nits[0], nits[1]), nits[2]);
    if (peak <= 0.0) {
        rgb[0] = rgb[1] = rgb[2] = 0.0;
        return;
    }
    const double mapped = pq_decode(eetf(pq_encode(peak) / p.src_pq, p) * p.src_pq);
    double lin[3];
    for (int i = 0; i < 3; ++i)
        lin[i] = nits[i] * (mapped / peak / 100.0);

    // Linear BT.2020 to BT.709.
    double o[3] = {
        lin[0] * 1.660491 + lin[1] * -0.587641 + lin[2] * -0.072850,
        lin[0] * -0.124550 + lin[1] * 1.132900 + lin[2] * -0.008349,
        lin[0] * -0.018151 + lin[1] * -0.100579 + lin[2] * 1.118730,
    };
    const double luma = std::max(o[0] * 0.2126 + o[1] * 0.7152 + o[2] * 0.0722, 0.0);
    const double negative = std::max(-std::min(std::min(o[0], o[1]), o[2]), 0.0);
    const double scale = luma / std::max(luma + negative, 1e-20);
    for (int i = 0; i < 3; ++i)
        rgb[i] = bt709_oetf(std::min(std::max((o[i] + negative) * scale, 0.0), 1.0));
}

uint8_t limited(double normalized, double level, double amplitude)
{
    return static_cast<uint8_t>(std::min(std::max(std::round(level + amplitude * normalized), 0.0), 255.0));
}

size_t luma_offset(unsigned width, unsigned x, unsigned y) { return (static_cast<size_t>(y) * width + x) * 2U; }

size_t chroma_offset(unsigned width, unsigned height, unsigned bx, unsigned by)
{
    return static_cast<size_t>(width) * height * 2U + (static_cast<size_t>(by) * (width / 2) + bx) * 4U;
}

// A synthetic HDR10 surface: the first two columns are PQ black, the last two
// carry exactly the mastering peak, and the rest a geometric ramp from 0.05 to
// 10000 cd/m². Chroma sweeps both ways down the rows and is neutral at both
// ends, so a wrong matrix or a swapped chroma plane shows up as a diff rather
// than as a grey picture, and the two endpoints stay two exact codes.
void make_hdr10_frame(unsigned width, unsigned height, double peak_nits, std::vector<uint8_t>& frame)
{
    frame.assign(static_cast<size_t>(width) * height * 3U, 0);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            double nits;
            if (x < 2)
                nits = 0.0;
            else if (x + 2 >= width)
                nits = peak_nits;
            else
                nits = 0.05 * std::pow(10000.0 / 0.05, static_cast<double>(x - 2) / (width - 5));
            const unsigned code = static_cast<unsigned>(std::lround(64.0 + pq_encode(nits) * 876.0));
            const size_t off = luma_offset(width, x, y);
            frame[off] = static_cast<uint8_t>(code << 6U & 0xffU);
            frame[off + 1] = static_cast<uint8_t>(code << 6U >> 8U);
        }
        for (unsigned bx = 0; bx < width / 2; ++bx) {
            const bool neutral = bx == 0 || bx + 1 >= width / 2;
            const double sweep = neutral ? 0.0 : 2.0 * static_cast<double>(y) / height - 1.0;
            const unsigned u = static_cast<unsigned>(std::lround(512.0 + 96.0 * sweep));
            const unsigned v = static_cast<unsigned>(std::lround(512.0 - 64.0 * sweep));
            const size_t off = chroma_offset(width, height, bx, y / 2);
            frame[off] = static_cast<uint8_t>((u << 6U) & 0xffU);
            frame[off + 1] = static_cast<uint8_t>((u << 6U) >> 8U);
            frame[off + 2] = static_cast<uint8_t>((v << 6U) & 0xffU);
            frame[off + 3] = static_cast<uint8_t>((v << 6U) >> 8U);
        }
    }
}

// The same ramp in 8-bit NV12, for the refusal that proves a tone map needs a
// 10-bit source.
void make_nv12_frame(unsigned width, unsigned height, std::vector<uint8_t>& frame)
{
    frame.assign(static_cast<size_t>(width) * height * 3U / 2U, 128);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            frame[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(16 + (x * 200) / width);
}

double sample10(const std::vector<uint8_t>& frame, size_t off)
{
    return static_cast<double>(frame[off] | (frame[off + 1] << 8U)) / 64.0;
}

// The pipeline's own output for this frame, in packed NV12 rows. Chroma is
// averaged over the pixels of each 2x2 block, the way the kernel averages it:
// one pair covers two columns of two rows, and on a steep ramp the four pixels
// do not agree.
void expected_sdr(const std::vector<uint8_t>& hdr, unsigned width, unsigned height, double peak_nits,
    std::vector<uint8_t>& out)
{
    const ToneParams p = tone_params(peak_nits);
    out.assign(static_cast<size_t>(width) * height * 3U / 2U, 0);
    const unsigned cw = width / 2, ch = height / 2;
    std::vector<double> cb_sum(static_cast<size_t>(cw) * ch, 0.0);
    std::vector<double> cr_sum(static_cast<size_t>(cw) * ch, 0.0);
    std::vector<unsigned> samples(static_cast<size_t>(cw) * ch, 0);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const double u = sample10(hdr, chroma_offset(width, height, x / 2, y / 2));
            const double v = sample10(hdr, chroma_offset(width, height, x / 2, y / 2) + 2);
            double rgb[3];
            tone_pixel(sample10(hdr, luma_offset(width, x, y)), u, v, p, rgb);
            const double luma = rgb[0] * 0.2126 + rgb[1] * 0.7152 + rgb[2] * 0.0722;
            out[static_cast<size_t>(y) * width + x] = limited(luma, 16.0, 219.0);
            const size_t cell = static_cast<size_t>(y / 2) * cw + x / 2;
            cb_sum[cell] += (rgb[2] - luma) / 1.8556;
            cr_sum[cell] += (rgb[0] - luma) / 1.5748;
            ++samples[cell];
        }
    }
    for (unsigned by = 0; by < ch; ++by) {
        for (unsigned bx = 0; bx < cw; ++bx) {
            const size_t cell = static_cast<size_t>(by) * cw + bx;
            const double n = samples[cell] ? static_cast<double>(samples[cell]) : 0.0;
            const size_t uv = static_cast<size_t>(width) * height + static_cast<size_t>(by) * width + bx * 2;
            out[uv] = limited(n > 0.0 ? cb_sum[cell] / n : 0.0, 128.0, 224.0);
            out[uv + 1] = limited(n > 0.0 ? cr_sum[cell] / n : 0.0, 128.0, 224.0);
        }
    }
}

void tonemap_usage(const char* argv0)
{
    std::fprintf(stderr, "usage: %s tonemap <render-node> <W> <H> <peak-nits> <out.raw> [--source-nv12] "
                         "[--no-filter] [--rotate] [--expect-refusal]\n",
        argv0);
}

// The tone-mapping mode. `argv[0]` is the render node.
int run_tonemap(int argc, char** argv)
{
    if (argc < 5) {
        tonemap_usage("iris-va-vpp-probe");
        return 2;
    }
    const unsigned width = static_cast<unsigned>(std::atoi(argv[1]));
    const unsigned height = static_cast<unsigned>(std::atoi(argv[2]));
    const double peak = std::atof(argv[3]);
    const char* out_path = argv[4];
    bool source_nv12 = false, no_filter = false, rotate = false, expect_refusal = false;
    for (int i = 5; i < argc; ++i) {
        if (std::strcmp(argv[i], "--source-nv12") == 0)
            source_nv12 = true;
        else if (std::strcmp(argv[i], "--no-filter") == 0)
            no_filter = true;
        else if (std::strcmp(argv[i], "--rotate") == 0)
            rotate = true;
        else if (std::strcmp(argv[i], "--expect-refusal") == 0)
            expect_refusal = true;
        else {
            tonemap_usage("iris-va-vpp-probe");
            return 2;
        }
    }
    if (width < 4 || height < 4 || (width & 1U) || (height & 1U)) {
        std::fprintf(stderr, "tone-mapping geometry must be even and at least 4x4\n");
        return 2;
    }

    std::vector<uint8_t> hdr;
    make_hdr10_frame(width, height, peak, hdr);
    std::vector<uint8_t> nv12;
    make_nv12_frame(width, height, nv12);

    const int fd = open(argv[0], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::perror("open render node");
        return 2;
    }
    VADisplay display = vaGetDisplayDRM(fd);
    int major = 0, minor = 0;
    if (vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "vaInitialize failed (is V4L2_VA_VPP=1 set?)\n");
        return 2;
    }

    VAConfigAttrib attrib = {};
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
    VAConfigID config = VA_INVALID_ID;
    VAStatus status = vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &attrib, 1, &config);
    if (status != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "vaCreateConfig(VAProfileNone, VideoProc) failed: %s\n", vaErrorStr(status));
        return 2;
    }

    VASurfaceID src_surface = VA_INVALID_ID, dst_surface = VA_INVALID_ID;
    if (vaCreateSurfaces(display, source_nv12 ? VA_RT_FORMAT_YUV420 : VA_RT_FORMAT_YUV420_10, width, height,
            &src_surface, 1, nullptr, 0)
            != VA_STATUS_SUCCESS
        || vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height, &dst_surface, 1, nullptr, 0)
            != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "vaCreateSurfaces failed\n");
        return 2;
    }
    check(upload(display, src_surface, source_nv12 ? nv12 : hdr, width, height, source_nv12 ? 1 : 2),
        "upload the source frame");

    VAContextID context = VA_INVALID_ID;
    status = vaCreateContext(display, config, static_cast<int>(width), static_cast<int>(height), VA_PROGRESSIVE,
        &dst_surface, 1, &context);
    check(status == VA_STATUS_SUCCESS, "create a VideoProc context");

    // The filter has to be advertised before a client can ask for it, and the
    // caps have to name HDR10 in and SDR out rather than a generic conversion.
    VAProcFilterType filters[4] = {};
    unsigned filter_count = 4;
    check(vaQueryVideoProcFilters(display, context, filters, &filter_count) == VA_STATUS_SUCCESS
            && filter_count == 1 && filters[0] == VAProcFilterHighDynamicRangeToneMapping,
        "the HDR10 tone mapper is the one advertised filter");
    VAProcFilterCapHighDynamicRange filter_caps = {};
    unsigned cap_count = 1;
    const VAStatus caps_status = vaQueryVideoProcFilterCaps(display, context,
        VAProcFilterHighDynamicRangeToneMapping, &filter_caps, &cap_count);
    check(caps_status == VA_STATUS_SUCCESS && cap_count == 1
            && filter_caps.metadata_type == VAProcHighDynamicRangeMetadataHDR10
            && (filter_caps.caps_flag & VA_TONE_MAPPING_HDR_TO_SDR) != 0
            && (filter_caps.caps_flag & VA_TONE_MAPPING_HDR_TO_HDR) == 0,
        "the caps offer HDR10 to SDR and no HDR to HDR");

    VAHdrMetaDataHDR10 metadata = {};
    metadata.display_primaries_x[0] = 13250;
    metadata.display_primaries_y[0] = 34500;
    metadata.display_primaries_x[1] = 7500;
    metadata.display_primaries_y[1] = 3000;
    metadata.display_primaries_x[2] = 34000;
    metadata.display_primaries_y[2] = 16000;
    metadata.white_point_x = 15635;
    metadata.white_point_y = 16450;
    metadata.max_display_mastering_luminance = static_cast<uint32_t>(std::lround(peak * 10000.0));
    metadata.min_display_mastering_luminance = 50; // 0.005 cd/m2
    VAProcFilterParameterBufferHDRToneMapping params = {};
    params.type = VAProcFilterHighDynamicRangeToneMapping;
    params.data.metadata_type = VAProcHighDynamicRangeMetadataHDR10;
    params.data.metadata = &metadata;
    params.data.metadata_size = sizeof(metadata);

    VABufferID filter_buffer = VA_INVALID_ID;
    if (!no_filter) {
        status = vaCreateBuffer(display, context, VAProcFilterParameterBufferType, sizeof(params), 1, &params,
            &filter_buffer);
        check(status == VA_STATUS_SUCCESS, "create the HDR10 filter buffer");
    }

    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = src_surface;
    pipeline.surface_color_standard = VAProcColorStandardBT2020;
    pipeline.output_color_standard = VAProcColorStandardBT709;
    pipeline.num_filters = 1;
    pipeline.filters = &filter_buffer;
    pipeline.rotation_state = rotate ? VA_ROTATION_90 : VA_ROTATION_NONE;
    VABufferID pipeline_buffer = VA_INVALID_ID;
    status = vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(pipeline), 1, &pipeline,
        &pipeline_buffer);
    check(status == VA_STATUS_SUCCESS, "create the pipeline buffer");

    check(vaBeginPicture(display, context, dst_surface) == VA_STATUS_SUCCESS, "vaBeginPicture");
    VABufferID render[2] = { pipeline_buffer, filter_buffer };
    check(vaRenderPicture(display, context, render, no_filter ? 1 : 2) == VA_STATUS_SUCCESS, "vaRenderPicture");
    status = vaEndPicture(display, context);

    if (expect_refusal) {
        const bool refused = status != VA_STATUS_SUCCESS;
        std::printf("%s vaEndPicture refused the tone map (status 0x%x)\n", refused ? "PASS" : "FAIL",
            static_cast<unsigned>(status));
        if (!refused)
            ++failures;
    } else {
        check(status == VA_STATUS_SUCCESS, "vaEndPicture");
        std::vector<uint8_t> output;
        if (status == VA_STATUS_SUCCESS)
            check(download(display, dst_surface, output, width, height, VA_FOURCC_NV12),
                "download the tone-mapped frame");
        if (status == VA_STATUS_SUCCESS && output.size() == static_cast<size_t>(width) * height * 3U / 2U) {
            // Black in, limited-range black out, with neutral chroma.
            check(output[0] == 16 && output[static_cast<size_t>(width) * height] == 128
                    && output[static_cast<size_t>(width) * height + 1] == 128,
                "PQ black maps to 16/128/128");
            // The EETF's endpoint is the SDR reference white, so the source
            // peak lands at 235 whatever the peak was.
            const uint8_t white = output[static_cast<size_t>(height / 2) * width + width - 1];
            check(white >= 234 && white <= 236, "the mastering peak maps to limited-range white");

            std::vector<uint8_t> expected;
            expected_sdr(hdr, width, height, peak, expected);
            unsigned max_diff = 0;
            double sum = 0;
            size_t worst = 0;
            for (size_t i = 0; i < expected.size(); ++i) {
                const unsigned d = static_cast<unsigned>(std::abs(int(expected[i]) - int(output[i])));
                if (d > max_diff) {
                    max_diff = d;
                    worst = i;
                }
                sum += d;
            }
            // The model's frame beside the kernel's, so a disagreement names
            // the plane and the column instead of only a count.
            const std::string side = std::string(out_path) + ".expected";
            write_file(side.c_str(), expected.data(), expected.size());
            if (max_diff > 2)
                std::printf("worst offset=%zu plane=%s expected=%u got=%u\n", worst,
                    worst < static_cast<size_t>(width) * height ? "Y" : "chroma", expected[worst], output[worst]);
            check(max_diff <= 2, "every byte within 2 codes of the documented pipeline");
            // A monotone ramp in, a monotone ramp out: a knee applied to the
            // wrong variable or a sign error in the matrix breaks this even
            // when the tolerance above is generous.
            bool monotone = true;
            const unsigned row = height / 2;
            for (unsigned x = 3; x + 2 < width; ++x)
                monotone = monotone
                    && output[static_cast<size_t>(row) * width + x]
                    >= output[static_cast<size_t>(row) * width + x - 1];
            check(monotone, "the luminance ramp stays monotone");
            check(write_file(out_path, output.data(), output.size()), "write the output frame");
            std::printf("tonemap %ux%u peak=%.0f max_diff=%u mean=%.3f\n", width, height, peak, max_diff,
                sum / static_cast<double>(expected.size()));
        }
    }

    vaDestroySurfaces(display, &src_surface, 1);
    vaDestroySurfaces(display, &dst_surface, 1);
    if (context != VA_INVALID_ID)
        vaDestroyContext(display, context);
    vaDestroyConfig(display, config);
    vaTerminate(display);
    close(fd);

    if (failures) {
        std::printf("FAIL iris-va-vpp-probe tonemap %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS iris-va-vpp-probe tonemap %ux%u peak=%.0f\n", width, height, peak);
    return 0;
}

// ---- global-alpha compositing ----------------------------------------------
//
// The contract test/vulkan-vpp/overlay.comp documents: the main surface stays
// in place, the overlay surface is scaled into one rectangle of the output at
// swscale's point positions, and inside it
//
//     out = alpha * overlay + (1 - alpha) * main
//
// on the coded values of both planes. The rectangle is even, so a chroma cell
// is never half blended. Below is the same map and the same mix in double
// precision, plus two frames that change on both axes so a transposed or
// shifted sample shows up as a difference.

unsigned step16(unsigned src, unsigned dst)
{
    return ((src << 16) + (dst >> 1)) / dst;
}

unsigned pick16(unsigned i, unsigned inc)
{
    return (i * inc + (inc >> 1)) >> 16;
}

// Packed rows, NV12 or P010, with a pattern that differs on every row and
// column of both planes.
void make_pattern(unsigned width, unsigned height, unsigned bytes, unsigned seed, std::vector<uint8_t>& frame)
{
    const unsigned top = bytes == 2 ? 1024u : 256u;
    frame.assign(static_cast<size_t>(width) * height * bytes * 3 / 2, 0);
    const auto put = [&](size_t sample, unsigned code) {
        const unsigned stored = bytes == 2 ? code << 6 : code;
        frame[sample * bytes] = static_cast<uint8_t>(stored & 0xff);
        if (bytes == 2)
            frame[sample * 2 + 1] = static_cast<uint8_t>(stored >> 8);
    };
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            put(static_cast<size_t>(y) * width + x, (x * 3 + y * 5 + seed) % top);
    for (unsigned y = 0; y < height / 2; ++y)
        for (unsigned x = 0; x < width / 2; ++x) {
            const size_t cell = static_cast<size_t>(width) * height + static_cast<size_t>(y) * width + x * 2;
            put(cell, (x * 7 + y * 11 + seed) % top);
            put(cell + 1, (x * 13 + y * 17 + seed * 3) % top);
        }
}

unsigned code_at(const std::vector<uint8_t>& frame, size_t sample, unsigned bytes)
{
    if (bytes == 1)
        return frame[sample];
    return (static_cast<unsigned>(frame[sample * 2]) | (static_cast<unsigned>(frame[sample * 2 + 1]) << 8)) >> 6;
}

void set_code(std::vector<uint8_t>& frame, size_t sample, unsigned bytes, unsigned code)
{
    const unsigned stored = bytes == 2 ? code << 6 : code;
    frame[sample * bytes] = static_cast<uint8_t>(stored & 0xff);
    if (bytes == 2)
        frame[sample * 2 + 1] = static_cast<uint8_t>(stored >> 8);
}

struct Mix {
    std::vector<uint8_t> main;
    std::vector<uint8_t> overlay;
    unsigned width = 0, height = 0;
    unsigned overlay_width = 0, overlay_height = 0;
    unsigned x = 0, y = 0, rect_width = 0, rect_height = 0;
    unsigned bytes = 1;
    double alpha = 1.0;
};

double mixed(double m, double o, double alpha)
{
    return alpha * o + (1.0 - alpha) * m;
}

void expected_composite(const Mix& mix, std::vector<uint8_t>& out)
{
    out = mix.main;
    const unsigned hx = step16(mix.overlay_width, mix.rect_width);
    const unsigned hy = step16(mix.overlay_height, mix.rect_height);
    for (unsigned dy = 0; dy < mix.rect_height; ++dy)
        for (unsigned dx = 0; dx < mix.rect_width; ++dx) {
            const size_t m = static_cast<size_t>(mix.y + dy) * mix.width + mix.x + dx;
            const size_t o = static_cast<size_t>(pick16(dy, hy)) * mix.overlay_width + pick16(dx, hx);
            const double v = mixed(code_at(out, m, mix.bytes), code_at(mix.overlay, o, mix.bytes), mix.alpha);
            set_code(out, m, mix.bytes, static_cast<unsigned>(std::lround(v)));
        }
    // Chroma cells cover 2x2 luma pixels, and the rectangle is even, so a cell
    // is inside it exactly when its own coordinates are.
    const unsigned hx_c = step16(mix.overlay_width / 2, mix.rect_width / 2);
    const unsigned hy_c = step16(mix.overlay_height / 2, mix.rect_height / 2);
    for (unsigned cy = 0; cy < mix.rect_height / 2; ++cy)
        for (unsigned cx = 0; cx < mix.rect_width / 2; ++cx) {
            const size_t m = static_cast<size_t>(mix.width) * mix.height
                + static_cast<size_t>(mix.y / 2 + cy) * mix.width + (mix.x / 2 + cx) * 2;
            const size_t o = static_cast<size_t>(mix.overlay_width) * mix.overlay_height
                + static_cast<size_t>(pick16(cy, hy_c)) * mix.overlay_width + pick16(cx, hx_c) * 2;
            for (unsigned k = 0; k < 2; ++k) {
                const double v = mixed(code_at(out, m + k, mix.bytes), code_at(mix.overlay, o + k, mix.bytes),
                    mix.alpha);
                set_code(out, m + k, mix.bytes, static_cast<unsigned>(std::lround(v)));
            }
        }
}

void overlay_usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s overlay <render-node> <W> <H> <ovW> <ovH> <x> <y> <rw> <rh> <alpha> <out.raw>\n"
        "                 [--p010] [--premultiplied] [--luma-key] [--third] [--blend-first]\n"
        "                 [--main-crop]\n"
        "                 [--expect-refusal]\n",
        argv0);
}

// The compositing mode. `argv[0]` is the render node.
int run_overlay(int argc, char** argv)
{
    if (argc < 11) {
        overlay_usage("iris-va-vpp-probe");
        return 2;
    }
    const unsigned width = static_cast<unsigned>(std::atoi(argv[1]));
    const unsigned height = static_cast<unsigned>(std::atoi(argv[2]));
    const unsigned ov_w = static_cast<unsigned>(std::atoi(argv[3]));
    const unsigned ov_h = static_cast<unsigned>(std::atoi(argv[4]));
    const unsigned x = static_cast<unsigned>(std::atoi(argv[5]));
    const unsigned y = static_cast<unsigned>(std::atoi(argv[6]));
    const unsigned rw = static_cast<unsigned>(std::atoi(argv[7]));
    const unsigned rh = static_cast<unsigned>(std::atoi(argv[8]));
    const double alpha = std::atof(argv[9]);
    const char* out_path = argv[10];
    bool p010 = false, premultiplied = false, luma_key = false, third = false, blend_first = false;
    bool main_crop = false, expect_refusal = false;
    for (int i = 11; i < argc; ++i) {
        if (std::strcmp(argv[i], "--p010") == 0)
            p010 = true;
        else if (std::strcmp(argv[i], "--premultiplied") == 0)
            premultiplied = true;
        else if (std::strcmp(argv[i], "--luma-key") == 0)
            luma_key = true;
        else if (std::strcmp(argv[i], "--third") == 0)
            third = true;
        else if (std::strcmp(argv[i], "--blend-first") == 0)
            blend_first = true;
        else if (std::strcmp(argv[i], "--main-crop") == 0)
            main_crop = true;
        else if (std::strcmp(argv[i], "--expect-refusal") == 0)
            expect_refusal = true;
        else {
            overlay_usage("iris-va-vpp-probe");
            return 2;
        }
    }
    if (width < 4 || height < 4 || ov_w < 2 || ov_h < 2 || (width & 1U) || (height & 1U) || (ov_w & 1U)
        || (ov_h & 1U)) {
        std::fprintf(stderr, "overlay geometry must be even, the surfaces at least 4x4\n");
        return 2;
    }

    const unsigned bytes = p010 ? 2 : 1;
    const uint32_t fourcc = p010 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    const unsigned rt_format = p010 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    Mix mix;
    mix.width = width;
    mix.height = height;
    mix.overlay_width = ov_w;
    mix.overlay_height = ov_h;
    mix.x = x;
    mix.y = y;
    mix.rect_width = rw;
    mix.rect_height = rh;
    mix.bytes = bytes;
    mix.alpha = alpha;
    make_pattern(width, height, bytes, 1, mix.main);
    make_pattern(ov_w, ov_h, bytes, 2, mix.overlay);

    const int fd = open(argv[0], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::perror("open render node");
        return 2;
    }
    VADisplay display = vaGetDisplayDRM(fd);
    int major = 0, minor = 0;
    if (vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "vaInitialize failed (is V4L2_VA_VPP=1 set?)\n");
        return 2;
    }

    VAConfigAttrib attrib = {};
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
    VAConfigID config = VA_INVALID_ID;
    VAStatus status = vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &attrib, 1, &config);
    if (status != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "vaCreateConfig(VAProfileNone, VideoProc) failed: %s\n", vaErrorStr(status));
        return 2;
    }

    VASurfaceID main_surface = VA_INVALID_ID, overlay_surface = VA_INVALID_ID, dst_surface = VA_INVALID_ID;
    VASurfaceID created[3] = { VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID };
    check(vaCreateSurfaces(display, rt_format, width, height, &main_surface, 1, nullptr, 0)
            == VA_STATUS_SUCCESS,
        "create the main surface");
    check(vaCreateSurfaces(display, rt_format, ov_w, ov_h, &overlay_surface, 1, nullptr, 0)
            == VA_STATUS_SUCCESS,
        "create the overlay surface");
    check(vaCreateSurfaces(display, rt_format, width, height, &dst_surface, 1, nullptr, 0)
            == VA_STATUS_SUCCESS,
        "create the output surface");

    VAContextID context = VA_INVALID_ID;
    check(vaCreateContext(display, config, static_cast<int>(width), static_cast<int>(height), VA_PROGRESSIVE,
              &dst_surface, 1, &context)
            == VA_STATUS_SUCCESS,
        "create a VideoProc context");
    if (context != VA_INVALID_ID) {
        // `overlay_vaapi` will not build at all unless the driver reports the
        // global alpha, so a client check is worth making here too.
        VAProcPipelineCaps caps = {};
        check(vaQueryVideoProcPipelineCaps(display, context, nullptr, 0, &caps) == VA_STATUS_SUCCESS
                && (caps.blend_flags & VA_BLEND_GLOBAL_ALPHA) != 0,
            "the pipeline caps offer global-alpha blending");
        check((caps.blend_flags & (VA_BLEND_PREMULTIPLIED_ALPHA | VA_BLEND_LUMA_KEY)) == 0,
            "no per-pixel alpha and no luma key are advertised");
    }

    if (main_surface != VA_INVALID_ID)
        check(upload(display, main_surface, mix.main, width, height, bytes), "upload the main frame");
    if (overlay_surface != VA_INVALID_ID)
        check(upload(display, overlay_surface, mix.overlay, ov_w, ov_h, bytes), "upload the overlay frame");

    VABlendState blend = {};
    blend.flags = VA_BLEND_GLOBAL_ALPHA;
    if (premultiplied)
        blend.flags |= VA_BLEND_PREMULTIPLIED_ALPHA;
    if (luma_key)
        blend.flags |= VA_BLEND_LUMA_KEY;
    blend.global_alpha = static_cast<float>(alpha);

    VARectangle full = {};
    full.width = static_cast<uint16_t>(width);
    full.height = static_cast<uint16_t>(height);
    VARectangle region = {};
    region.x = static_cast<int16_t>(x);
    region.y = static_cast<int16_t>(y);
    region.width = static_cast<uint16_t>(rw);
    region.height = static_cast<uint16_t>(rh);

    // FFmpeg's shape: the main pass fills the output, and the overlay pass is
    // that same buffer with its own surface, region and blend state.
    VAProcPipelineParameterBuffer main_params = {};
    main_params.surface = main_surface;
    // FFmpeg names the main frame's whole region even though it covers the
    // pass, and leaves the overlay's region null; both shapes are exercised.
    VARectangle half = {};
    half.width = static_cast<uint16_t>(width / 2);
    half.height = static_cast<uint16_t>(height / 2);
    main_params.surface_region = main_crop ? &half : &full;
    main_params.output_region = &full;
    main_params.surface_color_standard = VAProcColorStandardBT709;
    main_params.output_color_standard = VAProcColorStandardBT709;
    VAProcPipelineParameterBuffer overlay_params = main_params;
    overlay_params.surface = overlay_surface;
    // FFmpeg clears the overlay's region even though it copies the main pass:
    // the overlay surface is scaled into its output region as a whole.
    overlay_params.surface_region = nullptr;
    overlay_params.output_region = &region;
    overlay_params.blend_state = &blend;

    VABufferID main_buffer = VA_INVALID_ID, overlay_buffer = VA_INVALID_ID;
    check(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(main_params), 1,
              &main_params, &main_buffer)
            == VA_STATUS_SUCCESS,
        "create the main pipeline buffer");
    check(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(overlay_params), 1,
              &overlay_params, &overlay_buffer)
            == VA_STATUS_SUCCESS,
        "create the blending pipeline buffer");

    VAStatus begin = vaBeginPicture(display, context, dst_surface);
    VABufferID order[3] = { main_buffer, overlay_buffer, overlay_buffer };
    int count = third ? 3 : 2;
    if (blend_first) {
        order[0] = overlay_buffer;
        order[1] = main_buffer;
    }
    VAStatus render = begin == VA_STATUS_SUCCESS
        ? vaRenderPicture(display, context, order, count)
        : begin;
    status = render == VA_STATUS_SUCCESS ? vaEndPicture(display, context) : render;

    if (expect_refusal) {
        const bool refused = render != VA_STATUS_SUCCESS || status != VA_STATUS_SUCCESS;
        std::printf("%s the driver refused the composite (render 0x%x, end 0x%x)\n", refused ? "PASS" : "FAIL",
            static_cast<unsigned>(render), static_cast<unsigned>(status));
        if (!refused)
            ++failures;
    } else {
        check(render == VA_STATUS_SUCCESS, "vaRenderPicture");
        check(status == VA_STATUS_SUCCESS, "vaEndPicture");
        std::vector<uint8_t> output;
        if (status == VA_STATUS_SUCCESS)
            check(download(display, dst_surface, output, width, height, fourcc), "download the output frame");
        if (status == VA_STATUS_SUCCESS && output.size() == mix.main.size()) {
            std::vector<uint8_t> expected;
            expected_composite(mix, expected);
            unsigned max_diff = 0;
            size_t worst = 0;
            for (size_t i = 0; i < expected.size(); ++i) {
                const unsigned d = static_cast<unsigned>(std::abs(int(expected[i]) - int(output[i])));
                if (d > max_diff) {
                    max_diff = d;
                    worst = i;
                }
            }
            const std::string side = std::string(out_path) + ".expected";
            write_file(side.c_str(), expected.data(), expected.size());
            if (max_diff > 1)
                std::printf("worst offset=%zu plane=%s expected=%u got=%u\n", worst,
                    worst < static_cast<size_t>(width) * height * bytes ? "Y" : "chroma", expected[worst],
                    output[worst]);
            // 0, 1/2 and 1 are exact in binary, so all but the last code of
            // slack is float32 against double; an alpha that is not dyadic
            // may round a tie either way.
            check(max_diff <= 1, "every byte within 1 code of the documented mix");
            // Inside the rectangle the output has to differ from the main
            // frame whenever alpha and the two frames differ: a kernel that
            // silently copied the main surface would pass a loose tolerance
            // test but fail this one.
            if (alpha > 0.0) {
                bool moved = false;
                for (unsigned dy = 0; dy < rh && !moved; ++dy)
                    for (unsigned dx = 0; dx < rw && !moved; ++dx) {
                        const size_t m = static_cast<size_t>(y + dy) * width + x + dx;
                        moved = code_at(output, m, bytes) != code_at(mix.main, m, bytes);
                    }
                check(moved, "the overlay changed the pixels inside its rectangle");
            }
            check(write_file(out_path, output.data(), output.size()), "write the output frame");
            // The two inputs beside it, so the gate can compare a full-cover
            // alpha of 0 or 1 with the frame it should reproduce byte for
            // byte instead of trusting only the model.
            write_file((std::string(out_path) + ".main").c_str(), mix.main.data(), mix.main.size());
            write_file((std::string(out_path) + ".overlay").c_str(), mix.overlay.data(), mix.overlay.size());
            std::printf("overlay %ux%u rect=%ux%u+%u,%u alpha=%.2f max_diff=%u\n", width, height, rw, rh, x, y,
                alpha, max_diff);
        }
    }

    created[0] = main_surface;
    created[1] = overlay_surface;
    created[2] = dst_surface;
    for (VASurfaceID id : created)
        if (id != VA_INVALID_ID)
            vaDestroySurfaces(display, &id, 1);
    if (context != VA_INVALID_ID)
        vaDestroyContext(display, context);
    vaDestroyConfig(display, config);
    vaTerminate(display);
    close(fd);

    if (failures) {
        std::printf("FAIL iris-va-vpp-probe overlay %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS iris-va-vpp-probe overlay %ux%u rect=%ux%u+%u,%u alpha=%.2f\n", width, height, rw, rh, x, y,
        alpha);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::strcmp(argv[1], "tonemap") == 0)
        return run_tonemap(argc - 2, argv + 2);
    if (argc >= 2 && std::strcmp(argv[1], "overlay") == 0)
        return run_overlay(argc - 2, argv + 2);

    const bool expect_refusal = argc == 13 && std::strcmp(argv[12], "--expect-refusal") == 0;
    if (argc != 12 && !expect_refusal) {
        std::fprintf(stderr, "usage: %s <render-node> <src.raw> <srcW> <srcH> <cropW> <cropH> <cropX> "
                             "<cropY> <dstW> <dstH> <out.raw> [--expect-refusal]\n",
            argv[0]);
        return 2;
    }
    const unsigned src_w = static_cast<unsigned>(std::atoi(argv[3]));
    const unsigned src_h = static_cast<unsigned>(std::atoi(argv[4]));
    VARectangle region = {};
    region.width = static_cast<uint16_t>(std::atoi(argv[5]));
    region.height = static_cast<uint16_t>(std::atoi(argv[6]));
    region.x = static_cast<int16_t>(std::atoi(argv[7]));
    region.y = static_cast<int16_t>(std::atoi(argv[8]));
    const unsigned dst_w = static_cast<unsigned>(std::atoi(argv[9]));
    const unsigned dst_h = static_cast<unsigned>(std::atoi(argv[10]));

    std::vector<uint8_t> source;
    if (!read_file(argv[2], source, static_cast<size_t>(src_w) * src_h * 3 / 2)) {
        std::fprintf(stderr, "cannot read %s as an NV12 %ux%u frame\n", argv[2], src_w, src_h);
        return 2;
    }

    const int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::perror("open render node");
        return 2;
    }
    VADisplay display = vaGetDisplayDRM(fd);
    int major = 0, minor = 0;
    if (vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "vaInitialize failed (is V4L2_VA_VPP=1 set?)\n");
        return 2;
    }

    VAConfigAttrib attrib = {};
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = VA_RT_FORMAT_YUV420;
    VAConfigID config = VA_INVALID_ID;
    VAStatus status = vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &attrib, 1, &config);
    if (status != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "vaCreateConfig(VAProfileNone, VideoProc) failed: %s\n", vaErrorStr(status));
        return 2;
    }

    VASurfaceID src_surface = VA_INVALID_ID, dst_surface = VA_INVALID_ID;
    if (vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, src_w, src_h, &src_surface, 1, nullptr, 0)
            != VA_STATUS_SUCCESS
        || vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, dst_w, dst_h, &dst_surface, 1, nullptr, 0)
            != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "vaCreateSurfaces failed\n");
        return 2;
    }
    check(upload(display, src_surface, source, src_w, src_h, 1), "upload the source frame");

    VAContextID context = VA_INVALID_ID;
    status = vaCreateContext(display, config, static_cast<int>(dst_w), static_cast<int>(dst_h), VA_PROGRESSIVE,
        &dst_surface, 1, &context);
    check(status == VA_STATUS_SUCCESS, "create a VideoProc context");

    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = src_surface;
    pipeline.surface_region = &region;
    VABufferID pipeline_buffer = VA_INVALID_ID;
    status = vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(pipeline), 1, &pipeline,
        &pipeline_buffer);
    check(status == VA_STATUS_SUCCESS, "create the pipeline buffer");

    check(vaBeginPicture(display, context, dst_surface) == VA_STATUS_SUCCESS, "vaBeginPicture");
    check(vaRenderPicture(display, context, &pipeline_buffer, 1) == VA_STATUS_SUCCESS, "vaRenderPicture");
    status = vaEndPicture(display, context);

    if (expect_refusal) {
        const bool refused = status != VA_STATUS_SUCCESS;
        std::printf("%s vaEndPicture refused the region (status 0x%x)\n", refused ? "PASS" : "FAIL",
            static_cast<unsigned>(status));
        if (!refused)
            ++failures;
    } else {
        check(status == VA_STATUS_SUCCESS, "vaEndPicture");
        std::vector<uint8_t> output;
        if (status == VA_STATUS_SUCCESS)
            check(download(display, dst_surface, output, dst_w, dst_h, VA_FOURCC_NV12),
                "download the processed frame");
        if (!output.empty())
            check(output.size() == static_cast<size_t>(dst_w) * dst_h * 3 / 2
                    && write_file(argv[11], output.data(), output.size()),
                "write the output frame");
    }

    vaDestroySurfaces(display, &src_surface, 1);
    vaDestroySurfaces(display, &dst_surface, 1);
    if (context != VA_INVALID_ID)
        vaDestroyContext(display, context);
    vaDestroyConfig(display, config);
    vaTerminate(display);
    close(fd);

    if (failures)
        std::printf("FAIL iris-va-vpp-probe %d check(s)\n", failures);
    else
        std::printf("PASS iris-va-vpp-probe crop=%ux%u+%d+%d -> %ux%u\n", region.width, region.height,
            region.x, region.y, dst_w, dst_h);
    return failures ? 1 : 0;
}
