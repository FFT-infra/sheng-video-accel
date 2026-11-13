// SPDX-License-Identifier: MIT
//
// Surfaces: creation, lifetime, sync, and export. A surface owns at most
// one iris::Target and, once a client has imported it before its first
// decode, one StableBuffer whose fd never changes (architecture §3.3).

#include "driver.h"

#include "../util/error.h"
#include "../util/prime.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <unistd.h>
#include <va/va_drmcommon.h>
}

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

uint32_t fourcc_for(uint32_t rt_format)
{
    if (rt_format & VA_RT_FORMAT_RGB32)
        return VA_FOURCC_BGRA;
    return (rt_format & VA_RT_FORMAT_YUV420_10) ? VA_FOURCC_P010 : VA_FOURCC_NV12;
}

// Blend a settled picture's grain into the job's display buffer. The source
// is the CAPTURE slot a Direct target holds, or the buffer a Copy or Import
// target published into. Returns why it could not, or null when it did.
const char* blend_into(const Surface::GrainJob& job, const iris::Target& source)
{
    const iris::FrameRef& frame = source.frame;
    iris::StableBuffer* from_buffer = frame ? nullptr : source.destination;
    if (!source.completed || source.error || (!frame && !from_buffer))
        return "film grain: the picture it grains did not decode";
    iris::StableBuffer& destination = *job.destination;
    bool blended = false;
    try {
        // A Direct completion's CAPTURE slot is storage-aligned: its layout
        // is the pool's rectangle, not the picture the client's surfaces
        // name. The picture sits at the top left of that storage with the
        // pool stride, so the blend reads it as the display surface's
        // geometry.
        iris::Layout from = frame ? frame->layout() : from_buffer->layout();
        from.width = destination.layout().width;
        from.height = destination.layout().height;
        uint8_t* src = frame ? frame->map() : from_buffer->map();
        const int source_fd = frame ? frame->fd() : from_buffer->fd();
        uint8_t* dst = destination.map();
        // The same coherency the CPU copy engine uses: wait for whoever still
        // reads the destination, then bracket both buffers' CPU access.
        iris::wait_dma_buf(destination.fd(), true, 5000);
        iris::dma_buf_cpu_sync(source_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        iris::dma_buf_cpu_sync(destination.fd(), DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        blended = apply_grain(job.params, from, src, destination.layout(), dst);
        iris::dma_buf_cpu_sync(destination.fd(), DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        iris::dma_buf_cpu_sync(source_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    } catch (const std::exception&) {
        return "film grain: the blend could not map the surfaces";
    }
    if (!blended)
        return "film grain: the picture is not a layout this driver decodes";
    job.trace(
        "va grain %ux%u source=%s", destination.layout().width, destination.layout().height, frame ? "frame" : "stable");
    return nullptr;
}

// Take the job off its display surface and record what it left behind. Only
// this surface's own job is touched: a newer picture may have replaced it.
void settle_grain_job(DriverData& d, const std::shared_ptr<Surface::GrainJob>& job, bool ok)
{
    if (auto display = job->display.lock()) {
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        if (display->grain != job)
            return;
        display->grain.reset();
        display->grain_pixels = ok;
        display->failed = !ok;
    }
}

// Wait for the surface's picture outside the driver mutex, then report.
// `session` is the owner's session captured under the lock (may be null when
// the owner is gone); holding it by value keeps it alive for the wait.
VAStatus wait_for(DriverData& d, std::shared_ptr<iris::Session> session, const std::shared_ptr<Surface>& s)
{
    iris::require(!s->failed, "surface decode failed", VA_STATUS_ERROR_DECODING_ERROR);
    std::shared_ptr<Surface::GrainJob> job;
    {
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        job = s->grain;
    }
    if (job) {
        // endPicture settles a grained picture before it returns, so a job
        // still on the surface means another thread is inside that call. A VA
        // client serializes calls on a surface, and waiting here would have
        // nothing that ends it.
        throw iris::Error(
            VA_STATUS_ERROR_SURFACE_BUSY, "film grain: the display surface's picture has not finished");
    }
    if (!session) {
        // The owner was destroyed between our two reads. destroyContext
        // drains before it drops the context, so the target is settled now.
        iris::require(!s->rendering(), "rendering surface has no live session", VA_STATUS_ERROR_INVALID_SURFACE);
        iris::require(!s->target.error, "surface decode failed", VA_STATUS_ERROR_DECODING_ERROR);
        return VA_STATUS_SUCCESS;
    }
    try {
        // Ask the session even when the target has stopped rendering. A
        // completion another call pumped may have marked it failed, and that
        // failure lives in the target, not in Surface::failed; returning here
        // on `rendering()` alone reported SUCCESS for a surface the firmware
        // rejected.
        session->wait(s->target);
    } catch (const iris::Error& error) {
        s->failed = true;
        d.trace("va sync failed surface status=%d: %s", error.status, error.what());
        throw;
    }
    return VA_STATUS_SUCCESS;
}

std::shared_ptr<iris::Session> session_of(DriverData& d, const Surface& s)
{
    auto owner = owner_of(d, s);
    return owner ? owner->session : nullptr;
}

// The buffer's real length. DMA-BUF llseek reports it on the Iris kernel, so
// a descriptor cannot claim more memory than the object holds; 0 means the
// kernel did not say and the import cannot cross-check.
uint64_t allocation_size(int fd)
{
    const off_t length = lseek(fd, 0, SEEK_END);
    return length > 0 ? static_cast<uint64_t>(length) : 0;
}

// The surface becomes a view over the client's memory: a StableBuffer whose
// fd never changes, marked persistent so the existing Import/Copy policy
// applies unchanged (architecture §3.3). The fd is duplicated here, so the
// client may close its own at any time.
std::unique_ptr<iris::StableBuffer> import_stable(int fd, uint64_t allocation, const iris::PlaneLayout& planes,
    uint32_t fourcc, unsigned width, unsigned height)
{
    const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    iris::require(copy >= 0, "duplicate the imported DMA-BUF", VA_STATUS_ERROR_ALLOCATION_FAILED);
    iris::Layout layout;
    layout.fourcc = fourcc;
    layout.width = width;
    layout.height = height;
    layout.stride = planes.stride;
    layout.storage_height = planes.storage_height;
    layout.data_offset = 0;
    layout.size = planes.size;
    const unsigned size = allocation >= planes.size ? static_cast<unsigned>(allocation) : planes.size;
    return std::make_unique<iris::StableBuffer>(copy, size, iris::MemoryOrigin::Imported, layout);
}

} // namespace

std::shared_ptr<Surface::GrainJob> queue_grain(DriverData& d, Context& c,
    const std::shared_ptr<Surface>& decoded)
{
    if (c.profile != VAProfileAV1Profile0 || !c.picture.parameters)
        return nullptr;
    const auto& picture = *static_cast<const VADecPictureParameterBufferAV1*>(c.picture.parameters);
    const auto& grain = picture.film_grain_info;
    if (!grain.film_grain_info_fields.bits.apply_grain)
        return nullptr;
    const VASurfaceID display_id = picture.current_display_picture;
    iris::require(picture.current_frame == c.picture.surface_id && display_id != VA_INVALID_ID
            && display_id != picture.current_frame,
        "film grain needs the picture on current_frame and a separate display surface",
        VA_STATUS_ERROR_INVALID_PARAMETER);
    const unsigned depth = picture.bit_depth_idx ? 10 : 8;

    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto display = surface(d, display_id);
    iris::require(display->width == decoded->width && display->height == decoded->height
            && display->fourcc == decoded->fourcc
            && display->fourcc == (depth == 10 ? VA_FOURCC_P010 : VA_FOURCC_NV12),
        "film grain: the display surface is not this picture's size and format", VA_STATUS_ERROR_INVALID_SURFACE);
    iris::require(!display->target.pending, "film grain: the display surface is decoding",
        VA_STATUS_ERROR_SURFACE_BUSY);
    auto job = std::make_shared<Surface::GrainJob>();
    // The job names its display surface weakly: the surface owns the job, so
    // a strong pointer back would keep the pair alive forever.
    job->display = display;
    ensure_stable(d, *display);
    job->destination = display->stable;
    job->params = grain_params(grain, depth, picture.matrix_coefficients);
    job->trace = d.trace;
    // Whatever this surface showed before belongs to an older picture.
    display->grain = job;
    display->grain_pixels = false;
    display->failed = false;
    return job;
}

void finish_grain(DriverData& d, const std::shared_ptr<Surface::GrainJob>& job,
    const std::shared_ptr<iris::Session>& session, iris::Target& source)
{
    // FFmpeg decodes into a scratch current_frame that it recycles at the
    // next access unit, so the picture is settled here: the target must be
    // read while it still names this picture, not from a later wait on the
    // display surface.
    const char* failure = nullptr;
    try {
        session->wait(source);
        failure = blend_into(*job, source);
    } catch (...) {
        settle_grain_job(d, job, false);
        throw;
    }
    if (failure) {
        settle_grain_job(d, job, false);
        throw iris::Error(VA_STATUS_ERROR_INVALID_SURFACE, failure);
    }
    settle_grain_job(d, job, true);
}

void abandon_grain(DriverData& d, const std::shared_ptr<Surface::GrainJob>& job)
{
    settle_grain_job(d, job, false);
}

bool producing(DriverData& d, const Surface& surface)
{
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    // A grain job exists only while endPicture is still blending it; after
    // that the surface has pixels no caller has been handed yet.
    return surface.rendering() || surface.grain != nullptr;
}

VAStatus createSurfaces2(VADriverContextP ctx, unsigned format, unsigned width, unsigned height, VASurfaceID* ids,
    unsigned count, VASurfaceAttrib* attributes, unsigned attribute_count)
{
    return guarded(ctx, "createSurfaces", [&] {
        auto& d = data(ctx);
        iris::require(width && height && count, "invalid surface geometry", VA_STATUS_ERROR_INVALID_PARAMETER);
        iris::require(format & (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10 | VA_RT_FORMAT_RGB32),
            "unsupported RT format", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        iris::require(!(format & VA_RT_FORMAT_RGB32) || format == VA_RT_FORMAT_RGB32,
            "RGB32 is the whole format, not a bit beside a YUV one", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        uint32_t fourcc = fourcc_for(format);
        unsigned memory_type = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
        const void* external = nullptr;
        for (unsigned i = 0; i < attribute_count; ++i) {
            const auto& attribute = attributes[i];
            if (!(attribute.flags & VA_SURFACE_ATTRIB_SETTABLE))
                continue;
            switch (attribute.type) {
            case VASurfaceAttribPixelFormat: {
                // A packed surface has two byte orders, so the client picks
                // one; the YUV formats have exactly one fourcc each.
                const uint32_t requested = attribute.value.type == VAGenericValueTypeInteger
                    ? static_cast<uint32_t>(attribute.value.value.i) : 0;
                const bool rgb = format == VA_RT_FORMAT_RGB32;
                iris::require(attribute.value.type == VAGenericValueTypeInteger
                        && (rgb ? (requested == VA_FOURCC_BGRA || requested == VA_FOURCC_RGBA)
                                : requested == fourcc),
                    "surface pixel format does not match the RT format", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
                if (rgb)
                    fourcc = requested;
                break;
            }
            case VASurfaceAttribMemoryType:
                memory_type = static_cast<unsigned>(attribute.value.value.i);
                break;
            case VASurfaceAttribExternalBufferDescriptor:
                iris::require(attribute.value.type == VAGenericValueTypePointer && attribute.value.value.p,
                    "null external buffer descriptor", VA_STATUS_ERROR_INVALID_PARAMETER);
                external = attribute.value.value.p;
                break;
            case VASurfaceAttribUsageHint:
                break;
            default:
                throw iris::Error(VA_STATUS_ERROR_ATTR_NOT_SUPPORTED, "unsupported surface attribute");
            }
        }
        // Client memory (architecture §3.3): the surface is created as a view
        // over one linear DMA-BUF in the CAPTURE layout, and every later
        // decode goes through the Import policy — driver-allocated surfaces
        // are the other case.
        std::unique_ptr<iris::StableBuffer> imported;
        int import_fd = -1;
        if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
            iris::require(external != nullptr, "DRM_PRIME_2 import needs a surface descriptor",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            iris::require(count == 1, "DRM_PRIME_2 import creates one surface per descriptor",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            const auto& descriptor = *static_cast<const VADRMPRIMESurfaceDescriptor*>(external);
            // Do not probe a descriptor whose object count is wrong:
            // objects[0] would be whatever the client left there, possibly an
            // unrelated open fd whose shared file offset allocation_size()
            // would move before the descriptor is refused.
            const int fd = descriptor.num_objects == 1 ? descriptor.objects[0].fd : -1;
            const uint64_t allocation = fd >= 0 ? allocation_size(fd) : 0;
            imported = import_stable(fd, allocation,
                iris::validate_prime2(descriptor, width, height, fourcc, allocation), fourcc, width, height);
        } else if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) {
            iris::require(external != nullptr, "DRM_PRIME import needs a buffer table",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            iris::require(count == 1, "DRM_PRIME import creates one surface per buffer",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            const auto& buffers = *static_cast<const VASurfaceAttribExternalBuffers*>(external);
            const int fd = buffers.num_buffers == 1 && buffers.buffers ? static_cast<int>(buffers.buffers[0]) : -1;
            const uint64_t allocation = fd >= 0 ? allocation_size(fd) : 0;
            imported = import_stable(fd, allocation,
                iris::validate_prime1(buffers, width, height, fourcc, allocation), fourcc, width, height);
        } else {
            iris::require(memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_VA && external == nullptr,
                "unsupported surface memory type", VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
        }
        if (imported)
            import_fd = imported->fd();

        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        for (unsigned i = 0; i < count; ++i) {
            auto s = std::make_shared<Surface>();
            s->width = width;
            s->height = height;
            s->fourcc = fourcc;
            if (imported) {
                s->stable = std::move(imported);
                s->persistent_export = true;
            }
            ids[i] = d.next_id++;
            d.surfaces[ids[i]] = s;
        }
        d.trace("va create_surfaces count=%u %ux%u fourcc=%.4s imported=%d fd=%d", count, width, height,
            reinterpret_cast<const char*>(&fourcc), import_fd >= 0 ? 1 : 0, import_fd);
        return VA_STATUS_SUCCESS;
    });
}

VAStatus createSurfaces(VADriverContextP ctx, int width, int height, int format, int count, VASurfaceID* ids)
{
    return createSurfaces2(ctx, static_cast<unsigned>(format), static_cast<unsigned>(width),
        static_cast<unsigned>(height), ids, static_cast<unsigned>(count), nullptr, 0);
}

VAStatus destroySurfaces(VADriverContextP ctx, VASurfaceID* ids, int count)
{
    return guarded(ctx, "destroySurfaces", [&] {
        auto& d = data(ctx);
        std::vector<std::shared_ptr<Surface>> doomed;
        std::vector<std::shared_ptr<iris::Session>> sessions;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            for (int i = 0; i < count; ++i) {
                auto it = d.surfaces.find(ids[i]);
                if (it == d.surfaces.end())
                    continue;
                iris::require(it->second->acquired_handles == 0, "surface has an acquired handle",
                    VA_STATUS_ERROR_SURFACE_BUSY);
                doomed.push_back(it->second);
                sessions.push_back(session_of(d, *it->second));
                // A synthesis belongs to the picture that is running it, and
                // that picture holds the job; the surface it would land on is
                // being destroyed, so the job is dropped here.
                it->second->grain.reset();
                d.surfaces.erase(it);
            }
        }
        // Release outside the table lock: release() takes the session lock
        // and may complete a deferred reconfigure.
        for (size_t i = 0; i < doomed.size(); ++i) {
            auto& s = doomed[i];
            if (sessions[i])
                sessions[i]->release(s->target);
            if (s->stable && d.copier)
                d.copier->forget(s->stable->id());
            if (s->target.frame && d.copier)
                d.copier->forget(s->target.frame->id());
            s->target.frame.reset();
        }
        return VA_STATUS_SUCCESS;
    });
}

VAStatus syncSurface(VADriverContextP ctx, VASurfaceID id)
{
    return guarded(ctx, "syncSurface", [&] {
        auto& d = data(ctx);
        std::shared_ptr<Surface> s;
        std::shared_ptr<iris::Session> session;
        std::shared_ptr<Context> owner;
        std::optional<Context::EncSubmitted> encoded;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            s = surface(d, id);
            session = session_of(d, *s);
            owner = owner_of(d, *s);
            // An encode context has no decode session: vaSyncSurface means
            // "this surface is free to reuse", which the firmware answers
            // when it has read the picture, before the access unit exists.
            if (owner && owner->encoding()) {
                auto it = owner->enc_submitted.find(id);
                if (it != owner->enc_submitted.end())
                    encoded = it->second;
            }
        }
        if (encoded) {
            // Waiting for the unit here made every client that submits ahead
            // serialise: FFmpeg syncs the input surface per picture. The
            // unit is claimed by the coded buffer's own vaSyncBuffer or
            // vaMapBuffer, which is where the bytes are wanted.
            if (encoded->session)
                encoded->session->wait_input(encoded->ticket);
            return VA_STATUS_SUCCESS;
        }
        return wait_for(d, std::move(session), s);
    });
}

VAStatus syncBuffer(VADriverContextP ctx, VABufferID id, uint64_t timeout_ns)
{
    return guarded(ctx, "syncBuffer", [&] {
        auto& d = data(ctx);
        // FFmpeg's capability probe: vaSyncBuffer(display, VA_INVALID_ID, 0)
        // only asks whether this entry point exists (vaapi_encode.c). Answer
        // that, not "invalid buffer", or every FFmpeg encode falls back to
        // synchronous mode.
        if (id == VA_INVALID_ID)
            return VA_STATUS_SUCCESS;
        std::shared_ptr<Buffer> b;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            b = buffer(d, id);
            // Only a coded buffer has a completion behind it. Anything else
            // is already settled when vaEndPicture returns, which is what a
            // success here means.
            if (b->type != VAEncCodedBufferType)
                return VA_STATUS_SUCCESS;
        }
        // The same claim vaMapBuffer makes, and the same one vaSyncSurface
        // makes on the input surface: whichever of the three arrives first
        // stores the access unit, and the others find it instead of getting a
        // second, empty answer. GStreamer's encoder calls two of them.
        return claim_coded(d, b, timeout_ns);
    });
}

VAStatus querySurfaceStatus(VADriverContextP ctx, VASurfaceID id, VASurfaceStatus* status)
{
    return guarded(ctx, "querySurfaceStatus", [&] {
        auto& d = data(ctx);
        std::shared_ptr<Surface> s;
        std::shared_ptr<iris::Session> session;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            s = surface(d, id);
            session = session_of(d, *s);
        }
        // A client that polls instead of syncing still drives completions.
        if (s->rendering() && session)
            session->service();
        // A grained display surface is owed its pixels until its picture's
        // endPicture has run the blend.
        bool grain_owed = false;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            grain_owed = s->grain != nullptr;
        }
        *status = (s->rendering() || grain_owed) ? VASurfaceRendering : VASurfaceReady;
        return VA_STATUS_SUCCESS;
    });
}

VAStatus querySurfaceAttributes(VADriverContextP ctx, VAConfigID config, VASurfaceAttrib* attributes, unsigned* count)
{
    auto& d = data(ctx);
    std::lock_guard<std::recursive_mutex> lock(d.mutex);
    auto it = d.configs.find(config);
    if (it == d.configs.end())
        return VA_STATUS_ERROR_INVALID_CONFIG;
    VASurfaceAttrib list[10] = {};
    unsigned n = 0;
    auto add = [&](VASurfaceAttribType type, int value, unsigned flags) {
        list[n].type = type;
        list[n].flags = flags;
        list[n].value.type = VAGenericValueTypeInteger;
        list[n].value.value.i = value;
        ++n;
    };
    // FFmpeg creates its config with no RT-format attribute and then picks
    // the surface format from this list by bit depth, so list every format
    // the profile can decode into rather than the one the config defaulted to.
    add(VASurfaceAttribPixelFormat, VA_FOURCC_NV12, VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE);
    const VAProfile profile = it->second.profile;
    // The encoder's raw port is NV12 only. A decode surface follows the
    // profile, and a VideoProc surface follows the config's RT format: the
    // processor scales P010 as well as NV12, one kernel per sample width.
    const bool encoding = it->second.entrypoint == VAEntrypointEncSlice;
    // A VideoProc config creates the render target, not a decoded picture, so
    // its floor is the kernel's rather than the node's; a client scaling down
    // to a thumbnail is otherwise refused before the driver is called.
    const bool vpp = profile == VAProfileNone && it->second.entrypoint == VAEntrypointVideoProc;
    if (!encoding && d.capabilities.p010 && (p010_decode_profile(profile) || vpp))
        add(VASurfaceAttribPixelFormat, VA_FOURCC_P010, VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE);
    // Packed surfaces exist for the processor's overlay input only: a client
    // uploads BGRA or RGBA and asks for it to be composited onto a YUV frame.
    if (vpp) {
        add(VASurfaceAttribPixelFormat, VA_FOURCC_BGRA,
            VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE);
        add(VASurfaceAttribPixelFormat, VA_FOURCC_RGBA,
            VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE);
    }
    const unsigned min_geometry = vpp ? kVppMinGeometry : d.capabilities.min_width;
    add(VASurfaceAttribMinWidth, static_cast<int>(min_geometry), VA_SURFACE_ATTRIB_GETTABLE);
    add(VASurfaceAttribMaxWidth, static_cast<int>(d.capabilities.max_width), VA_SURFACE_ATTRIB_GETTABLE);
    add(VASurfaceAttribMinHeight,
        static_cast<int>(vpp ? kVppMinGeometry : d.capabilities.min_height), VA_SURFACE_ATTRIB_GETTABLE);
    add(VASurfaceAttribMaxHeight, static_cast<int>(d.capabilities.max_height), VA_SURFACE_ATTRIB_GETTABLE);
    add(VASurfaceAttribMemoryType,
        VA_SURFACE_ATTRIB_MEM_TYPE_VA | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE);
    if (attributes)
        std::copy_n(list, std::min(n, *count), attributes);
    *count = n;
    return VA_STATUS_SUCCESS;
}

VAStatus exportSurfaceHandle(VADriverContextP ctx, VASurfaceID id, uint32_t mem_type, uint32_t flags, void* descriptor)
{
    return guarded(ctx, "exportSurfaceHandle", [&] {
        auto& d = data(ctx);
        iris::require(descriptor != nullptr, "null descriptor", VA_STATUS_ERROR_INVALID_PARAMETER);
        iris::require(mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, "export requires DRM_PRIME_2",
            VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
        std::shared_ptr<Surface> s;
        std::shared_ptr<iris::Session> session;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            s = surface(d, id);
            session = session_of(d, *s);
        }
        // The client wants pixels: finish the picture first. Asking even when
        // the target has stopped rendering is what catches a picture the
        // firmware already failed; exporting it would hand out a buffer that
        // was never filled.
        wait_for(d, std::move(session), s);

        int fd = -1;
        iris::Layout layout;
        bool persistent = false;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            if (s->target.frame) {
                // Direct: hand out the CAPTURE slot itself.
                fd = s->target.frame->fd();
                layout = s->target.frame->layout();
            } else {
                // Before any decode, or a Copy surface: the stable buffer.
                // Exporting before the first decode marks the surface
                // persistent, and every later completion is copied into it.
                auto& stable = ensure_stable(d, *s);
                fd = stable.fd();
                layout = stable.layout();
                if (!s->target.completed && !s->persistent_export) {
                    s->persistent_export = true;
                    persistent = true;
                }
            }
        }
        iris::require(!(flags & VA_EXPORT_SURFACE_WRITE_ONLY) || !s->target.frame,
            "a decoded CAPTURE slot cannot be exported writable", VA_STATUS_ERROR_UNIMPLEMENTED);

        auto& desc = *static_cast<VADRMPRIMESurfaceDescriptor*>(descriptor);
        desc = {};
        desc.fourcc = layout.fourcc;
        desc.width = s->width;
        desc.height = s->height;
        desc.num_objects = 1;
        desc.objects[0].fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        iris::require(desc.objects[0].fd >= 0, "duplicate DMA-BUF fd");
        desc.objects[0].size = layout.size;
        desc.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
        const bool p010 = layout.fourcc == VA_FOURCC_P010;
        if (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) {
            desc.num_layers = 1;
            desc.layers[0].drm_format = p010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
            desc.layers[0].num_planes = 2;
            for (unsigned i = 0; i < 2; ++i) {
                desc.layers[0].object_index[i] = 0;
                desc.layers[0].pitch[i] = layout.stride;
                desc.layers[0].offset[i] = i ? layout.chroma_offset() : layout.data_offset;
            }
        } else {
            desc.num_layers = 2;
            desc.layers[0].drm_format = p010 ? DRM_FORMAT_R16 : DRM_FORMAT_R8;
            desc.layers[1].drm_format = p010 ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88;
            for (unsigned i = 0; i < 2; ++i) {
                desc.layers[i].num_planes = 1;
                desc.layers[i].object_index[0] = 0;
                desc.layers[i].pitch[0] = layout.stride;
                desc.layers[i].offset[0] = i ? layout.chroma_offset() : layout.data_offset;
            }
        }
        d.trace("va export_surface id=%u persistent=%d source=%s fd=%d pitch=%u", id, persistent ? 1 : 0,
            s->target.frame ? "frame" : "stable", desc.objects[0].fd, layout.stride);
        return VA_STATUS_SUCCESS;
    });
}

VAStatus putSurface(VADriverContextP, VASurfaceID, void*, short, short, unsigned short, unsigned short, short, short,
    unsigned short, unsigned short, VARectangle*, unsigned, unsigned)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus lockSurface(VADriverContextP, VASurfaceID, unsigned*, unsigned*, unsigned*, unsigned*, unsigned*, unsigned*,
    unsigned*, unsigned*, void**)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus unlockSurface(VADriverContextP, VASurfaceID) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

} // namespace va
