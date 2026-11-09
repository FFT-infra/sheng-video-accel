// SPDX-License-Identifier: MIT
//
// Is a GPU job's fence still pending on the target buffer right after
// glFlush()? The encode path hands a client's surface to the firmware, and
// the client may have rendered into it with the GPU. Whether that write has
// landed is visible only through the DMA-BUF reservation object, so this
// probe measures it directly: it imports two stable buffers into EGL, blits
// one into the other, and exports the destination's pending fence as a
// sync_file (DMA_BUF_IOCTL_EXPORT_SYNC_FILE) at two points:
//
//   - right after glFlush(), no glFinish: the fence must still be pending;
//   - after glFinish(): the fence must be signaled.
//
// Usage: gpu-fence-check <render-node> [width height]
//
// A pending fence at the first point is what makes the encode input wait
// meaningful: without it the firmware can read pixels the GPU has not
// written yet. If this probe reports the fence already signaled after
// glFlush, the window is too small to observe on this GPU and the encode
// probe below cannot claim a wait it did not see.

#include "../src/iris/alloc.h"
#include "../src/util/error.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
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

// Whether the buffer's exported fence is signaled right now.
// Returns 1 signaled, 0 pending, -1 no fence could be exported.
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

struct Gl {
    int fd = -1;
    gbm_device* gbm = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image = nullptr;
    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
    bool ok = false;

    bool init(int render_fd)
    {
        fd = fcntl(render_fd, F_DUPFD_CLOEXEC, 0);
        if (fd < 0)
            return false;
        gbm = gbm_create_device(fd);
        if (!gbm)
            return false;
        auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        bind_image = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!get_display || !create_image || !destroy_image || !bind_image)
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
        if (!renderer || std::strstr(renderer, "llvmpipe") || std::strstr(renderer, "softpipe"))
            return false;
        ok = true;
        return true;
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

struct View {
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
};

// Packed RGBA8 over the luma plane, the same import gpu_copy.cc builds.
bool make_view(Gl& gl, const iris::StableBuffer& buffer, View& view)
{
    const iris::Layout& layout = buffer.layout();
    const unsigned columns = (layout.width + 3) / 4;
    const unsigned rows = layout.height;
    const EGLint attributes[] = {
        EGL_WIDTH, static_cast<EGLint>(columns),
        EGL_HEIGHT, static_cast<EGLint>(rows),
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, buffer.fd(),
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(layout.stride),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE,
    };
    view.image = gl.create_image(gl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
    if (view.image == EGL_NO_IMAGE_KHR)
        return false;
    glGenTextures(1, &view.texture);
    glBindTexture(GL_TEXTURE_2D, view.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.bind_image(GL_TEXTURE_2D, view.image);
    return glGetError() == GL_NO_ERROR;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 4) {
        std::fprintf(stderr, "usage: %s <render-node> [width height]\n", argv[0]);
        return 2;
    }
    const unsigned width = argc == 4 ? static_cast<unsigned>(std::atoi(argv[2])) : 1920;
    const unsigned height = argc == 4 ? static_cast<unsigned>(std::atoi(argv[3])) : 1080;

    const int drm = open(argv[1], O_RDWR | O_CLOEXEC);
    if (drm < 0) {
        std::perror("open render node");
        return 2;
    }
    Gl gl;
    if (!gl.init(drm)) {
        std::printf("FAIL no hardware EGL renderer on this node\n");
        return 1;
    }

    const iris::Layout layout = iris::stable_layout(VA_FOURCC_NV12, width, height);
    std::unique_ptr<iris::StableBuffer> source, destination;
    try {
        source = iris::allocate_stable(drm, layout);
        destination = iris::allocate_stable(drm, layout);
    } catch (const iris::Error& error) {
        std::printf("FAIL allocate_stable status=%#x: %s\n", error.status, error.what());
        return 1;
    }

    check(fence_state(destination->fd()) == 1, "an untouched buffer has no pending fence");

    View src, dst;
    if (!make_view(gl, *source, src) || !make_view(gl, *destination, dst)) {
        std::printf("FAIL EGL import of the stable buffers\n");
        return 1;
    }

    // Enough work that the GPU queue does not drain before the flush returns.
    const unsigned columns = (width + 3) / 4;
    for (unsigned repeat = 0; repeat < 8; ++repeat)
        glCopyImageSubData(src.texture, GL_TEXTURE_2D, 0, 0, 0, 0, dst.texture, GL_TEXTURE_2D, 0, 0, 0, 0,
            static_cast<GLsizei>(columns), static_cast<GLsizei>(height), 1);
    glFlush();

    const int flushed = fence_state(destination->fd());
    std::printf("fence after glFlush: %s\n", flushed == 1 ? "signaled" : flushed == 0 ? "pending" : "none");
    check(flushed == 0, "glFlush leaves the destination fence pending");

    glFinish();
    const int finished = fence_state(destination->fd());
    std::printf("fence after glFinish: %s\n", finished == 1 ? "signaled" : finished == 0 ? "pending" : "none");
    check(finished == 1, "glFinish retires the destination fence");

    check(glGetError() == GL_NO_ERROR, "the GPU copy reported no error");

    close(drm);
    std::printf("%s gpu-fence %ux%u\n", failures ? "FAIL" : "PASS", width, height);
    return failures ? 1 : 0;
}
