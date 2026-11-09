// SPDX-License-Identifier: MIT
//
// Hardware probe: does the encoder wait for an input surface's pending GPU
// write before handing it to the firmware? docs/architecture.md §10
// invariant 11 says it does; this is the run that has to back that up.
//
// Three NV12 stable buffers, all with the same chroma and different luma:
//
//   A  "current" — the picture the client means to encode.
//   C  "stale"   — the picture it is replacing.
//   B  "staged"  — starts as a CPU copy of C. The GPU then rewrites C over
//                  B many times (content unchanged, the queue stays busy)
//                  and finally blits A into B. The picture is submitted
//                  right after glFlush(), with B's fence still pending.
//
// B's memory therefore says which side of the fence the firmware read from:
// before the blits land it is C, after them it is A. An encoder that waits
// produces A's access unit; one that queues the fd and lets the firmware
// read early produces C's. The probe asserts:
//
//   - the fence is pending when the picture is submitted, and still pending
//     when it returned, so the window was open for the whole submit;
//   - the access unit from B equals the one from A;
//   - the one from C differs, so the comparison above can fail.
//
// The last two points are why the filler blits exist: a single blit of A
// over C drained before the submit could be queued, and then the byte
// comparison passed whether or not the driver waited.
//
// Usage: iris-encode-fence-probe <encoder-node> <render-node> [width height]
//        IRIS_FENCE_BLITS=<n>  filler count (default 512)

#include "../src/iris/alloc.h"
#include "../src/iris/device.h"
#include "../src/iris/encode.h"

#include "../src/util/error.h"
#include "../src/util/options.h"
#include "../src/util/trace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <va/va.h>
}

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

// 1 signaled, 0 pending, -1 no fence could be exported.
int fence_state(int fd)
{
    dma_buf_export_sync_file request = {};
    request.flags = DMA_BUF_SYNC_WRITE;
    if (ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request) != 0)
        return -1;
    pollfd fence = { request.fd, POLLIN, 0 };
    const int result = ::poll(&fence, 1, 0);
    close(request.fd);
    if (result < 0)
        return -1;
    return result > 0 ? 1 : 0;
}

void write_plane(iris::StableBuffer& buffer, unsigned offset, unsigned stride, unsigned rows, unsigned row_bytes,
    uint8_t seed)
{
    uint8_t* base = buffer.map();
    for (unsigned row = 0; row < rows; ++row)
        for (unsigned column = 0; column < row_bytes; ++column)
            base[offset + static_cast<size_t>(row) * stride + column] = static_cast<uint8_t>(seed + row + column * 7);
}

// A and C differ only in luma, so a luma-only blit makes B identical to A.
void fill(iris::StableBuffer& buffer, uint8_t luma_seed)
{
    const iris::Layout& layout = buffer.layout();
    write_plane(buffer, layout.data_offset, layout.stride, layout.height, layout.width, luma_seed);
    const unsigned chroma_rows = (layout.height + 1) / 2;
    write_plane(buffer, layout.chroma_offset(), layout.stride, chroma_rows, layout.width & ~1u, 200);
    iris::dma_buf_cpu_sync(buffer.fd(), DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
    iris::dma_buf_cpu_sync(buffer.fd(), DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
}

struct Gl {
    int fd = -1;
    gbm_device* gbm = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image = nullptr;
    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;

    bool init(int render_fd)
    {
        fd = fcntl(render_fd, F_DUPFD_CLOEXEC, 0);
        if (fd < 0)
            return false;
        gbm = gbm_create_device(fd);
        auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        bind_image = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!gbm || !get_display || !create_image || !destroy_image || !bind_image)
            return false;
        display = get_display(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
            return false;
        if (!eglBindAPI(EGL_OPENGL_ES_API))
            return false;
        const EGLint attributes[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2,
            EGL_NONE };
        context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
        if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
            return false;
        const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        return renderer && !std::strstr(renderer, "llvmpipe") && !std::strstr(renderer, "softpipe");
    }

    ~Gl()
    {
        if (context != EGL_NO_CONTEXT) {
            glFinish();
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display, context);
        }
        if (display != EGL_NO_DISPLAY)
            eglTerminate(display);
        if (gbm)
            gbm_device_destroy(gbm);
        if (fd >= 0)
            close(fd);
    }
};

// The luma plane as packed RGBA8, the same import gpu_copy.cc builds.
GLuint make_luma_texture(Gl& gl, const iris::StableBuffer& buffer)
{
    const iris::Layout& layout = buffer.layout();
    const EGLint attributes[] = {
        EGL_WIDTH, static_cast<EGLint>((layout.width + 3) / 4),
        EGL_HEIGHT, static_cast<EGLint>(layout.height),
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, buffer.fd(),
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(layout.stride),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE,
    };
    const EGLImageKHR image = gl.create_image(gl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
    if (image == EGL_NO_IMAGE_KHR)
        return 0;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.bind_image(GL_TEXTURE_2D, image);
    return glGetError() == GL_NO_ERROR ? texture : 0;
}

iris::EncodeParams intra_params(uint32_t codec)
{
    iris::EncodeParams params;
    params.profile = codec == V4L2_PIX_FMT_HEVC ? static_cast<int>(V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN)
                                                : static_cast<int>(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
    params.rate_mode = iris::RateMode::Vbr;
    params.bitrate = 4000000;
    params.gop_size = 0; // intra only: one picture per session, no reference
    params.frame_rate_num = 30;
    params.frame_rate_den = 1;
    return params;
}

// One intra picture from its own session: no picture can predict from
// another, so identical pixels must give identical bytes. The session is
// configured before `before_picture` runs, so the V4L2 setup cannot absorb
// the time a pending fence would spend waiting: what happens between the
// flush and the QBUF is only the submit itself.
std::vector<uint8_t> encode_configured(
    const std::string& node, uint32_t codec, iris::StableBuffer& buffer, unsigned width, unsigned height,
    const std::function<void()>& before_picture, long long* submit_ms)
{
    auto device = iris::open_device(node, iris::NodeRole::Encoder);
    iris::EncodeConfig config;
    config.codec = codec;
    config.width = width;
    config.height = height;
    iris::EncoderSession session(std::move(device), config, iris::Options {}, iris::Trace(false));
    const iris::EncodeParams params = intra_params(codec);
    session.configure(params, width, height);
    before_picture();
    const auto start = std::chrono::steady_clock::now();
    const iris::EncodeResult result = session.encode(buffer.fd(), buffer.size(), params);
    if (submit_ms)
        *submit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    session.finish();
    return result.bytes;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 5) {
        std::fprintf(stderr, "usage: %s <encoder-node> <render-node> [width height]\n", argv[0]);
        return 2;
    }
    const std::string encoder_node = argv[1];
    const unsigned width = argc == 5 ? static_cast<unsigned>(std::atoi(argv[3])) : 1920;
    const unsigned height = argc == 5 ? static_cast<unsigned>(std::atoi(argv[4])) : 1080;

    const int render_fd = open(argv[2], O_RDWR | O_CLOEXEC);
    if (render_fd < 0) {
        std::perror("open render node");
        return 2;
    }
    Gl gl;
    if (!gl.init(render_fd)) {
        std::printf("FAIL no hardware EGL renderer on this node\n");
        return 1;
    }

    const iris::Layout layout = iris::stable_layout(VA_FOURCC_NV12, width, height);
    std::unique_ptr<iris::StableBuffer> current, staged, stale;
    try {
        current = iris::allocate_stable(render_fd, layout);
        staged = iris::allocate_stable(render_fd, layout);
        stale = iris::allocate_stable(render_fd, layout);
    } catch (const iris::Error& error) {
        std::printf("FAIL allocate_stable status=%#x: %s\n", error.status, error.what());
        return 1;
    }

    // `current` holds the picture the client means to encode, `stale` the one
    // it is replacing. `staged` starts as a copy of `stale` and becomes
    // `current` only when the last GPU blit lands, so its memory says which
    // side of the fence the firmware read from.
    fill(*current, 0);
    fill(*stale, 90);
    fill(*staged, 90);

    const GLuint current_texture = make_luma_texture(gl, *current);
    const GLuint stale_texture = make_luma_texture(gl, *stale);
    const GLuint staged_texture = make_luma_texture(gl, *staged);
    if (!current_texture || !stale_texture || !staged_texture) {
        std::printf("FAIL EGL import of the stable buffers\n");
        return 1;
    }

    std::vector<uint8_t> from_current, from_staged, from_stale;
    try {
        from_current = encode_configured(encoder_node, V4L2_PIX_FMT_H264, *current, width, height, [] {}, nullptr);
        from_stale = encode_configured(encoder_node, V4L2_PIX_FMT_H264, *stale, width, height, [] {}, nullptr);
    } catch (const iris::Error& error) {
        std::printf("FAIL reference encode status=%#x: %s\n", error.status, error.what());
        return 1;
    }
    check(!from_current.empty() && !from_stale.empty(), "the reference pictures encode");
    check(from_current != from_stale, "the two luma patterns encode to different access units");

    int pending = -1;
    int pending_after_wait = -1;
    long long submit_ms = 0;
    const unsigned columns = (width + 3) / 4;
    const unsigned blit_repeats
        = std::max(1u, static_cast<unsigned>(std::atoi(std::getenv("IRIS_FENCE_BLITS") ? std::getenv("IRIS_FENCE_BLITS") : "512")));
    try {
        // The blit runs inside the same function that then submits, after the
        // session is configured and streaming: nothing but the fence wait
        // stands between the glFlush() and the QBUF, so this is the window a
        // client actually creates.
        from_staged = encode_configured(encoder_node, V4L2_PIX_FMT_H264, *staged, width, height,
            [&] {
                // The filler blits rewrite the stale picture over itself, so
                // the destination's content says nothing until the queue
                // drains. The last blit is the one that makes it `current`:
                // memory read before the fence still shows the stale pixels,
                // and the encode below is only correct if the driver waited.
                for (unsigned repeat = 0; repeat < blit_repeats; ++repeat)
                    glCopyImageSubData(stale_texture, GL_TEXTURE_2D, 0, 0, 0, 0, staged_texture, GL_TEXTURE_2D,
                        0, 0, 0, 0, static_cast<GLsizei>(columns), static_cast<GLsizei>(height), 1);
                glCopyImageSubData(current_texture, GL_TEXTURE_2D, 0, 0, 0, 0, staged_texture, GL_TEXTURE_2D, 0,
                    0, 0, 0, static_cast<GLsizei>(columns), static_cast<GLsizei>(height), 1);
                glFlush();
                pending = fence_state(staged->fd());
            },
            &submit_ms);
        pending_after_wait = fence_state(staged->fd());
    } catch (const iris::Error& error) {
        std::printf("FAIL encode through the pending fence status=%#x: %s\n", error.status, error.what());
        return 1;
    }

    std::printf("fence at submit: %s, after submit: %s\n", pending == 1 ? "signaled" : pending == 0 ? "pending" : "none",
        pending_after_wait == 1 ? "signaled" : pending_after_wait == 0 ? "pending" : "none");
    // Both halves matter: a fence that was already signaled at the submit
    // would let the byte comparison pass without any wait, and one that is
    // still pending after the submit returned means the wait was skipped.
    check(pending == 0, "the blit's write fence is pending when the picture is submitted");
    check(pending_after_wait == 1, "the fence is retired by the time submit returns");

    std::printf("encoded %zu / %zu / %zu bytes, staged round trip %lldms\n", from_current.size(),
        from_staged.size(), from_stale.size(), submit_ms);
    check(!from_staged.empty(), "the picture behind the fence encodes");
    check(from_staged == from_current, "the surface is encoded as the GPU left it, not as it held before the blit");

    close(render_fd);
    std::printf("%s encode-fence %ux%u blits=%u staged=%zu current=%zu\n", failures ? "FAIL" : "PASS", width, height,
        blit_repeats, from_staged.size(), from_current.size());
    return failures ? 1 : 0;
}
