// SPDX-License-Identifier: MIT
//
// GPU blit between two DMA-BUFs on Adreno. Both buffers are imported as
// packed RGBA8 EGLImages (four raw bytes per texel, no YUV conversion) and
// copied with glCopyImageSubData. All EGL and GL calls, including teardown,
// happen on one private worker thread with a private GBM display: VA entry
// points may arrive on any thread of a GPU process with its own GL context
// current, and that must never be disturbed.

#include "publish.h"

#include "../util/error.h"

#include <algorithm>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <thread>

extern "C" {
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <libdrm/drm_fourcc.h>
#include <poll.h>
#include <unistd.h>
}

namespace iris {

namespace {

class GpuBlitter {
public:
    explicit GpuBlitter(int render_fd)
    {
        try {
            fd_ = fcntl(render_fd, F_DUPFD_CLOEXEC, 0);
            require(fd_ >= 0, "gpu copy: duplicate render fd");
            gbm_ = gbm_create_device(fd_);
            require(gbm_ != nullptr, "gpu copy: gbm device");
            auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
                eglGetProcAddress("eglGetPlatformDisplayEXT"));
            create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
            destroy_image_
                = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
            bind_image_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
                eglGetProcAddress("glEGLImageTargetTexture2DOES"));
            require(get_display && create_image_ && destroy_image_ && bind_image_, "gpu copy: EGL entry points");
            display_ = get_display(EGL_PLATFORM_GBM_KHR, gbm_, nullptr);
            require(display_ != EGL_NO_DISPLAY && eglInitialize(display_, nullptr, nullptr), "gpu copy: eglInitialize");
            initialized_ = true;
            require(eglBindAPI(EGL_OPENGL_ES_API), "gpu copy: eglBindAPI");
            const EGLint attributes[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2,
                EGL_NONE };
            context_ = eglCreateContext(display_, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
            require(context_ != EGL_NO_CONTEXT && eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_),
                "gpu copy: GLES 3.2 context");
            const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            require(renderer && !std::strstr(renderer, "llvmpipe") && !std::strstr(renderer, "softpipe"),
                "gpu copy: hardware renderer required");
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~GpuBlitter() { cleanup(); }

    void copy(StableBuffer& destination, Frame& source)
    {
        const Layout& from = source.layout();
        const Layout& to = destination.layout();
        require(from.fourcc == to.fourcc && (from.fourcc == VA_FOURCC_NV12 || from.fourcc == VA_FOURCC_P010),
            "gpu copy: format mismatch", VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
        wait_dma_buf(source.fd(), false, 5000);
        wait_dma_buf(destination.fd(), true, 5000);
        // Each view describes its own buffer, and the copy covers what both
        // can hold. copy_pixels() already intersects the client's image
        // rectangle with the buffer's for the CPU path; a client surface can
        // still carry the pre-change geometry when the first picture of a
        // new coded size arrives (FFmpeg's download path reuses its surface
        // pool across a resolution change), so the GPU path has to do the
        // same or it asks for a source rectangle the source does not have.
        View& src = view(source_views_, source.id(), source.fd(), from, source.length(), from.width, from.height);
        View& dst = view(dest_views_, destination.id(), destination.fd(), to, destination.size(), to.width, to.height);
        const unsigned width = std::min(from.width, to.width);
        const unsigned height = std::min(from.height, to.height);
        for (unsigned plane = 0; plane < 2; ++plane) {
            const unsigned bytes = from.fourcc == VA_FOURCC_P010 ? 2 : 1;
            const uint64_t row_bytes = static_cast<uint64_t>(plane ? ((width + 1) & ~1u) : width) * bytes;
            const unsigned columns = static_cast<unsigned>((row_bytes + 3) / 4);
            const unsigned rows = plane ? (height + 1) / 2 : height;
            glCopyImageSubData(src.textures[plane], GL_TEXTURE_2D, 0, 0, 0, 0, dst.textures[plane], GL_TEXTURE_2D, 0, 0,
                0, 0, static_cast<GLsizei>(std::min(columns, src.columns[plane])),
                static_cast<GLsizei>(std::min(rows, src.rows[plane])), 1);
        }
        // The consumer of a persistent export may sample it without any
        // further VA call; the copy must be complete when this returns.
        glFinish();
        require(glGetError() == GL_NO_ERROR, "gpu copy failed; set V4L2_VA_COPY=cpu");
    }

    void forget(uint64_t id)
    {
        source_views_.erase(id);
        dest_views_.erase(id);
    }

private:
    struct View {
        GpuBlitter& owner;
        EGLImageKHR images[2] = { EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR };
        GLuint textures[2] = {};
        unsigned columns[2] = {}, rows[2] = {};
        unsigned width, height;
        View(GpuBlitter& gpu, unsigned w, unsigned h)
            : owner(gpu)
            , width(w)
            , height(h)
        {
        }
        ~View()
        {
            glDeleteTextures(2, textures);
            for (auto image : images)
                if (image != EGL_NO_IMAGE_KHR)
                    owner.destroy_image_(owner.display_, image);
        }
    };
    using Views = std::map<uint64_t, std::unique_ptr<View>>;

    View& view(Views& views, uint64_t id, int fd, const Layout& layout, unsigned size, unsigned width, unsigned height)
    {
        auto it = views.find(id);
        if (it != views.end()) {
            if (it->second->width == width && it->second->height == height)
                return *it->second;
            views.erase(it);
        }
        require(width && height && width <= layout.width && height <= layout.height
                && height <= layout.storage_height && fd >= 0 && layout.stride <= INT_MAX,
            "gpu copy: invalid buffer layout");
        auto v = std::make_unique<View>(*this, width, height);
        const unsigned bytes = layout.fourcc == VA_FOURCC_P010 ? 2 : 1;
        for (unsigned plane = 0; plane < 2; ++plane) {
            const uint64_t row_bytes = static_cast<uint64_t>(plane ? ((width + 1) & ~1u) : width) * bytes;
            const unsigned columns = static_cast<unsigned>((row_bytes + 3) / 4);
            const unsigned rows = plane ? (height + 1) / 2 : height;
            const uint64_t offset = plane ? layout.chroma_offset() : layout.data_offset;
            require(static_cast<uint64_t>(columns) * 4 <= layout.stride && offset <= INT_MAX
                    && offset + static_cast<uint64_t>(rows - 1) * layout.stride + static_cast<uint64_t>(columns) * 4
                        <= size,
                "gpu copy: packed plane exceeds buffer");
            const EGLint attributes[] = {
                EGL_WIDTH, static_cast<EGLint>(columns),
                EGL_HEIGHT, static_cast<EGLint>(rows),
                EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
                EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(offset),
                EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(layout.stride),
                EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
                EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
                EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                EGL_NONE,
            };
            v->images[plane] = create_image_(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
            require(v->images[plane] != EGL_NO_IMAGE_KHR, "gpu copy: DMA-BUF import");
            glGenTextures(1, &v->textures[plane]);
            glBindTexture(GL_TEXTURE_2D, v->textures[plane]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            bind_image_(GL_TEXTURE_2D, v->images[plane]);
            require(glGetError() == GL_NO_ERROR, "gpu copy: texture import");
            v->columns[plane] = columns;
            v->rows[plane] = rows;
        }
        View& result = *v;
        views.emplace(id, std::move(v));
        return result;
    }

    void cleanup() noexcept
    {
        if (context_ != EGL_NO_CONTEXT) {
            glFinish();
            source_views_.clear();
            dest_views_.clear();
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        if (initialized_) {
            eglTerminate(display_);
            initialized_ = false;
        }
        if (gbm_) {
            gbm_device_destroy(gbm_);
            gbm_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        eglReleaseThread();
    }

    int fd_ = -1;
    gbm_device* gbm_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    bool initialized_ = false;
    PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image_ = nullptr;
    Views source_views_;
    Views dest_views_;
};

class GpuCopy final : public CopyEngine {
public:
    explicit GpuCopy(int render_fd)
    {
        std::promise<void> ready;
        auto ready_future = ready.get_future();
        worker_ = std::thread([this, render_fd, ready = std::move(ready)]() mutable {
            std::unique_ptr<GpuBlitter> blitter;
            try {
                blitter = std::make_unique<GpuBlitter>(render_fd);
                ready.set_value();
            } catch (...) {
                ready.set_exception(std::current_exception());
                return;
            }
            for (;;) {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || task_.valid(); });
                if (stop_)
                    break;
                auto task = std::move(task_);
                lock.unlock();
                task(blitter.get());
            }
            // Teardown on the same thread as every other EGL call.
            blitter.reset();
        });
        try {
            ready_future.get();
        } catch (...) {
            worker_.join();
            throw;
        }
    }

    ~GpuCopy() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable())
            worker_.join();
    }

    const char* name() const override { return "gpu"; }

    void copy(StableBuffer& destination, Frame& source) override
    {
        run([&](GpuBlitter* blitter) { blitter->copy(destination, source); });
    }

    void forget(uint64_t id) override
    {
        run([id](GpuBlitter* blitter) { blitter->forget(id); });
    }

private:
    template <typename F> void run(F&& function)
    {
        std::packaged_task<void(GpuBlitter*)> task(std::forward<F>(function));
        auto done = task.get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_ = std::move(task);
        }
        wake_.notify_one();
        done.get();
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::packaged_task<void(GpuBlitter*)> task_;
    bool stop_ = false;
    std::thread worker_;
};

} // namespace

std::unique_ptr<CopyEngine> make_gpu_copy(int render_fd)
{
    if (render_fd < 0)
        return nullptr;
    try {
        return std::make_unique<GpuCopy>(render_fd);
    } catch (...) {
        return nullptr;
    }
}

} // namespace iris
