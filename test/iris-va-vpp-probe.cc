// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): one VideoProc picture, driven the way a
// client drives it.
//
//   iris-va-vpp-probe <render-node> <src.raw> <srcW> <srcH>
//                     <cropW> <cropH> <cropX> <cropY> <dstW> <dstH> <out.raw>
//                     [--expect-refusal] [--interp=nearest|bilinear|advanced]
//
// It uploads the NV12 frame through a derived image, creates a
// VAProfileNone / VAEntrypointVideoProc context, submits a
// VAProcPipelineParameterBuffer whose surface_region is the crop, ends the
// picture and downloads the result with vaGetImage. FFmpeg 8.1 has no
// crop_vaapi filter, so this is the only client that can ask for a region.
// `--interp` names the interpolation method in filter_flags. All three have a
// kernel and each must run as itself: `nearest` is the point map, `bilinear`
// the four-tap blend, `advanced` the driver's own pick. The caps name the best
// of the three.
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
//                     [--p010] [--premultiplied] [--luma-key] [--layers=N]
//                     [--blend-first] [--main-crop] [--pad=<columns>]
//                     [--expect-refusal]
//
// The overlay mode submits two or more pipeline buffers in one picture, the way
// overlay_vaapi does: the main surface over the whole output, then the
// overlay surface into one rectangle with a VABlendState naming a global
// alpha. It compares the result with the mix documented in
// test/vulkan-vpp/overlay.comp, and checks that the caps advertise
// VA_BLEND_GLOBAL_ALPHA, which is what overlay_vaapi refuses to run without.
//
// `--expect-refusal` inverts the verdict: the picture must fail (an odd or
// out-of-bounds region, an NV12 source, a filter without its parameters, a
// rotation, a blending pass rendered before the main one, more than sixteen
// layers, all are the driver's to refuse rather than to round).
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
// through a derived image; `bytes` = 4 is a packed BGRA/RGBA overlay, which
// has one plane.
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
    if (bytes != 4)
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

// The scale kernel's map (test/vulkan-vpp/nv12_scale.comp) applied to a
// converted frame: the crop is rotated clockwise, mirrored, then sampled at
// swscale's point positions into the destination. This is the pass the
// processor runs after the tone mapper, so the probe's model composes them in
// the same order.
unsigned step16(unsigned src, unsigned dst);
unsigned pick16(unsigned i, unsigned inc);

void map_frame(const std::vector<uint8_t>& in, unsigned in_w, unsigned in_h, unsigned cx, unsigned cy,
    unsigned cw, unsigned ch, unsigned rot, unsigned mirror, unsigned out_w, unsigned out_h,
    std::vector<uint8_t>& out)
{
    out.assign(static_cast<size_t>(out_w) * out_h * 3U / 2U, 0);
    const bool transposed = (rot & 1U) != 0U;
    const unsigned rw = transposed ? ch : cw, rh = transposed ? cw : ch;
    auto mirror_at = [&](unsigned& x, unsigned& y, unsigned maxw, unsigned maxh) {
        if ((mirror & 1U) != 0U)
            x = maxw - 1U - x;
        if ((mirror & 2U) != 0U)
            y = maxh - 1U - y;
    };
    auto unrotate = [&](unsigned rx, unsigned ry, unsigned w, unsigned h, unsigned& sx, unsigned& sy) {
        switch (rot & 3U) {
        case 1U: sx = ry; sy = h - 1U - rx; break;
        case 2U: sx = w - 1U - rx; sy = h - 1U - ry; break;
        case 3U: sx = w - 1U - ry; sy = rx; break;
        default: sx = rx; sy = ry; break;
        }
    };
    const unsigned hx = step16(rw, out_w), hy = step16(rh, out_h);
    for (unsigned dy = 0; dy < out_h; ++dy) {
        unsigned ry = pick16(dy, hy);
        for (unsigned dx = 0; dx < out_w; ++dx) {
            // The mirror maps one pixel, so it works on a copy: flipping the
            // running row coordinate would undo itself on the next column.
            unsigned mx = pick16(dx, hx), my = ry;
            mirror_at(mx, my, rw, rh);
            unsigned sx = 0, sy = 0;
            unrotate(mx, my, cw, ch, sx, sy);
            out[static_cast<size_t>(dy) * out_w + dx] = in[static_cast<size_t>(cy + sy) * in_w + cx + sx];
        }
    }
    const unsigned ccw = cw >> 1, cch = ch >> 1;
    const unsigned crw = transposed ? cch : ccw, crh = transposed ? ccw : cch;
    const unsigned dcw = out_w >> 1, dch = out_h >> 1;
    const unsigned chx = step16(crw, dcw), chy = step16(crh, dch);
    for (unsigned dy = 0; dy < dch; ++dy) {
        unsigned ry = pick16(dy, chy);
        for (unsigned dx = 0; dx < dcw; ++dx) {
            unsigned mx = pick16(dx, chx), my = ry;
            mirror_at(mx, my, crw, crh);
            unsigned sx = 0, sy = 0;
            unrotate(mx, my, ccw, cch, sx, sy);
            const size_t from = static_cast<size_t>(in_w) * in_h
                + static_cast<size_t>((cy >> 1) + sy) * in_w + ((cx >> 1) + sx) * 2U;
            const size_t to = static_cast<size_t>(out_w) * out_h + static_cast<size_t>(dy) * out_w + dx * 2U;
            out[to] = in[from];
            out[to + 1] = in[from + 1];
        }
    }
}

void tonemap_usage(const char* argv0)
{
    std::fprintf(stderr, "usage: %s tonemap <render-node> <W> <H> <peak-nits> <out.raw> [--source-nv12] "
                         "[--no-filter] [--expect-refusal] [--dst=WxH] [--crop=x,y,w,h] [--rotate=N] "
                         "[--mirror=N]\n",
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
    bool source_nv12 = false, no_filter = false, expect_refusal = false;
    unsigned dst_width = width, dst_height = height;
    unsigned crop_x = 0, crop_y = 0, crop_w = width, crop_h = height;
    unsigned rot = 0, mirror = 0;
    for (int i = 5; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--source-nv12") == 0)
            source_nv12 = true;
        else if (std::strcmp(arg, "--no-filter") == 0)
            no_filter = true;
        else if (std::strcmp(arg, "--expect-refusal") == 0)
            expect_refusal = true;
        else if (std::strncmp(arg, "--dst=", 6) == 0)
            std::sscanf(arg + 6, "%ux%u", &dst_width, &dst_height);
        else if (std::strncmp(arg, "--crop=", 7) == 0)
            std::sscanf(arg + 7, "%u,%u,%u,%u", &crop_x, &crop_y, &crop_w, &crop_h);
        else if (std::strncmp(arg, "--rotate=", 9) == 0)
            rot = static_cast<unsigned>(std::atoi(arg + 9));
        else if (std::strncmp(arg, "--mirror=", 9) == 0)
            mirror = static_cast<unsigned>(std::atoi(arg + 9));
        else {
            tonemap_usage("iris-va-vpp-probe");
            return 2;
        }
    }
    if (width < 4 || height < 4 || (width & 1U) || (height & 1U)) {
        std::fprintf(stderr, "tone-mapping geometry must be even and at least 4x4\n");
        return 2;
    }
    if (rot > 3 || mirror > 3 || dst_width < 2 || dst_height < 2 || (dst_width & 1U) || (dst_height & 1U)) {
        std::fprintf(stderr, "rotation 0..3, mirror 0..3, an even destination of at least 2x2\n");
        return 2;
    }
    const bool geometry = crop_x || crop_y || crop_w != width || crop_h != height || dst_width != width
        || dst_height != height || rot || mirror;

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
        || vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, dst_width, dst_height, &dst_surface, 1, nullptr, 0)
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

    VARectangle region = {};
    region.x = static_cast<int16_t>(crop_x);
    region.y = static_cast<int16_t>(crop_y);
    region.width = static_cast<uint16_t>(crop_w);
    region.height = static_cast<uint16_t>(crop_h);

    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = src_surface;
    pipeline.surface_color_standard = VAProcColorStandardBT2020;
    pipeline.output_color_standard = VAProcColorStandardBT709;
    pipeline.num_filters = 1;
    pipeline.filters = &filter_buffer;
    pipeline.surface_region = geometry ? &region : nullptr;
    pipeline.rotation_state = rot == 1 ? VA_ROTATION_90
        : rot == 2                    ? VA_ROTATION_180
        : rot == 3                    ? VA_ROTATION_270
                                      : VA_ROTATION_NONE;
    pipeline.mirror_state = mirror == 1 ? VA_MIRROR_HORIZONTAL
        : mirror == 2                   ? VA_MIRROR_VERTICAL
        : mirror == 3                   ? (VA_MIRROR_HORIZONTAL | VA_MIRROR_VERTICAL)
                                        : VA_MIRROR_NONE;
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
            check(download(display, dst_surface, output, dst_width, dst_height, VA_FOURCC_NV12),
                "download the tone-mapped frame");
        if (status == VA_STATUS_SUCCESS
            && output.size() == static_cast<size_t>(dst_width) * dst_height * 3U / 2U) {
            if (!geometry) {
                // Black in, limited-range black out, with neutral chroma.
                check(output[0] == 16 && output[static_cast<size_t>(width) * height] == 128
                        && output[static_cast<size_t>(width) * height + 1] == 128,
                    "PQ black maps to 16/128/128");
                // The EETF's endpoint is the SDR reference white, so the source
                // peak lands at 235 whatever the peak was.
                const uint8_t white = output[static_cast<size_t>(height / 2) * width + width - 1];
                check(white >= 234 && white <= 236, "the mastering peak maps to limited-range white");
            }

            std::vector<uint8_t> expected, sdr;
            expected_sdr(hdr, width, height, peak, sdr);
            if (geometry)
                map_frame(sdr, width, height, crop_x, crop_y, crop_w, crop_h, rot, mirror, dst_width,
                    dst_height, expected);
            else
                expected = sdr;
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
            // when the tolerance above is generous. Only the unrotated frame
            // keeps the ramp along a row.
            if (!geometry) {
                bool monotone = true;
                const unsigned row = height / 2;
                for (unsigned x = 3; x + 2 < width; ++x)
                    monotone = monotone
                        && output[static_cast<size_t>(row) * width + x]
                        >= output[static_cast<size_t>(row) * width + x - 1];
                check(monotone, "the luminance ramp stays monotone");
            }
            check(write_file(out_path, output.data(), output.size()), "write the output frame");
            std::printf("tonemap %ux%u -> %ux%u rot=%u mirror=%u peak=%.0f max_diff=%u mean=%.3f\n", width,
                height, dst_width, dst_height, rot, mirror, peak, max_diff,
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

// The left `width` columns of a `full_width`-wide frame, as packed rows: what
// a client's picture looks like inside a decode surface wider than it is.
void crop_left(const std::vector<uint8_t>& full, unsigned full_width, unsigned width, unsigned height,
    unsigned bytes, std::vector<uint8_t>& out)
{
    out.assign(static_cast<size_t>(width) * height * bytes * 3 / 2, 0);
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(&out[static_cast<size_t>(y) * width * bytes],
            &full[static_cast<size_t>(y) * full_width * bytes], static_cast<size_t>(width) * bytes);
    const size_t luma = static_cast<size_t>(width) * height * bytes;
    const size_t full_luma = static_cast<size_t>(full_width) * height * bytes;
    for (unsigned y = 0; y < height / 2; ++y)
        std::memcpy(&out[luma + static_cast<size_t>(y) * width * bytes],
            &full[full_luma + static_cast<size_t>(y) * full_width * bytes], static_cast<size_t>(width) * bytes);
}

unsigned code_at(const std::vector<uint8_t>& frame, size_t sample, unsigned bytes)
{
    if (bytes == 1)
        return frame[sample];
    return (static_cast<unsigned>(frame[sample * 2]) | (static_cast<unsigned>(frame[sample * 2 + 1]) << 8)) >> 6;
}

// The kernel's code_bits: round to nearest and clamp into the representable
// range. A mix of two codes can leave it — the cubic has negative lobes, so an
// overshoot is ordinary — and an unclamped write would wrap into a completely
// different code rather than saturate.
void set_code(std::vector<uint8_t>& frame, size_t sample, unsigned bytes, double value)
{
    const long top = bytes == 2 ? 1023 : 255;
    const long code = std::min(std::max(std::lround(value), 0L), top);
    const unsigned stored = bytes == 2 ? static_cast<unsigned>(code) << 6 : static_cast<unsigned>(code);
    frame[sample * bytes] = static_cast<uint8_t>(stored & 0xff);
    if (bytes == 2)
        frame[sample * 2 + 1] = static_cast<uint8_t>(stored >> 8);
}

struct Mix {
    std::vector<uint8_t> main;
    // The overlay frame: three planes packed as NV12/P010, or four bytes per
    // pixel when `rgb` is set.
    std::vector<uint8_t> overlay;
    unsigned width = 0, height = 0;
    unsigned overlay_width = 0, overlay_height = 0;
    // The rectangle of the overlay surface the layer samples, and the
    // rectangle of the output it is drawn into.
    unsigned ov_x = 0, ov_y = 0, ov_crop_w = 0, ov_crop_h = 0;
    unsigned x = 0, y = 0, rect_width = 0, rect_height = 0;
    unsigned bytes = 1;
    double alpha = 1.0;
    bool rgb = false;
    bool bgra = true;
    bool premultiplied = false;
    // The destination's range: false limited (16..235 luma), true full
    // (0..255). It decides the encoded output and the neutral code a
    // premultiplied source sits on.
    bool full = false;
    // 0 the point map, 1 the four-tap blend, 4 the driver's per-axis pick.
    unsigned interp = 0;
    // 0 BT.709, 1 BT.601, 2 BT.2020: the matrix the RGB kernel converts with.
    unsigned standard = 0;
};

double mixed(double m, double o, double alpha)
{
    return alpha * o + (1.0 - alpha) * m;
}

// The mix around the plane's neutral code: a premultiplied source already
// carries its alpha, so only the main term is scaled and the offset comes off
// it. The kernel does the same in overlay.comp.
double mixed_codes(const Mix& mix, double m, double o, double neutral)
{
    if (mix.premultiplied)
        return o + (1.0 - mix.alpha) * (m - neutral);
    return mixed(m, o, mix.alpha);
}

// The method one axis actually runs, as both overlay kernels resolve it: the
// client's own, or the driver's pick when it asked for "advanced".
unsigned axis_method(const Mix& mix, unsigned src, unsigned dst)
{
    if (mix.interp != 4u)
        return mix.interp;
    if (src > dst)
        return 2u;
    if (src < dst)
        return 3u;
    return 1u;
}

// Up to four (index, weight) taps for one axis of the overlay's region, the
// same model the scale and overlay kernels run: one tap for the point map,
// every covered source sample for the area average, two for the four-tap
// blend and four Catmull-Rom taps for the cubic.
int axis_taps(const Mix& mix, unsigned d, unsigned dst, unsigned src, int idx[4], double wgt[4])
{
    const unsigned m = axis_method(mix, src, dst);
    if (m == 0u) {
        const unsigned inc = ((src << 16) + (dst >> 1)) / dst;
        idx[0] = static_cast<int>((d * inc + (inc >> 1)) >> 16);
        wgt[0] = 1.0;
        return 1;
    }
    if (m == 2u) {
        unsigned a = (d * src) / dst;
        unsigned b = ((d + 1u) * src + dst - 1u) / dst;
        if (b <= a)
            b = a + 1u;
        b = std::min(b, src);
        a = std::min(a, b - 1u);
        const unsigned n = b - a;
        if (n <= 4u) {
            for (int i = 0; i < 4; ++i) {
                idx[i] = static_cast<int>(std::min(a + static_cast<unsigned>(i), b - 1u));
                wgt[i] = 1.0 / n;
            }
            return static_cast<int>(n);
        }
        for (int i = 0; i < 4; ++i) {
            idx[i] = static_cast<int>(a + (static_cast<unsigned>(i) * (n - 1u)) / 3u);
            wgt[i] = 0.25;
        }
        return 4;
    }
    const double v = std::min(std::max((d + 0.5) * (static_cast<double>(src) / dst) - 0.5, 0.0),
        static_cast<double>(src - 1u));
    const unsigned i0 = static_cast<unsigned>(std::floor(v));
    const double f = v - i0;
    if (m == 1u) {
        idx[0] = static_cast<int>(i0);
        idx[1] = static_cast<int>(std::min(i0 + 1u, src - 1u));
        wgt[0] = 1.0 - f;
        wgt[1] = f;
        return 2;
    }
    const double f2 = f * f, f3 = f2 * f;
    wgt[0] = -0.5 * f3 + f2 - 0.5 * f;
    wgt[1] = 1.5 * f3 - 2.5 * f2 + 1.0;
    wgt[2] = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    wgt[3] = 0.5 * f3 - 0.5 * f2;
    for (int i = 0; i < 4; ++i)
        idx[i] = std::min(std::max(static_cast<int>(i0) - 1 + i, 0), static_cast<int>(src) - 1);
    return 4;
}

// One overlay code from one plane, at destination offset (dx, dy) inside the
// output rectangle. The chroma plane is half scale on both axes, so its
// region, its origin and its sample positions are too.
double overlay_code(const Mix& mix, unsigned plane, unsigned dx, unsigned dy, unsigned comp)
{
    const unsigned dst_w = plane ? mix.rect_width / 2 : mix.rect_width;
    const unsigned dst_h = plane ? mix.rect_height / 2 : mix.rect_height;
    const unsigned src_w = plane ? mix.ov_crop_w / 2 : mix.ov_crop_w;
    const unsigned src_h = plane ? mix.ov_crop_h / 2 : mix.ov_crop_h;
    const unsigned ox = plane ? mix.ov_x / 2 : mix.ov_x;
    const unsigned oy = plane ? mix.ov_y / 2 : mix.ov_y;
    int xi[4], yi[4];
    double xw[4], yw[4];
    const int nx = axis_taps(mix, dx, dst_w, src_w, xi, xw);
    const int ny = axis_taps(mix, dy, dst_h, src_h, yi, yw);
    const size_t luma = static_cast<size_t>(mix.overlay_width) * mix.overlay_height;
    double v = 0.0;
    for (int j = 0; j < ny; ++j) {
        double row = 0.0;
        for (int i = 0; i < nx; ++i) {
            const size_t at = plane
                ? luma
                    + static_cast<size_t>(oy + yi[j]) * mix.overlay_width + (ox + xi[i]) * 2 + comp
                : static_cast<size_t>(oy + yi[j]) * mix.overlay_width + (ox + xi[i]);
            row += xw[i] * code_at(mix.overlay, at, mix.bytes);
        }
        v += yw[j] * row;
    }
    return v;
}

// A premultiplied Y'CbCr frame: every sample moves toward its plane's neutral
// by the global alpha, which is the data VA_BLEND_PREMULTIPLIED_ALPHA names.
void premultiply_pattern(std::vector<uint8_t>& frame, unsigned width, unsigned height, unsigned bytes,
    double alpha)
{
    const double luma = bytes == 2 ? 64.0 : 16.0;
    const double chroma = bytes == 2 ? 512.0 : 128.0;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y) * width + x;
            set_code(frame, i, bytes, luma + alpha * (code_at(frame, i, bytes) - luma));
        }
    for (unsigned y = 0; y < height / 2; ++y)
        for (unsigned x = 0; x < width / 2; ++x) {
            const size_t cell = static_cast<size_t>(width) * height + static_cast<size_t>(y) * width + x * 2;
            for (unsigned k = 0; k < 2; ++k)
                set_code(frame, cell + k, bytes, chroma + alpha * (code_at(frame, cell + k, bytes) - chroma));
        }
}

// One layer mixed onto `out` in place. `out` is both the main term and the
// destination, which is what makes a stack work: the driver reads the output
// back between dispatches, so layer i sees layers 0..i-1 where a single-layer
// picture sees the main surface.
void apply_composite(const Mix& mix, std::vector<uint8_t>& out)
{
    const double luma = mix.full ? 0.0 : (mix.bytes == 2 ? 64.0 : 16.0);
    const double chroma = mix.bytes == 2 ? 512.0 : 128.0;
    for (unsigned dy = 0; dy < mix.rect_height; ++dy)
        for (unsigned dx = 0; dx < mix.rect_width; ++dx) {
            const size_t m = static_cast<size_t>(mix.y + dy) * mix.width + mix.x + dx;
            const double v = mixed_codes(mix, code_at(out, m, mix.bytes),
                overlay_code(mix, 0u, dx, dy, 0u), luma);
            set_code(out, m, mix.bytes, v);
        }
    // Chroma cells cover 2x2 luma pixels, and the rectangle is even, so a cell
    // is inside it exactly when its own coordinates are.
    for (unsigned cy = 0; cy < mix.rect_height / 2; ++cy)
        for (unsigned cx = 0; cx < mix.rect_width / 2; ++cx) {
            const size_t m = static_cast<size_t>(mix.width) * mix.height
                + static_cast<size_t>(mix.y / 2 + cy) * mix.width + (mix.x / 2 + cx) * 2;
            for (unsigned k = 0; k < 2; ++k) {
                const double v = mixed_codes(mix, code_at(out, m + k, mix.bytes),
                    overlay_code(mix, 1u, cx, cy, k), chroma);
                set_code(out, m + k, mix.bytes, v);
            }
        }
}

// A packed RGB overlay that differs on every row, column and channel. The
// alpha byte is a coverage the client means, and VA scopes it to
// VA_BLEND_PREMULTIPLIED_ALPHA: a picture that names that flag has its colours
// already multiplied by the same coverage, so `premultiply` writes each
// channel scaled by the byte the pixel carries. `alpha_byte` overrides the
// per-pixel byte with one constant, which is what makes a premultiplied frame
// provably the same picture as a straight one at the matching global alpha.
void make_rgb_pattern(unsigned width, unsigned height, bool bgra, bool premultiply, int alpha_byte,
    std::vector<uint8_t>& frame)
{
    frame.assign(static_cast<size_t>(width) * height * 4, 0);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            double rgb[3] = { static_cast<double>((x * 3 + y * 5 + 7) & 0xff) / 255.0,
                static_cast<double>((x * 11 + y * 7 + 40) & 0xff) / 255.0,
                static_cast<double>((x * 5 + y * 13 + 90) & 0xff) / 255.0 };
            const uint8_t a = alpha_byte >= 0 ? static_cast<uint8_t>(alpha_byte)
                                              : static_cast<uint8_t>((x * 17 + y * 3 + 200) & 0xff);
            if (premultiply)
                for (double& c : rgb)
                    c = static_cast<double>(std::lround(c * a)) / 255.0;
            uint8_t* p = &frame[(static_cast<size_t>(y) * width + x) * 4];
            const uint8_t r = static_cast<uint8_t>(std::lround(rgb[0] * 255.0));
            const uint8_t g = static_cast<uint8_t>(std::lround(rgb[1] * 255.0));
            const uint8_t b = static_cast<uint8_t>(std::lround(rgb[2] * 255.0));
            if (bgra) {
                p[0] = b;
                p[1] = g;
                p[2] = r;
            } else {
                p[0] = r;
                p[1] = g;
                p[2] = b;
            }
            p[3] = a;
        }
}

// The RGB pixel at (px, py) as 0..1 channels.
void rgb_at(const Mix& mix, unsigned px, unsigned py, double rgb[3])
{
    const uint8_t* p = &mix.overlay[(static_cast<size_t>(py) * mix.overlay_width + px) * 4];
    rgb[0] = (mix.bgra ? p[2] : p[0]) / 255.0;
    rgb[1] = p[1] / 255.0;
    rgb[2] = (mix.bgra ? p[0] : p[2]) / 255.0;
}

// The conversion overlay_rgb.comp documents, in double precision: normalised
// luma and the two chroma offsets for the picture's matrix.
void to_ycbcr(const Mix& mix, const double rgb[3], double out[3])
{
    double kr = 0.2126, kb = 0.0722;
    if (mix.standard == 1) {
        kr = 0.299;
        kb = 0.114;
    } else if (mix.standard == 2) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const double y = kr * rgb[0] + (1.0 - kr - kb) * rgb[1] + kb * rgb[2];
    out[0] = y;
    out[1] = (rgb[2] - y) / (2.0 * (1.0 - kb));
    out[2] = (rgb[0] - y) / (2.0 * (1.0 - kr));
}

// One channel normalised onto the destination's own scale: `neutral` is the
// code that carries zero (16 luma, 128 chroma for NV12; 64 and 512 for the
// 10-bit range), `span` the distance to 1.0.
double normalise(unsigned code, double neutral, double span)
{
    return (static_cast<double>(code) - neutral) / span;
}

// The constants for the destination's bit depth and range: zero and the
// distance to 1.0 on each plane. A full-range picture has no 16..235 window,
// and its chroma midpoint is the same code as the limited one.
double luma_zero(const Mix& mix)
{
    return mix.full ? 0.0 : (mix.bytes == 2 ? 64.0 : 16.0);
}

double luma_span(const Mix& mix)
{
    return mix.full ? (mix.bytes == 2 ? 1023.0 : 255.0) : (mix.bytes == 2 ? 876.0 : 219.0);
}

double chroma_zero(const Mix& mix)
{
    return mix.bytes == 2 ? 512.0 : 128.0;
}

double chroma_span(const Mix& mix)
{
    return mix.full ? (mix.bytes == 2 ? 1023.0 : 255.0) : (mix.bytes == 2 ? 896.0 : 224.0);
}

// The normalised mix at one pixel. `a` is the overlay sample's own alpha byte,
// which is 1.0 unless the picture is premultiplied. A premultiplied colour
// already carries that byte but not the global alpha, so the layer's coverage
// is the product and the overlay is scaled by the global alpha alone.
double mixed_norm(const Mix& mix, double main_norm, double overlay_norm, double a)
{
    const double layer = mix.alpha * a;
    if (mix.premultiplied)
        return mix.alpha * overlay_norm + (1.0 - layer) * main_norm;
    return layer * overlay_norm + (1.0 - layer) * main_norm;
}

// One overlay sample of the packed surface at destination offset (dx, dy)
// inside the output rectangle: the weighted colour and the weighted alpha, the
// same outer product overlay_rgb.comp evaluates.
void sample_rgb(const Mix& mix, unsigned dx, unsigned dy, double rgb[3], double* alpha)
{
    int xi[4], yi[4];
    double xw[4], yw[4];
    const int nx = axis_taps(mix, dx, mix.rect_width, mix.ov_crop_w, xi, xw);
    const int ny = axis_taps(mix, dy, mix.rect_height, mix.ov_crop_h, yi, yw);
    double sum[3] = { 0.0, 0.0, 0.0 };
    double asum = 0.0;
    for (int j = 0; j < ny; ++j) {
        double row[3] = { 0.0, 0.0, 0.0 };
        double arow = 0.0;
        for (int i = 0; i < nx; ++i) {
            const unsigned px = mix.ov_x + static_cast<unsigned>(xi[i]);
            const unsigned py = mix.ov_y + static_cast<unsigned>(yi[j]);
            double one[3];
            rgb_at(mix, px, py, one);
            for (unsigned c = 0; c < 3; ++c)
                row[c] += xw[i] * one[c];
            // VA scopes the alpha byte to VA_BLEND_PREMULTIPLIED_ALPHA, so a
            // straight-alpha picture carries the global alpha alone.
            const uint8_t byte = mix.overlay[(static_cast<size_t>(py) * mix.overlay_width + px) * 4 + 3];
            arow += xw[i] * (mix.premultiplied ? byte / 255.0 : 1.0);
        }
        for (unsigned c = 0; c < 3; ++c)
            sum[c] += yw[j] * row[c];
        asum += yw[j] * arow;
    }
    for (unsigned c = 0; c < 3; ++c)
        rgb[c] = sum[c];
    *alpha = std::min(std::max(asum, 0.0), 1.0);
}

// The same in-place mix for a packed RGB overlay; `out` carries the pixels the
// previous layer left, as the driver's output buffer does.
void apply_composite_rgb(const Mix& mix, std::vector<uint8_t>& out)
{
    for (unsigned dy = 0; dy < mix.rect_height; ++dy)
        for (unsigned dx = 0; dx < mix.rect_width; ++dx) {
            const size_t m = static_cast<size_t>(mix.y + dy) * mix.width + mix.x + dx;
            double rgb[3], ycbcr[3], a = 1.0;
            sample_rgb(mix, dx, dy, rgb, &a);
            to_ycbcr(mix, rgb, ycbcr);
            const double v = mixed_norm(mix,
                normalise(code_at(out, m, mix.bytes), luma_zero(mix), luma_span(mix)), ycbcr[0], a);
            set_code(out, m, mix.bytes, luma_zero(mix) + luma_span(mix) * v);
        }
    // Chroma: the rectangle is even, so a cell is inside it exactly when its
    // own half-scale coordinates are. The cell covers a 2x2 luma block, so its
    // overlay sample is the block's mean, alpha included.
    const unsigned x0 = mix.x / 2, y0 = mix.y / 2;
    for (unsigned cy = 0; cy < mix.rect_height / 2; ++cy)
        for (unsigned cx = 0; cx < mix.rect_width / 2; ++cx) {
            const size_t m = static_cast<size_t>(mix.width) * mix.height
                + static_cast<size_t>(y0 + cy) * mix.width + (x0 + cx) * 2;
            double rgb[3] = { 0.0, 0.0, 0.0 }, ycbcr[3], a = 0.0;
            for (unsigned j = 0; j < 2; ++j)
                for (unsigned i = 0; i < 2; ++i) {
                    double one[3], oa = 1.0;
                    sample_rgb(mix, cx * 2 + i, cy * 2 + j, one, &oa);
                    for (unsigned c = 0; c < 3; ++c)
                        rgb[c] += one[c];
                    a += oa;
                }
            for (unsigned c = 0; c < 3; ++c)
                rgb[c] *= 0.25;
            a *= 0.25;
            to_ycbcr(mix, rgb, ycbcr);
            for (unsigned k = 0; k < 2; ++k) {
                const double v = mixed_norm(mix,
                    normalise(code_at(out, m + k, mix.bytes), chroma_zero(mix), chroma_span(mix)),
                    ycbcr[1 + k], a);
                set_code(out, m + k, mix.bytes, chroma_zero(mix) + chroma_span(mix) * v);
            }
        }
}

void overlay_usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s overlay <render-node> <W> <H> <ovW> <ovH> <x> <y> <rw> <rh> <alpha> <out.raw>\n"
        "                 [--p010] [--premultiplied] [--luma-key] [--layers=N] [--blend-first]\n"
        "                 [--main-crop] [--pad=<columns>] [--tonemap=<peak-nits>]\n"
        "                 [--rgb[=rgba]] [--standard=601|709|2020] [--full] [--alpha-byte=<n>]\n"
        "                 [--crop=<w>x<h>+<x>+<y>]\n"
        "                 [--interp=nearest|bilinear|advanced] [--expect-refusal]\n",
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
    bool p010 = false, premultiplied = false, luma_key = false, blend_first = false;
    bool main_crop = false, expect_refusal = false, full_range = false;
    // The overlay surface is packed BGRA (default with --rgb) or RGBA.
    bool rgb = false, bgra = true;
    unsigned standard = 0;
    unsigned pad = 0;
    double tonemap_peak = 0.0;
    // The overlay's own region inside its surface, and the interpolation
    // method that samples it: 0 the point map, 1 the four-tap blend, 4 the
    // driver's per-axis pick.
    unsigned ov_crop_w = 0, ov_crop_h = 0, ov_crop_x = 0, ov_crop_y = 0, interp = 0;
    bool ov_crop_set = false;
    // The overlay's per-pixel alpha byte: -1 keeps the pattern's own.
    int alpha_byte = -1;
    // How many blending passes to submit over the main one. Each is the same
    // overlay surface into a rectangle one step down and to the right, so a
    // stack is distinguishable from a single layer: the model composites them
    // in submission order, exactly as the driver does.
    unsigned layers = 1;
    for (int i = 11; i < argc; ++i) {
        if (std::strcmp(argv[i], "--p010") == 0)
            p010 = true;
        else if (std::strcmp(argv[i], "--rgb") == 0)
            rgb = true;
        else if (std::strcmp(argv[i], "--rgb=rgba") == 0) {
            rgb = true;
            bgra = false;
        } else if (std::strncmp(argv[i], "--standard=", 11) == 0) {
            const char* name = argv[i] + 11;
            standard = std::strcmp(name, "601") == 0 ? 1 : std::strcmp(name, "2020") == 0 ? 2 : 0;
            if (std::strcmp(name, "601") != 0 && std::strcmp(name, "709") != 0
                && std::strcmp(name, "2020") != 0) {
                overlay_usage("iris-va-vpp-probe");
                return 2;
            }
        }
        else if (std::strncmp(argv[i], "--tonemap=", 10) == 0)
            tonemap_peak = std::atof(argv[i] + 10);
        else if (std::strcmp(argv[i], "--premultiplied") == 0)
            premultiplied = true;
        else if (std::strcmp(argv[i], "--luma-key") == 0)
            luma_key = true;
        else if (std::strcmp(argv[i], "--blend-first") == 0)
            blend_first = true;
        else if (std::strcmp(argv[i], "--main-crop") == 0)
            main_crop = true;
        else if (std::strncmp(argv[i], "--pad=", 6) == 0)
            pad = static_cast<unsigned>(std::atoi(argv[i] + 6));
        else if (std::strcmp(argv[i], "--full") == 0)
            full_range = true;
        else if (std::strncmp(argv[i], "--alpha-byte=", 13) == 0)
            alpha_byte = std::atoi(argv[i] + 13);
        else if (std::strncmp(argv[i], "--crop=", 7) == 0) {
            if (std::sscanf(argv[i] + 7, "%ux%u+%u+%u", &ov_crop_w, &ov_crop_h, &ov_crop_x, &ov_crop_y) != 4) {
                overlay_usage("iris-va-vpp-probe");
                return 2;
            }
            ov_crop_set = true;
        } else if (std::strncmp(argv[i], "--interp=", 9) == 0) {
            const char* name = argv[i] + 9;
            interp = std::strcmp(name, "nearest") == 0    ? 0u
                : std::strcmp(name, "bilinear") == 0      ? 1u
                : std::strcmp(name, "advanced") == 0      ? 4u
                                                          : 99u;
            if (interp == 99u) {
                overlay_usage("iris-va-vpp-probe");
                return 2;
            }
        }
        else if (std::strncmp(argv[i], "--layers=", 9) == 0) {
            layers = static_cast<unsigned>(std::atoi(argv[i] + 9));
            // The driver's limit is sixteen blending passes; a larger count is
            // the driver's to refuse, not the probe's, so it is submitted.
            if (layers < 1 || layers > 64) {
                std::fprintf(stderr, "--layers must be 1..64\n");
                return 2;
            }
        }
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
    // `--tonemap` runs the HDR10 filter on the main pass: the main surface is
    // P010, the overlay and the output stay NV12, and the model converts the
    // main frame before mixing it.
    if (tonemap_peak > 0.0 && (p010 || pad)) {
        std::fprintf(stderr, "--tonemap takes an NV12 output and no padded main surface\n");
        return 2;
    }
    const unsigned main_bytes = tonemap_peak > 0.0 ? 2 : bytes;
    const unsigned main_rt = tonemap_peak > 0.0 ? VA_RT_FORMAT_YUV420_10 : rt_format;
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
    mix.rgb = rgb;
    mix.bgra = bgra;
    mix.premultiplied = premultiplied;
    mix.standard = standard;
    mix.full = full_range;
    mix.interp = interp;
    mix.ov_x = ov_crop_x;
    mix.ov_y = ov_crop_y;
    mix.ov_crop_w = ov_crop_set ? ov_crop_w : ov_w;
    mix.ov_crop_h = ov_crop_set ? ov_crop_h : ov_h;
    // The main surface is `pad` columns wider than the picture, as a decoder
    // allocates it; the picture is the left `width` columns.
    std::vector<uint8_t> main_surface_frame;
    if (tonemap_peak > 0.0) {
        make_hdr10_frame(width, height, tonemap_peak, main_surface_frame);
        expected_sdr(main_surface_frame, width, height, tonemap_peak, mix.main);
    } else {
        make_pattern(width + pad, height, bytes, 1, main_surface_frame);
        if (pad)
            crop_left(main_surface_frame, width + pad, width, height, bytes, mix.main);
        else
            mix.main = main_surface_frame;
    }
    if (rgb)
        // A premultiplied frame stores each colour already multiplied by the
        // coverage its alpha byte carries, which is the data
        // VA_BLEND_PREMULTIPLIED_ALPHA names.
        make_rgb_pattern(ov_w, ov_h, bgra, premultiplied, alpha_byte, mix.overlay);
    else {
        make_pattern(ov_w, ov_h, bytes, 2, mix.overlay);
        if (premultiplied)
            premultiply_pattern(mix.overlay, ov_w, ov_h, bytes, alpha);
    }

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
    check(vaCreateSurfaces(display, main_rt, width + pad, height, &main_surface, 1, nullptr, 0)
            == VA_STATUS_SUCCESS,
        "create the main surface");
    // A packed overlay is a VA_RT_FORMAT_RGB32 surface, and the byte order the
    // client wants is the pixel-format attribute.
    VASurfaceAttrib pixel_format = {};
    pixel_format.type = VASurfaceAttribPixelFormat;
    pixel_format.flags = VA_SURFACE_ATTRIB_SETTABLE;
    pixel_format.value.type = VAGenericValueTypeInteger;
    pixel_format.value.value.i = static_cast<int>(bgra ? VA_FOURCC_BGRA : VA_FOURCC_RGBA);
    check(vaCreateSurfaces(display, rgb ? VA_RT_FORMAT_RGB32 : rt_format, ov_w, ov_h, &overlay_surface, 1,
              rgb ? &pixel_format : nullptr, rgb ? 1 : 0)
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
        // The stored-alpha form of the same mix is implemented; luma keying,
        // which is a different kind of masking, is not.
        check((caps.blend_flags & VA_BLEND_PREMULTIPLIED_ALPHA) != 0,
            "the pipeline caps offer premultiplied alpha");
        check((caps.blend_flags & VA_BLEND_LUMA_KEY) == 0, "no luma key is advertised");
    }

    if (main_surface != VA_INVALID_ID)
        check(upload(display, main_surface, main_surface_frame, width + pad, height, main_bytes),
            "upload the main frame");
    if (overlay_surface != VA_INVALID_ID)
        check(upload(display, overlay_surface, mix.overlay, ov_w, ov_h, rgb ? 4u : bytes),
            "upload the overlay frame");

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
    const VAProcColorStandardType named = standard == 1   ? VAProcColorStandardBT601
        : standard == 2                                   ? VAProcColorStandardBT2020
                                                          : VAProcColorStandardBT709;
    main_params.surface_color_standard = named;
    main_params.output_color_standard = named;
    VABufferID filter_buffer = VA_INVALID_ID;
    if (tonemap_peak > 0.0) {
        VAHdrMetaDataHDR10 metadata = {};
        metadata.display_primaries_x[0] = 13250;
        metadata.display_primaries_y[0] = 34500;
        metadata.display_primaries_x[1] = 7500;
        metadata.display_primaries_y[1] = 3000;
        metadata.display_primaries_x[2] = 34000;
        metadata.display_primaries_y[2] = 16000;
        metadata.white_point_x = 15635;
        metadata.white_point_y = 16450;
        metadata.max_display_mastering_luminance =
            static_cast<uint32_t>(std::lround(tonemap_peak * 10000.0));
        metadata.min_display_mastering_luminance = 50;
        VAProcFilterParameterBufferHDRToneMapping params = {};
        params.type = VAProcFilterHighDynamicRangeToneMapping;
        params.data.metadata_type = VAProcHighDynamicRangeMetadataHDR10;
        params.data.metadata = &metadata;
        params.data.metadata_size = sizeof(metadata);
        check(vaCreateBuffer(display, context, VAProcFilterParameterBufferType, sizeof(params), 1, &params,
                  &filter_buffer)
                == VA_STATUS_SUCCESS,
            "create the HDR10 filter buffer");
        main_params.num_filters = 1;
        main_params.filters = &filter_buffer;
        main_params.surface_color_standard = VAProcColorStandardBT2020;
        main_params.output_color_standard = VAProcColorStandardBT709;
    }
    VAProcPipelineParameterBuffer overlay_params = main_params;
    overlay_params.surface = overlay_surface;
    // The blending pass carries no filter of its own; the conversion belongs
    // to the pass that reads the HDR10 surface.
    overlay_params.num_filters = 0;
    overlay_params.filters = nullptr;
    // FFmpeg clears the overlay's region, which means the whole surface; a
    // client that names one composites part of it, and the driver scales that
    // rectangle into the output region.
    VARectangle ov_region = {};
    ov_region.x = static_cast<int16_t>(mix.ov_x);
    ov_region.y = static_cast<int16_t>(mix.ov_y);
    ov_region.width = static_cast<uint16_t>(mix.ov_crop_w);
    ov_region.height = static_cast<uint16_t>(mix.ov_crop_h);
    overlay_params.surface_region = ov_crop_set ? &ov_region : nullptr;
    overlay_params.output_region = &region;
    overlay_params.blend_state = &blend;
    // The method that samples the overlay is the blending pass's own, and the
    // destination's range is the output properties' — the same picture a
    // client would submit.
    overlay_params.filter_flags = interp == 0u ? 0u
        : interp == 1u                         ? static_cast<unsigned>(VA_FILTER_INTERPOLATION_BILINEAR)
                                               : static_cast<unsigned>(VA_FILTER_INTERPOLATION_ADVANCED);
    if (full_range) {
        // FFmpeg builds the blending pass by copying the main one, so both
        // carry the same colour properties; the driver requires that.
        overlay_params.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        overlay_params.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        main_params.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        main_params.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    }

    // One pipeline buffer per layer, each blending a rectangle one step down
    // and right of the one before, so a stack is a picture no single layer
    // produces. The step is two pixels, which keeps every rectangle even and
    // every chroma cell wholly inside or outside its layer. A rectangle the
    // caller put outside the output is passed through unchanged: refusing it
    // is the driver's job, and clamping here would hide that.
    std::vector<VAProcPipelineParameterBuffer> layer_params(layers, overlay_params);
    std::vector<VARectangle> layer_regions(layers);
    for (unsigned i = 0; i < layers; ++i) {
        VARectangle& r = layer_regions[i];
        r.x = static_cast<int16_t>(x + 2u * i);
        r.y = static_cast<int16_t>(y + 2u * i);
        r.width = static_cast<uint16_t>(rw);
        r.height = static_cast<uint16_t>(rh);
        layer_params[i].output_region = &r;
    }

    VABufferID main_buffer = VA_INVALID_ID;
    check(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(main_params), 1,
              &main_params, &main_buffer)
            == VA_STATUS_SUCCESS,
        "create the main pipeline buffer");
    std::vector<VABufferID> layer_buffers(layers, VA_INVALID_ID);
    for (unsigned i = 0; i < layers; ++i)
        check(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(layer_params[i]), 1,
                  &layer_params[i], &layer_buffers[i])
                == VA_STATUS_SUCCESS,
            "create a blending pipeline buffer");

    VAStatus begin = vaBeginPicture(display, context, dst_surface);
    std::vector<VABufferID> order;
    order.push_back(main_buffer);
    for (VABufferID id : layer_buffers)
        order.push_back(id);
    if (blend_first)
        std::swap(order[0], order[1]);
    if (filter_buffer != VA_INVALID_ID)
        order.push_back(filter_buffer);
    VAStatus render = begin == VA_STATUS_SUCCESS
        ? vaRenderPicture(display, context, order.data(), static_cast<int>(order.size()))
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
            // One mix per layer, in submission order, each reading what the
            // one before it left — the driver reads its own output back
            // between dispatches, so this is the same picture.
            std::vector<uint8_t> expected = mix.main;
            for (unsigned i = 0; i < layers; ++i) {
                Mix step = mix;
                step.x = layer_regions[i].x;
                step.y = layer_regions[i].y;
                step.rect_width = layer_regions[i].width;
                step.rect_height = layer_regions[i].height;
                if (rgb)
                    apply_composite_rgb(step, expected);
                else
                    apply_composite(step, expected);
            }
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
            // The RGB path converts per sample before it mixes, so its model
            // is longer than the YUV one and keeps the tone mapper's slack.
            check(max_diff <= (tonemap_peak > 0.0 || rgb ? 2u : 1u),
                "every byte within the documented mix's tolerance");
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

    // The interpolation method and the refusal switch are options anywhere on
    // the command line; the twelve arguments before them are positional.
    unsigned filter_flags = 0;
    const char* interp = nullptr;
    bool expect_refusal = false;
    for (int i = 1; i < argc; i++) {
        // Options are scanned, not positionally assumed: an option that only
        // counts when it lands in the right slot is an option that silently
        // stops being passed once a second one is added.
        if (std::strcmp(argv[i], "--expect-refusal") == 0)
            expect_refusal = true;
        else if (std::strncmp(argv[i], "--interp=", 9) == 0) {
            interp = argv[i] + 9;
            if (std::strcmp(interp, "nearest") == 0)
                filter_flags = VA_FILTER_INTERPOLATION_NEAREST_NEIGHBOR;
            else if (std::strcmp(interp, "bilinear") == 0)
                filter_flags = VA_FILTER_INTERPOLATION_BILINEAR;
            else if (std::strcmp(interp, "advanced") == 0)
                filter_flags = VA_FILTER_INTERPOLATION_ADVANCED;
            else {
                std::fprintf(stderr, "--interp must be nearest, bilinear or advanced\n");
                return 2;
            }
        }
    }
    if (argc != 12 && !expect_refusal && interp == nullptr) {
        std::fprintf(stderr, "usage: %s <render-node> <src.raw> <srcW> <srcH> <cropW> <cropH> <cropX> "
                             "<cropY> <dstW> <dstH> <out.raw> [--expect-refusal] "
                             "[--interp=nearest|bilinear|advanced]\n",
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

    // What the caps advertise is the driver's claim; the picture below is what
    // it actually runs. Checking both here keeps the two from drifting apart:
    // a method in the caps without a kernel is over-advertising, and a method
    // with a kernel but not in the caps is invisible to a client that asks.
    {
        VAProcPipelineCaps caps = {};
        check(vaQueryVideoProcPipelineCaps(display, context, nullptr, 0, &caps) == VA_STATUS_SUCCESS,
            "query the VideoProc pipeline caps");
        // filter_flags carries one interpolation value, not a set of bits:
        // nearest | bilinear is the advanced value, so "both bits set" would
        // mean the opposite of what it looks like.
        check((caps.filter_flags & VA_FILTER_INTERPOLATION_MASK) == VA_FILTER_INTERPOLATION_ADVANCED,
            "the caps name advanced interpolation, the best method the driver runs");
    }

    VAProcPipelineParameterBuffer pipeline = {};
    pipeline.surface = src_surface;
    pipeline.surface_region = &region;
    pipeline.filter_flags = filter_flags;
    VABufferID pipeline_buffer = VA_INVALID_ID;
    status = vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(pipeline), 1, &pipeline,
        &pipeline_buffer);
    check(status == VA_STATUS_SUCCESS, "create the pipeline buffer");

    check(vaBeginPicture(display, context, dst_surface) == VA_STATUS_SUCCESS, "vaBeginPicture");
    // A refusal can land at either call: the pipeline buffer is judged when it
    // is rendered, and anything that needs both passes is judged at the end.
    // Counting only the end would report a driver that refused correctly as a
    // driver that ran the picture.
    const VAStatus render_status = vaRenderPicture(display, context, &pipeline_buffer, 1);
    status = vaEndPicture(display, context);

    if (expect_refusal) {
        const bool refused = status != VA_STATUS_SUCCESS || render_status != VA_STATUS_SUCCESS;
        std::printf("%s the picture was refused (render 0x%x, end 0x%x)\n", refused ? "PASS" : "FAIL",
            static_cast<unsigned>(render_status), static_cast<unsigned>(status));
        if (!refused)
            ++failures;
    } else {
        check(render_status == VA_STATUS_SUCCESS, "vaRenderPicture");
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
        std::printf("PASS iris-va-vpp-probe crop=%ux%u+%d+%d -> %ux%u interp=%s\n", region.width, region.height,
            region.x, region.y, dst_w, dst_h, interp ? interp : "default");
    return failures ? 1 : 0;
}
