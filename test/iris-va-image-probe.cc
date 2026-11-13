// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): the two ways a client fills a VA surface
// with pixels it owns, and the one way it reads them back.
//
//   - vaCreateImage + vaPutImage, in whole frames and in regions;
//   - vaDeriveImage, map, write, unmap (the path FFmpeg's hwupload takes);
//   - vaGetImage afterwards, which used to refuse with "surface has no
//     decoded pixels" because nothing had decoded into it.
//
// Usage: iris-va-image-probe <render-node>
//
// The negative cases are part of the contract: vaPutImage cannot scale, and
// a 4:2:0 upload region must be even.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
}

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

constexpr unsigned kWidth = 64;
constexpr unsigned kHeight = 64;

// A per-byte sequence: two different fills cannot alias, and a half-written
// buffer changes the comparison.
void fill(std::vector<uint8_t>& bytes, uint8_t seed)
{
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(seed + (i * 7) % 251);
}

// Map an image buffer, hand its bytes to `body`, unmap.
template <typename Body> bool with_image(VADisplay display, VAImage& image, Body&& body)
{
    void* mapped = nullptr;
    if (vaMapBuffer(display, image.buf, &mapped) != VA_STATUS_SUCCESS)
        return false;
    body(static_cast<uint8_t*>(mapped), image.data_size);
    vaUnmapBuffer(display, image.buf);
    return true;
}

// Packed NV12 rows of a frame, as a VAImage of the same geometry lays them
// out, so the plane offsets of a derived view can be driven from it.
void write_view(uint8_t* base, const VAImage& view, const std::vector<uint8_t>& frame, unsigned width,
    unsigned height)
{
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(base + view.offsets[0] + static_cast<size_t>(y) * view.pitches[0],
            frame.data() + static_cast<size_t>(y) * width, width);
    for (unsigned y = 0; y < (height + 1) / 2; ++y)
        std::memcpy(base + view.offsets[1] + static_cast<size_t>(y) * view.pitches[1],
            frame.data() + static_cast<size_t>(width) * height + static_cast<size_t>(y) * width, width);
}

VAImage make_image(VADisplay display)
{
    VAImageFormat format = {};
    format.fourcc = VA_FOURCC_NV12;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
    VAImage image = {};
    if (vaCreateImage(display, &format, kWidth, kHeight, &image) != VA_STATUS_SUCCESS)
        image.image_id = VA_INVALID_ID;
    return image;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <render-node>\n", argv[0]);
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
        std::fprintf(stderr, "vaInitialize failed\n");
        return 2;
    }

    VASurfaceID surface = VA_INVALID_ID;
    if (vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, kWidth, kHeight, &surface, 1, nullptr, 0)
        != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "vaCreateSurfaces failed\n");
        return 2;
    }

    std::vector<uint8_t> source(kWidth * kHeight * 3 / 2);
    fill(source, 0x20);

    // 1. A whole frame through vaCreateImage + vaPutImage, read back with
    //    vaGetImage. Before vaPutImage existed this returned UNIMPLEMENTED.
    VAImage uploaded = make_image(display);
    const bool uploaded_ok = uploaded.image_id != VA_INVALID_ID
        && with_image(display, uploaded, [&](uint8_t* data, size_t size) {
               std::memcpy(data, source.data(), std::min<size_t>(size, source.size()));
           });
    check(uploaded_ok, "createImage");
    VAStatus status = vaPutImage(display, surface, uploaded.image_id, 0, 0, kWidth, kHeight, 0, 0, kWidth,
        kHeight);
    check(status == VA_STATUS_SUCCESS, "vaPutImage uploads a full NV12 frame");

    VAImage fetched = make_image(display);
    status = vaGetImage(display, surface, 0, 0, kWidth, kHeight, fetched.image_id);
    check(status == VA_STATUS_SUCCESS, "vaGetImage reads back a client-written surface");
    bool same = false;
    if (status == VA_STATUS_SUCCESS)
        with_image(display, fetched, [&](uint8_t* data, size_t size) {
            same = std::memcmp(data, source.data(), std::min<size_t>(size, source.size())) == 0;
        });
    check(same, "vaPutImage -> vaGetImage round trip is byte-identical");

    // 2. A region: the 32x32 block at (16,16), with the rest left as the
    //    first upload left it. A driver that copies the whole image, or
    //    ignores dx/dy, fails here.
    std::vector<uint8_t> patch(kWidth * kHeight * 3 / 2, 0);
    fill(patch, 0x80);
    VAImage region = make_image(display);
    const bool region_ok = region.image_id != VA_INVALID_ID
        && with_image(display, region, [&](uint8_t* data, size_t size) {
               std::memcpy(data, patch.data(), std::min<size_t>(size, patch.size()));
           });
    check(region_ok, "createImage for a region upload");
    status = vaPutImage(display, surface, region.image_id, 16, 16, 32, 32, 16, 16, 32, 32);
    check(status == VA_STATUS_SUCCESS, "vaPutImage uploads a region");
    std::vector<uint8_t> after(kWidth * kHeight * 3 / 2);
    status = vaGetImage(display, surface, 0, 0, kWidth, kHeight, fetched.image_id);
    if (status == VA_STATUS_SUCCESS)
        with_image(display, fetched, [&](uint8_t* data, size_t size) {
            std::memcpy(after.data(), data, std::min<size_t>(size, after.size()));
        });
    bool region_moved = status == VA_STATUS_SUCCESS;
    for (unsigned y = 16; region_moved && y < 48; ++y)
        for (unsigned x = 16; x < 48; ++x)
            region_moved = after[y * kWidth + x] == patch[y * kWidth + x];
    // 4:2:0 chroma: the 16x16 chroma block at (8,8) covers the same pixels.
    for (unsigned y = 8; region_moved && y < 24; ++y)
        for (unsigned x = 8; x < 24; ++x)
            region_moved = after[kWidth * kHeight + y * kWidth + x]
                == patch[kWidth * kHeight + y * kWidth + x];
    check(region_moved, "the region landed where dx/dy asked");
    bool rest_kept = status == VA_STATUS_SUCCESS;
    for (unsigned y = 0; rest_kept && y < 16; ++y)
        rest_kept = after[y * kWidth + 0] == source[y * kWidth + 0]
            && after[y * kWidth + kWidth - 1] == source[y * kWidth + kWidth - 1];
    check(rest_kept, "pixels outside the region are unchanged");

    // 3. The upload path FFmpeg uses: derive, write through the mapping, and
    //    read back without any vaPutImage. This is the one that failed with
    //    "surface has no decoded pixels".
    VASurfaceID derived_surface = VA_INVALID_ID;
    vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, kWidth, kHeight, &derived_surface, 1, nullptr, 0);
    std::vector<uint8_t> written(kWidth * kHeight * 3 / 2);
    fill(written, 0x40);
    VAImage view = {};
    status = vaDeriveImage(display, derived_surface, &view);
    check(status == VA_STATUS_SUCCESS, "vaDeriveImage on a surface that never decoded");
    if (status == VA_STATUS_SUCCESS) {
        with_image(display, view, [&](uint8_t* data, size_t) { write_view(data, view, written, kWidth, kHeight); });
        vaDestroyImage(display, view.image_id);
    }
    status = vaGetImage(display, derived_surface, 0, 0, kWidth, kHeight, fetched.image_id);
    check(status == VA_STATUS_SUCCESS, "vaGetImage after a derived-image write");
    bool derived_read = false;
    if (status == VA_STATUS_SUCCESS)
        with_image(display, fetched, [&](uint8_t* data, size_t size) {
            derived_read = std::memcmp(data, written.data(), std::min<size_t>(size, written.size())) == 0;
        });
    check(derived_read, "derived-image write -> vaGetImage is byte-identical");

    // 4. Refusals.
    status = vaPutImage(display, surface, uploaded.image_id, 0, 0, 32, 32, 0, 0, 64, 64);
    check(status == VA_STATUS_ERROR_UNIMPLEMENTED, "vaPutImage refuses to scale");
    status = vaPutImage(display, surface, uploaded.image_id, 1, 0, 32, 32, 0, 0, 32, 32);
    check(status == VA_STATUS_ERROR_INVALID_PARAMETER, "vaPutImage refuses an odd 4:2:0 offset");
    status = vaPutImage(display, surface, uploaded.image_id, 0, 0, kWidth, 32, 0, 1, kWidth, 32);
    check(status == VA_STATUS_ERROR_INVALID_PARAMETER, "vaPutImage refuses a region past the surface");

    vaDestroySurfaces(display, &derived_surface, 1);
    vaDestroySurfaces(display, &surface, 1);
    vaDestroyImage(display, uploaded.image_id);
    vaDestroyImage(display, region.image_id);
    vaDestroyImage(display, fetched.image_id);
    vaTerminate(display);
    close(fd);

    if (failures)
        std::printf("FAIL iris-va-image-probe %d check(s)\n", failures);
    else
        std::printf("PASS iris-va-image-probe: putImage, region upload and the derived-image path\n");
    return failures ? 1 : 0;
}
