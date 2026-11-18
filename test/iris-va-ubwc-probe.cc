// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): a VA client that asks for a UBWC surface
// and decodes into it. test/iris-ubwc-gate.sh proves the session writes the
// kernel's Q08C layout; this drives the public API a browser or compositor
// uses, so the answers come from vaCreateSurfaces, the decode and
// vaExportSurfaceHandle, not from a unit test's idea of them.
//
// Usage: iris-va-ubwc-probe <render-node> <vp9.ivf> <width> <height>
//
// VP9 carries its whole frame in the slice-data buffer and its translator
// reads only a handful of header bits from the picture parameters, so the
// probe can be a decode client without synthesising parameter sets.
//
// With V4L2_VA_UBWC=1 in the environment the probe requires a
// QCOM_COMPRESSED surface: four R8 layers at the kernel's Q08C offsets,
// the modifier on the exported object, and the image entry points refusing
// it. With the switch unset it requires the same list to be refused, so the
// capability cannot appear without the opt-in. Either way it also requires
// QCOM_COMPRESSED on a 10-bit RT format to be refused, and a linear surface
// to keep the two-plane export.
//
// Exit status: 0 when every check passes, 1 on a failed check, 2 on usage,
// import or decode failure.

#include "../src/util/prime.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_dec_vp9.h>
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

bool read_file(const char* path, std::vector<uint8_t>& bytes)
{
    FILE* file = std::fopen(path, "rb");
    if (!file)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return got == bytes.size();
}

struct Frame {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// IVF: a 32-byte header, then a 12-byte record per frame (4-byte size,
// 8-byte timestamp) followed by the frame bytes.
bool parse_ivf(const std::vector<uint8_t>& bytes, std::vector<Frame>& frames)
{
    auto le32 = [](const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
            | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    };
    if (bytes.size() < 32 || std::memcmp(bytes.data(), "DKIF", 4) != 0)
        return false;
    size_t offset = 32;
    while (offset + 12 <= bytes.size()) {
        const uint32_t size = le32(bytes.data() + offset);
        offset += 12;
        if (size == 0 || offset + size > bytes.size())
            return false;
        frames.push_back({ bytes.data() + offset, size });
        offset += size;
    }
    return !frames.empty();
}

// The bits a VP9 picture parameter buffer has to agree with: the raw header
// the driver parses and compares. Everything after error_resilient_mode is
// left to the firmware, which receives the complete frame.
struct Vp9Header {
    unsigned profile = 0;
    unsigned frame_type = 0;
    unsigned show_frame = 0;
    unsigned error_resilient = 0;
};

bool read_vp9_header(const Frame& frame, Vp9Header& header)
{
    if (frame.size < 2)
        return false;
    unsigned position = 0;
    auto bit = [&]() {
        const uint8_t value = position < frame.size * 8
            ? static_cast<uint8_t>((frame.data[position / 8] >> (7 - position % 8)) & 1u)
            : 0u;
        ++position;
        return value;
    };
    const unsigned marker = (bit() << 1) | bit();
    const unsigned profile = bit() | (bit() << 1);
    if (marker != 2 || profile != 0)
        return false;
    if (bit()) // show_existing_frame
        return false;
    header.profile = profile;
    header.frame_type = bit();
    header.show_frame = bit();
    header.error_resilient = bit();
    return true;
}

struct ModifierList {
    VADRMFormatModifierList list = {};
    uint64_t modifiers[2] = {};
};

VASurfaceAttrib modifier_attribute(ModifierList& entry)
{
    VASurfaceAttrib attribute = {};
    attribute.type = VASurfaceAttribDRMFormatModifiers;
    attribute.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribute.value.type = VAGenericValueTypePointer;
    attribute.value.value.p = &entry.list;
    return attribute;
}

VASurfaceAttrib pixel_format_attribute()
{
    VASurfaceAttrib attribute = {};
    attribute.type = VASurfaceAttribPixelFormat;
    attribute.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribute.value.type = VAGenericValueTypeInteger;
    attribute.value.value.i = VA_FOURCC_NV12;
    return attribute;
}

VADisplay open_display(const char* node, int& fd)
{
    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    VADisplay display = vaGetDisplayDRM(fd);
    if (!display)
        return nullptr;
    int major = 0;
    int minor = 0;
    return vaInitialize(display, &major, &minor) == VA_STATUS_SUCCESS ? display : nullptr;
}

bool decode(VADisplay display, VAContextID context, const std::vector<Frame>& frames, const VASurfaceID* ids,
    unsigned count, unsigned width, unsigned height)
{
    for (size_t i = 0; i < frames.size(); ++i) {
        Vp9Header header;
        if (!read_vp9_header(frames[i], header)) {
            std::fprintf(stderr, "frame %zu is not a Profile 0 shown frame\n", i);
            return false;
        }
        VADecPictureParameterBufferVP9 parameters = {};
        parameters.frame_width = static_cast<uint16_t>(width);
        parameters.frame_height = static_cast<uint16_t>(height);
        for (VASurfaceID& reference : parameters.reference_frames)
            reference = VA_INVALID_SURFACE;
        parameters.pic_fields.bits.subsampling_x = 1;
        parameters.pic_fields.bits.subsampling_y = 1;
        parameters.pic_fields.bits.frame_type = header.frame_type;
        parameters.pic_fields.bits.show_frame = header.show_frame;
        parameters.pic_fields.bits.error_resilient_mode = header.error_resilient;
        parameters.profile = static_cast<uint8_t>(header.profile);
        parameters.bit_depth = 8;
        // The driver bounds these against the frame length and does not use
        // them further: the firmware reads the whole frame from the node.
        parameters.frame_header_length_in_bytes = 16;
        parameters.first_partition_size =
            static_cast<uint16_t>(std::min<size_t>(frames[i].size > 16 ? frames[i].size - 16 : 0, 12000));

        VABufferID buffers[2] = { VA_INVALID_ID, VA_INVALID_ID };
        if (vaCreateBuffer(display, context, VAPictureParameterBufferType, sizeof(parameters), 1, &parameters,
                &buffers[0])
                != VA_STATUS_SUCCESS
            || vaCreateBuffer(display, context, VASliceDataBufferType, static_cast<unsigned>(frames[i].size), 1,
                    const_cast<uint8_t*>(frames[i].data), &buffers[1])
                != VA_STATUS_SUCCESS) {
            std::fprintf(stderr, "vaCreateBuffer failed at frame %zu\n", i);
            return false;
        }
        const VASurfaceID surface = ids[i % count];
        const VAStatus status = vaBeginPicture(display, context, surface) == VA_STATUS_SUCCESS
                && vaRenderPicture(display, context, buffers, 2) == VA_STATUS_SUCCESS
                ? vaEndPicture(display, context)
                : VA_STATUS_ERROR_OPERATION_FAILED;
        vaDestroyBuffer(display, buffers[1]);
        vaDestroyBuffer(display, buffers[0]);
        if (status != VA_STATUS_SUCCESS || vaSyncSurface(display, surface) != VA_STATUS_SUCCESS) {
            std::fprintf(stderr, "decode failed at frame %zu: %s\n", i, vaErrorStr(status));
            return false;
        }
    }
    return true;
}

struct Export {
    VADRMPRIMESurfaceDescriptor descriptor = {};
    VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
    ~Export()
    {
        for (unsigned i = 0; i < descriptor.num_objects; ++i)
            if (descriptor.objects[i].fd >= 0)
                close(descriptor.objects[i].fd);
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <render-node> <vp9.ivf> <width> <height>\n", argv[0]);
        return 2;
    }
    const unsigned width = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
    const unsigned height = static_cast<unsigned>(std::strtoul(argv[4], nullptr, 10));
    std::vector<uint8_t> bytes;
    std::vector<Frame> frames;
    if (!width || !height || !read_file(argv[2], bytes) || !parse_ivf(bytes, frames)) {
        std::fprintf(stderr, "%s is not an IVF file with frames\n", argv[2]);
        return 2;
    }
    const bool ubwc_expected = std::getenv("V4L2_VA_UBWC") != nullptr;
    std::printf("INFO frames=%zu ubwc_expected=%d\n", frames.size(), ubwc_expected ? 1 : 0);

    int fd = -1;
    VADisplay display = open_display(argv[1], fd);
    if (!display) {
        std::fprintf(stderr, "cannot open a VA display on %s\n", argv[1]);
        return 2;
    }
    VAConfigAttrib attribute = {};
    attribute.type = VAConfigAttribRTFormat;
    attribute.value = VA_RT_FORMAT_YUV420;
    VAConfigID config = VA_INVALID_ID;
    if (vaCreateConfig(display, VAProfileVP9Profile0, VAEntrypointVLD, &attribute, 1, &config)
        != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "VP9 decode config refused\n");
        return 2;
    }

    const iris::PlaneLayout ubwc = iris::ubwc_layout(width, height);
    const iris::PlaneLayout linear = iris::plane_layout(VA_FOURCC_NV12, width, height);

    // 1. A modifier the driver cannot produce is refused, not downgraded.
    {
        ModifierList bogus;
        bogus.modifiers[0] = 0xdeadbeefu;
        bogus.list.num_modifiers = 1;
        bogus.list.modifiers = bogus.modifiers;
        VASurfaceAttrib attributes[] = { pixel_format_attribute(), modifier_attribute(bogus) };
        VASurfaceID ids[1] = { VA_INVALID_ID };
        check(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height, ids, 1, attributes, 2)
                == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
            "a modifier list the driver cannot produce is refused");
    }

    // 2. The UBWC list itself: granted with the switch, refused without it.
    {
        ModifierList list;
        list.modifiers[0] = DRM_FORMAT_MOD_QCOM_COMPRESSED;
        list.modifiers[1] = DRM_FORMAT_MOD_LINEAR;
        list.list.num_modifiers = 2;
        list.list.modifiers = list.modifiers;
        VASurfaceAttrib attributes[] = { pixel_format_attribute(), modifier_attribute(list) };
        VASurfaceID ids[4] = { VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID };
        const VAStatus status = vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height, ids, 4, attributes, 2);
        if (!ubwc_expected) {
            check(status == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
                "without V4L2_VA_UBWC the compressed modifier is refused");
            if (failures)
                return 1;
            std::printf("PASS UBWC surfaces stay off without the opt-in\n");
            return 0;
        }
        check(status == VA_STATUS_SUCCESS, "V4L2_VA_UBWC grants a QCOM_COMPRESSED surface");
        if (status != VA_STATUS_SUCCESS) {
            std::fprintf(stderr, "vaCreateSurfaces: %s\n", vaErrorStr(status));
            return 2;
        }

        VAContextID context = VA_INVALID_ID;
        check(vaCreateContext(display, config, width, height, 0, ids, 4, &context) == VA_STATUS_SUCCESS,
            "a decode context opens over UBWC surfaces");
        const bool decoded = context != VA_INVALID_ID && decode(display, context, frames, ids, 4, width, height);
        check(decoded, "every frame decodes into a UBWC surface");

        if (decoded) {
            const VASurfaceID last = ids[(frames.size() - 1) % 4];
            Export exported;
            exported.status = vaExportSurfaceHandle(display, last, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_SEPARATE_LAYERS, &exported.descriptor);
            check(exported.status == VA_STATUS_SUCCESS, "vaExportSurfaceHandle accepts a decoded UBWC surface");
            if (exported.status == VA_STATUS_SUCCESS) {
                const auto& descriptor = exported.descriptor;
                check(descriptor.objects[0].drm_format_modifier == DRM_FORMAT_MOD_QCOM_COMPRESSED,
                    "the exported object carries the QCOM_COMPRESSED modifier");
                check(descriptor.objects[0].size >= ubwc.size, "the exported object holds the Q08C allocation");
                check(descriptor.num_layers == 4, "the export describes four planes");
                bool planes_match = descriptor.num_layers == 4;
                for (unsigned i = 0; i < descriptor.num_layers && planes_match; ++i)
                    planes_match = descriptor.layers[i].drm_format == DRM_FORMAT_R8
                        && descriptor.layers[i].num_planes == 1
                        && descriptor.layers[i].pitch[0] == ubwc.planes[i].pitch
                        && descriptor.layers[i].offset[0] == ubwc.planes[i].offset;
                check(planes_match, "the four planes sit at the kernel's Q08C offsets and pitches");
            }

            VAImage image = {};
            check(vaDeriveImage(display, last, &image) == VA_STATUS_ERROR_UNIMPLEMENTED,
                "vaDeriveImage refuses a compressed surface");
            VAImageFormat format = {};
            format.fourcc = VA_FOURCC_NV12;
            if (vaCreateImage(display, &format, static_cast<int>(width), static_cast<int>(height), &image)
                == VA_STATUS_SUCCESS) {
                check(vaGetImage(display, last, 0, 0, width, height, image.image_id)
                        == VA_STATUS_ERROR_UNIMPLEMENTED,
                    "vaGetImage refuses a compressed surface");
                check(vaPutImage(display, last, image.image_id, 0, 0, width, height, 0, 0, width, height)
                        == VA_STATUS_ERROR_UNIMPLEMENTED,
                    "vaPutImage refuses a compressed surface");
                vaDestroyImage(display, image.image_id);
            } else {
                check(false, "vaCreateImage for the download check");
            }
        }
        if (context != VA_INVALID_ID)
            vaDestroyContext(display, context);
    }

    // 3. Q10C is not offered: the modifier on a 10-bit RT format is refused
    // rather than silently producing a surface nothing can read.
    {
        ModifierList list;
        list.modifiers[0] = DRM_FORMAT_MOD_QCOM_COMPRESSED;
        list.list.num_modifiers = 1;
        list.list.modifiers = list.modifiers;
        VASurfaceAttrib attributes[] = { modifier_attribute(list) };
        VASurfaceID ids[1] = { VA_INVALID_ID };
        check(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420_10, width, height, ids, 1, attributes, 1)
                == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
            "a compressed 10-bit (Q10C) surface is refused");
    }

    // 4. A linear list keeps the two-plane linear export.
    {
        ModifierList list;
        list.modifiers[0] = DRM_FORMAT_MOD_LINEAR;
        list.list.num_modifiers = 1;
        list.list.modifiers = list.modifiers;
        VASurfaceAttrib attributes[] = { pixel_format_attribute(), modifier_attribute(list) };
        VASurfaceID ids[2] = { VA_INVALID_ID, VA_INVALID_ID };
        const VAStatus status = vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height, ids, 2, attributes, 2);
        check(status == VA_STATUS_SUCCESS, "a linear modifier list still creates surfaces");
        if (status == VA_STATUS_SUCCESS) {
            VAContextID context = VA_INVALID_ID;
            const std::vector<Frame> first(frames.begin(), frames.begin() + 1);
            check(vaCreateContext(display, config, width, height, 0, ids, 2, &context) == VA_STATUS_SUCCESS,
                "a decode context opens over linear surfaces");
            const bool decoded = context != VA_INVALID_ID && decode(display, context, first, ids, 2, width, height);
            check(decoded, "the first frame decodes into a linear surface");
            if (decoded) {
                Export exported;
                exported.status = vaExportSurfaceHandle(display, ids[0], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                    VA_EXPORT_SURFACE_SEPARATE_LAYERS, &exported.descriptor);
                check(exported.status == VA_STATUS_SUCCESS, "vaExportSurfaceHandle accepts the linear surface");
                if (exported.status == VA_STATUS_SUCCESS) {
                    const auto& descriptor = exported.descriptor;
                    check(descriptor.objects[0].drm_format_modifier == DRM_FORMAT_MOD_LINEAR,
                        "the linear surface exports DRM_FORMAT_MOD_LINEAR");
                    check(descriptor.num_layers == 2, "the linear export describes two planes");
                    check(descriptor.num_layers == 2 && descriptor.layers[1].offset[0] == linear.chroma_offset
                            && descriptor.layers[1].pitch[0] == linear.stride,
                        "the linear chroma plane keeps the CAPTURE offset and pitch");
                }
            }
            if (context != VA_INVALID_ID)
                vaDestroyContext(display, context);
        }
    }

    vaTerminate(display);
    close(fd);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS the VA layer grants and exports a UBWC surface\n");
    return 0;
}
