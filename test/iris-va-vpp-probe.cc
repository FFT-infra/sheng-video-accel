// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): one VideoProc picture, driven the way a
// client drives it, with a surface region on the input.
//
//   iris-va-vpp-probe <render-node> <src.raw> <srcW> <srcH> \
//                     <cropW> <cropH> <cropX> <cropY> <dstW> <dstH> <out.raw> \
//                     [--expect-refusal]
//
// It uploads the NV12 frame through a derived image, creates a
// VAProfileNone / VAEntrypointVideoProc context, submits a
// VAProcPipelineParameterBuffer whose surface_region is the crop, ends the
// picture and downloads the result with vaGetImage. FFmpeg 8.1 has no
// crop_vaapi filter, so this is the only client that can ask for a region.
//
// `--expect-refusal` inverts the verdict: the picture must fail (an odd or
// out-of-bounds region is the driver's to refuse, not to round).
//
// Needs V4L2_VA_VPP=1, LIBVA_DRIVER_NAME=v4l2 and either the installed driver
// or LIBVA_DRIVERS_PATH pointing at a build tree.

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// Packed NV12 rows into the surface's own layout, through a derived image.
bool upload(VADisplay display, VASurfaceID surface, const std::vector<uint8_t>& frame, unsigned width,
    unsigned height)
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
            frame.data() + static_cast<size_t>(y) * width, width);
    for (unsigned y = 0; y < (height + 1) / 2; ++y)
        std::memcpy(base + image.offsets[1] + static_cast<size_t>(y) * image.pitches[1],
            frame.data() + static_cast<size_t>(width) * height + static_cast<size_t>(y) * width, width);
    vaUnmapBuffer(display, image.buf);
    vaDestroyImage(display, image.image_id);
    return true;
}

// The surface's visible pixels as packed NV12 rows.
bool download(VADisplay display, VASurfaceID surface, std::vector<uint8_t>& frame, unsigned width,
    unsigned height)
{
    VAImageFormat format = {};
    format.fourcc = VA_FOURCC_NV12;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
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
    frame.resize(static_cast<size_t>(width) * height * 3 / 2);
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(frame.data() + static_cast<size_t>(y) * width,
            base + image.offsets[0] + static_cast<size_t>(y) * image.pitches[0], width);
    for (unsigned y = 0; y < (height + 1) / 2; ++y)
        std::memcpy(frame.data() + static_cast<size_t>(width) * height + static_cast<size_t>(y) * width,
            base + image.offsets[1] + static_cast<size_t>(y) * image.pitches[1], width);
    vaUnmapBuffer(display, image.buf);
    vaDestroyImage(display, image.image_id);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
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
    check(upload(display, src_surface, source, src_w, src_h), "upload the source frame");

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
            check(download(display, dst_surface, output, dst_w, dst_h), "download the processed frame");
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
