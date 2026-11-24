// SPDX-License-Identifier: MIT
//
// Hardware probe (built, not a registered meson test): a VA client can create
// an encode misc parameter buffer whose element size is smaller than the
// header the driver reads. renderPicture must refuse it with
// VA_STATUS_ERROR_INVALID_PARAMETER instead of reading past the client's
// bytes. The unit tests link the whole VA glue but cannot open an encoder
// context on a host, so this runs against the target's real nodes.
//
// Without the bounds check, a 1-byte element read a garbage `type` out of the
// neighbouring heap (VA_STATUS_ERROR_UNIMPLEMENTED) and a header-only
// QualityLevel element read a garbage `quality_level` that happened to be
// <= 1 and passed silently. Both are out-of-bounds reads, not refusals.
//
// Usage: iris-encode-misc-bound-probe [render-node]
//   default /dev/dri/renderD128.
//   A build tree needs LIBVA_DRIVER_NAME=v4l2 with LIBVA_DRIVERS_PATH, or
//   libva resolves msm_drv_video.so instead of v4l2_drv_video.so:
//     env LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH=/path/to/build/src
//         iris-encode-misc-bound-probe

#include <va/va.h>
#include <va/va_drm.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

constexpr unsigned kWidth = 640;
constexpr unsigned kHeight = 480;

} // namespace

int main(int argc, char** argv)
{
    const char* render = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int fd = open(render, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::printf("FAIL cannot open %s\n", render);
        return 2;
    }
    VADisplay display = vaGetDisplayDRM(fd);
    int major = 0, minor = 0;
    if (vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS)
        return 2;

    VAConfigAttrib attr[2] = {};
    attr[0].type = VAConfigAttribRTFormat;
    attr[0].value = VA_RT_FORMAT_YUV420;
    attr[1].type = VAConfigAttribRateControl;
    attr[1].value = VA_RC_CBR;
    VAConfigID config = VA_INVALID_ID;
    VAStatus s = vaCreateConfig(display, VAProfileH264High, VAEntrypointEncSlice, attr, 2, &config);
    if (s != VA_STATUS_SUCCESS) {
        std::printf("FAIL createConfig status=%d\n", s);
        return 1;
    }

    VASurfaceID surface = VA_INVALID_ID;
    s = vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, kWidth, kHeight, &surface, 1, nullptr, 0);
    if (s != VA_STATUS_SUCCESS) {
        std::printf("FAIL createSurfaces status=%d\n", s);
        return 1;
    }

    // Each case gets its own context: a render that succeeds leaves the
    // picture open, and beginPicture refuses a context already in progress.
    auto submit = [&](unsigned size, VAEncMiscParameterType type) -> VAStatus {
        VAContextID context = VA_INVALID_ID;
        VAStatus st = vaCreateContext(display, config, kWidth, kHeight, 0, &surface, 1, &context);
        if (st != VA_STATUS_SUCCESS)
            return st;
        VAEncMiscParameterBuffer header = {};
        header.type = type;
        unsigned char storage[64] = {};
        std::memcpy(storage, &header, sizeof(header));
        VABufferID buffer = VA_INVALID_ID;
        st = vaCreateBuffer(display, context, VAEncMiscParameterBufferType, size, 1,
            storage, &buffer);
        if (st == VA_STATUS_SUCCESS) {
            st = vaBeginPicture(display, context, surface);
            if (st == VA_STATUS_SUCCESS)
                st = vaRenderPicture(display, context, &buffer, 1);
            vaDestroyBuffer(display, buffer);
        }
        vaDestroyContext(display, context);
        return st;
    };

    // size=1 cannot hold VAEncMiscParameterBuffer::type (4 bytes).
    VAStatus small = submit(1, VAEncMiscParameterTypeFrameRate);
    std::printf("size=1 status=%d (want %d)\n", small, VA_STATUS_ERROR_INVALID_PARAMETER);
    check(small == VA_STATUS_ERROR_INVALID_PARAMETER, "a 1-byte misc element is refused");

    // A QualityLevel element holding the header but not its quality_level
    // field: the second bounds check.
    VAStatus header_only = submit(sizeof(VAEncMiscParameterBuffer), VAEncMiscParameterTypeQualityLevel);
    std::printf("header-only quality status=%d (want %d)\n", header_only,
        VA_STATUS_ERROR_INVALID_PARAMETER);
    check(header_only == VA_STATUS_ERROR_INVALID_PARAMETER,
        "a header-only QualityLevel element is refused");

    // A full, valid QualityLevel element still passes into the encoder.
    // Status 0 means begin+render succeeded; endPicture is not submitted in
    // this probe, so no firmware work is claimed.
    VAStatus full = submit(sizeof(VAEncMiscParameterBuffer) + sizeof(uint32_t),
        VAEncMiscParameterTypeQualityLevel);
    std::printf("full quality status=%d (want 0)\n", full);
    check(full == VA_STATUS_SUCCESS, "a full QualityLevel element still opens a picture");

    vaDestroySurfaces(display, &surface, 1);
    vaDestroyConfig(display, config);
    vaTerminate(display);
    close(fd);
    return failures ? 1 : 0;
}
