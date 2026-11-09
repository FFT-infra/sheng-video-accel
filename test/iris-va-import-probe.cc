// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): a VA client that hands the driver its
// own DMA-BUF. It drives the public libva API against the built module, so
// the answers come from vaCreateSurfaces2 and from the firmware, not from a
// unit test's idea of either.
//
// Usage: iris-va-import-probe <render-node> [gem|heap]
//
// `heap` makes the client's buffer a system-heap DMA-BUF of exactly the
// plane bytes, the way a client that sizes its own allocation hands one
// over; the default allocates it the way a compositor on this platform does
// (MSM GEM, 4 KiB-rounded). Both must work.
//
// Checks, in order:
//   - vaCreateSurfaces2 with a DRM_PRIME_2 descriptor in the CAPTURE layout
//     succeeds and PRIME_1 does too;
//   - the surface is a view of the client's memory, not a copy: bytes
//     written through the client's fd are visible in the derived image;
//   - an encode driven from that imported surface produces the same
//     bitstream (CQP, fresh session, intra only) as the same picture from a
//     driver-allocated surface, and a different bitstream once the client's
//     pixels change -- the firmware read the client's buffer;
//   - descriptors outside the CAPTURE layout fail with
//     VA_STATUS_ERROR_INVALID_PARAMETER.
//
// The decoder's use of the same memory is the Import publish policy proven by
// test/iris-import-probe.cc; this probe is the VA-API half.

#include "../src/iris/alloc.h"
#include "../src/util/prime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
}

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

// One NV12 frame with a per-row gradient, so two different fills (or a
// half-read buffer) cannot hash alike.
void fill(std::vector<uint8_t>& frame, uint8_t seed)
{
    for (size_t i = 0; i < frame.size(); ++i)
        frame[i] = static_cast<uint8_t>(seed + (i * 7) % 251);
}

// CPU writes into a DMA-BUF have to be bracketed, the same way the driver
// brackets its own stable buffers, or the firmware may read stale lines. The
// client buffer carries the CAPTURE layout, not a packed frame: luma rows at
// stride, chroma at the aligned chroma offset.
void write_client(int fd, uint8_t* mapping, const std::vector<uint8_t>& frame, const iris::PlaneLayout& planes,
    unsigned width, unsigned height)
{
    dma_buf_sync sync = {};
    sync.flags = DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_START;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    const unsigned chroma_rows = (height + 1) / 2;
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(mapping + static_cast<size_t>(y) * planes.stride, frame.data() + static_cast<size_t>(y) * width, width);
    for (unsigned y = 0; y < chroma_rows; ++y)
        std::memcpy(mapping + planes.chroma_offset + static_cast<size_t>(y) * planes.stride,
            frame.data() + static_cast<size_t>(width) * height + static_cast<size_t>(y) * width, width);
    sync.flags = DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_END;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

VADRMPRIMESurfaceDescriptor prime2(int fd, uint64_t size, unsigned width, unsigned height)
{
    const iris::PlaneLayout planes = iris::plane_layout(VA_FOURCC_NV12, width, height);
    VADRMPRIMESurfaceDescriptor descriptor = {};
    descriptor.fourcc = VA_FOURCC_NV12;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.num_objects = 1;
    descriptor.objects[0].fd = fd;
    descriptor.objects[0].size = static_cast<uint32_t>(size);
    descriptor.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    descriptor.num_layers = 1;
    descriptor.layers[0].drm_format = DRM_FORMAT_NV12;
    descriptor.layers[0].num_planes = 2;
    for (unsigned i = 0; i < 2; ++i) {
        descriptor.layers[0].object_index[i] = 0;
        descriptor.layers[0].pitch[i] = planes.stride;
        descriptor.layers[0].offset[i] = i ? planes.chroma_offset : 0;
    }
    return descriptor;
}

VAStatus import_surface(VADisplay display, unsigned width, unsigned height, VADRMPRIMESurfaceDescriptor& descriptor,
    VASurfaceID* surface)
{
    VASurfaceAttrib attributes[2] = {};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = &descriptor;
    return vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height, surface, 1, attributes, 2);
}

// Copy `frame` (tightly packed NV12 rows) into the surface through a derived
// image, the path every VA upload uses.
VAStatus upload(VADisplay display, VASurfaceID surface, const std::vector<uint8_t>& frame, unsigned width,
    unsigned height)
{
    VAImage image = {};
    VAStatus status = vaDeriveImage(display, surface, &image);
    if (status != VA_STATUS_SUCCESS)
        return status;
    void* mapped = nullptr;
    status = vaMapBuffer(display, image.buf, &mapped);
    if (status != VA_STATUS_SUCCESS) {
        vaDestroyImage(display, image.image_id);
        return status;
    }
    auto* base = static_cast<uint8_t*>(mapped);
    const unsigned chroma_rows = (height + 1) / 2;
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(base + image.offsets[0] + static_cast<size_t>(y) * image.pitches[0], frame.data() + y * width, width);
    for (unsigned y = 0; y < chroma_rows; ++y)
        std::memcpy(base + image.offsets[1] + static_cast<size_t>(y) * image.pitches[1],
            frame.data() + static_cast<size_t>(width) * height + y * width, width);
    vaUnmapBuffer(display, image.buf);
    vaDestroyImage(display, image.image_id);
    return VA_STATUS_SUCCESS;
}

VAStatus read_back(VADisplay display, VASurfaceID surface, std::vector<uint8_t>& frame, unsigned width,
    unsigned height)
{
    VAImage image = {};
    VAStatus status = vaDeriveImage(display, surface, &image);
    if (status != VA_STATUS_SUCCESS)
        return status;
    void* mapped = nullptr;
    status = vaMapBuffer(display, image.buf, &mapped);
    if (status != VA_STATUS_SUCCESS) {
        vaDestroyImage(display, image.image_id);
        return status;
    }
    const auto* base = static_cast<const uint8_t*>(mapped);
    frame.resize(static_cast<size_t>(width) * height * 3 / 2);
    const unsigned chroma_rows = (height + 1) / 2;
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(frame.data() + y * width, base + image.offsets[0] + static_cast<size_t>(y) * image.pitches[0], width);
    for (unsigned y = 0; y < chroma_rows; ++y)
        std::memcpy(frame.data() + static_cast<size_t>(width) * height + y * width,
            base + image.offsets[1] + static_cast<size_t>(y) * image.pitches[1], width);
    vaUnmapBuffer(display, image.buf);
    vaDestroyImage(display, image.image_id);
    return VA_STATUS_SUCCESS;
}

struct Encoder {
    VADisplay display = nullptr;
    VAConfigID config = VA_INVALID_ID;

    // One intra picture from a fresh session: both surfaces are then encoded
    // as the first picture (frame_num 0), so identical pixels give identical
    // bytes rather than a one-byte difference in the slice header.
    std::vector<uint8_t> encode(VASurfaceID surface, unsigned width, unsigned height)
    {
        VAContextID context = VA_INVALID_ID;
        if (vaCreateContext(display, config, width, height, 0, nullptr, 0, &context) != VA_STATUS_SUCCESS)
            return {};
        VABufferID coded = VA_INVALID_ID;
        if (vaCreateBuffer(display, context, VAEncCodedBufferType, 4 << 20, 1, nullptr, &coded) != VA_STATUS_SUCCESS) {
            vaDestroyContext(display, context);
            return {};
        }
        VAEncSequenceParameterBufferH264 sequence = {};
        sequence.level_idc = 41;
        sequence.bits_per_second = 4000000;
        // Intra-only: every picture is an IDR with its own headers, so two
        // encodes of the same content are byte-identical and neither can
        // predict from the other.
        sequence.intra_period = 0;
        sequence.ip_period = 1;
        sequence.seq_fields.bits.chroma_format_idc = 1;
        sequence.picture_width_in_mbs = width / 16;
        sequence.picture_height_in_mbs = height / 16;

        VAEncPictureParameterBufferH264 picture = {};
        picture.coded_buf = coded;
        picture.CurrPic.picture_id = surface;
        picture.CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        picture.pic_fields.bits.entropy_coding_mode_flag = 1;
        picture.pic_init_qp = 26;
        picture.pic_fields.bits.idr_pic_flag = 1;
        picture.num_ref_idx_l0_active_minus1 = 0;
        picture.num_ref_idx_l1_active_minus1 = 0;

        VAEncSliceParameterBufferH264 slice = {};
        slice.macroblock_address = 0;
        slice.num_macroblocks = width / 16 * (height / 16);

        VABufferID buffers[3] = { VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID };
        vaCreateBuffer(display, context, VAEncSequenceParameterBufferType, sizeof(sequence), 1, &sequence, &buffers[0]);
        vaCreateBuffer(display, context, VAEncPictureParameterBufferType, sizeof(picture), 1, &picture, &buffers[1]);
        vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(slice), 1, &slice, &buffers[2]);

        std::vector<uint8_t> payload;
        if (vaBeginPicture(display, context, surface) == VA_STATUS_SUCCESS
            && vaRenderPicture(display, context, buffers, 3) == VA_STATUS_SUCCESS
            && vaEndPicture(display, context) == VA_STATUS_SUCCESS) {
            void* mapped = nullptr;
            if (vaMapBuffer(display, coded, &mapped) == VA_STATUS_SUCCESS) {
                const auto* segment = static_cast<VACodedBufferSegment*>(mapped);
                payload.assign(static_cast<uint8_t*>(segment->buf),
                    static_cast<uint8_t*>(segment->buf) + segment->size);
                vaUnmapBuffer(display, coded);
            }
        }
        for (VABufferID buffer : buffers)
            if (buffer != VA_INVALID_ID)
                vaDestroyBuffer(display, buffer);
        vaDestroyBuffer(display, coded);
        vaDestroyContext(display, context);
        return payload;
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <render-node>\n", argv[0]);
        return 2;
    }
    const int drm = open(argv[1], O_RDWR | O_CLOEXEC);
    if (drm < 0) {
        std::perror("open render node");
        return 2;
    }
    VADisplay display = vaGetDisplayDRM(drm);
    int major = 0;
    int minor = 0;
    VAStatus status = vaInitialize(display, &major, &minor);
    check(status == VA_STATUS_SUCCESS, "the built module initialises");
    if (status != VA_STATUS_SUCCESS) {
        std::printf("status=%#x; is LIBVA_DRIVERS_PATH set to the build tree?\n", status);
        return 1;
    }
    std::printf("vendor=%s\n", vaQueryVendorString(display));

    constexpr unsigned width = 1280;
    constexpr unsigned height = 720;
    const iris::PlaneLayout planes = iris::plane_layout(VA_FOURCC_NV12, width, height);
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 3 / 2);
    fill(frame, 16);

    // The client's memory: one linear DMA-BUF in the CAPTURE layout, as a
    // compositor or another decoder hands over. Allocated the same way a
    // client allocation lands on this platform (MSM GEM, system heap as the
    // fallback), not through the driver's surface path.
    std::unique_ptr<iris::StableBuffer> client;
    if (argc > 2 && std::strcmp(argv[2], "heap") == 0) {
        // A client that allocated its own buffer: a system-heap DMA-BUF of
        // exactly the plane bytes, without the driver's 4 KiB rounding.
        const int fd = iris::allocate_dma_heap(planes.size);
        check(fd >= 0, "allocated the client's DMA-BUF");
        if (fd < 0)
            return 1;
        iris::Layout layout = iris::stable_layout(VA_FOURCC_NV12, width, height);
        layout.size = planes.size;
        client = std::make_unique<iris::StableBuffer>(fd, planes.size, iris::MemoryOrigin::DmaHeap, layout);
    } else {
        client = iris::allocate_stable(drm, iris::stable_layout(VA_FOURCC_NV12, width, height));
        check(client != nullptr, "allocated the client's DMA-BUF");
        if (!client)
            return 1;
    }
    const int client_fd = client->fd();
    auto* client_map = client->map();

    VASurfaceID imported = VA_INVALID_ID;
    VADRMPRIMESurfaceDescriptor descriptor = prime2(client_fd, client->size(), width, height);
    check(import_surface(display, width, height, descriptor, &imported) == VA_STATUS_SUCCESS,
        "DRM_PRIME_2 import in the CAPTURE layout creates a surface");

    VASurfaceID normal = VA_INVALID_ID;
    check(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height, &normal, 1, nullptr, 0) == VA_STATUS_SUCCESS,
        "driver-allocated surface creates");

    // Aliasing: bytes written through the client's fd must be what the
    // surface contains, without an upload.
    write_client(client_fd, client_map, frame, planes, width, height);
    std::vector<uint8_t> readback;
    check(read_back(display, imported, readback, width, height) == VA_STATUS_SUCCESS && readback == frame,
        "the imported surface is a view of the client's bytes");

    // Encode the same picture from both surfaces: identical input must give
    // an identical bitstream only if the firmware read the client's buffer.
    VAConfigAttrib attributes[2] = {};
    attributes[0].type = VAConfigAttribRTFormat;
    attributes[0].value = VA_RT_FORMAT_YUV420;
    attributes[1].type = VAConfigAttribRateControl;
    // Constant QP: the same pixels must produce the same bytes, or the
    // comparison below measures the rate controller instead of the buffer.
    attributes[1].value = VA_RC_CQP;
    VAConfigID config = VA_INVALID_ID;
    status = vaCreateConfig(display, VAProfileH264High, VAEntrypointEncSlice, attributes, 2, &config);
    check(status == VA_STATUS_SUCCESS, "H.264 encode config creates");
    if (status == VA_STATUS_SUCCESS) {
        Encoder encoder;
        encoder.display = display;
        encoder.config = config;
        {
            check(upload(display, normal, frame, width, height) == VA_STATUS_SUCCESS, "uploaded the same frame");
            const std::vector<uint8_t> from_normal = encoder.encode(normal, width, height);
            const std::vector<uint8_t> from_import = encoder.encode(imported, width, height);
            check(!from_normal.empty() && from_normal == from_import,
                "encoding the imported surface gives the same bitstream");

            // Different pixels on both sides: the bitstream must move, and
            // the imported surface must move with it.
            fill(frame, 200);
            upload(display, normal, frame, width, height);
            write_client(client_fd, client_map, frame, planes, width, height);
            const std::vector<uint8_t> changed = encoder.encode(normal, width, height);
            const std::vector<uint8_t> changed_import = encoder.encode(imported, width, height);
            check(!changed.empty() && changed != from_normal && changed == changed_import,
                "changed client bytes change the bitstream by the same amount");
            std::printf("payload normal=%zu import=%zu changed=%zu changed_import=%zu\n", from_normal.size(),
                from_import.size(), changed.size(), changed_import.size());
        }
        vaDestroyConfig(display, config);
    }

    // A descriptor outside the CAPTURE layout is refused, not half-filled.
    VADRMPRIMESurfaceDescriptor wrong = descriptor;
    wrong.layers[0].pitch[0] = planes.stride * 2;
    VASurfaceID rejected = VA_INVALID_ID;
    check(import_surface(display, width, height, wrong, &rejected) == VA_STATUS_ERROR_INVALID_PARAMETER,
        "a client pitch the firmware does not write is refused");

    // PRIME_1: the same buffer described as a buffer table.
    VASurfaceAttribExternalBuffers buffers = {};
    buffers.pixel_format = VA_FOURCC_NV12;
    buffers.width = width;
    buffers.height = height;
    buffers.data_size = planes.size;
    buffers.num_planes = 2;
    buffers.pitches[0] = planes.stride;
    buffers.pitches[1] = planes.stride;
    buffers.offsets[1] = planes.chroma_offset;
    buffers.num_buffers = 1;
    uintptr_t client_handle = static_cast<uintptr_t>(client_fd);
    buffers.buffers = &client_handle;
    VASurfaceAttrib prime1_attributes[2] = {};
    prime1_attributes[0].type = VASurfaceAttribMemoryType;
    prime1_attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    prime1_attributes[0].value.type = VAGenericValueTypeInteger;
    prime1_attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    prime1_attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    prime1_attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    prime1_attributes[1].value.type = VAGenericValueTypePointer;
    prime1_attributes[1].value.value.p = &buffers;
    VASurfaceID prime1 = VA_INVALID_ID;
    check(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height, &prime1, 1, prime1_attributes, 2)
            == VA_STATUS_SUCCESS,
        "PRIME_1 import in the CAPTURE layout creates a surface");

    if (prime1 != VA_INVALID_ID)
        vaDestroySurfaces(display, &prime1, 1);
    if (normal != VA_INVALID_ID)
        vaDestroySurfaces(display, &normal, 1);
    if (imported != VA_INVALID_ID)
        vaDestroySurfaces(display, &imported, 1);
    client.reset();
    vaTerminate(display);
    close(drm);
    std::printf("%s va-import width=%u height=%u stride=%u chroma=%u size=%u\n", failures ? "FAIL" : "PASS", width,
        height, planes.stride, planes.chroma_offset, planes.size);
    return failures ? 1 : 0;
}
