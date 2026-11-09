// SPDX-License-Identifier: MIT
//
// The client-memory import rules (docs/architecture.md §3.3): a DRM_PRIME_2
// descriptor or a PRIME_1 buffer table is accepted only when it describes
// the CAPTURE layout the firmware writes, and the layout it produces is the
// same one stable buffers allocate. No hardware and no libva display: the
// validator is device-free on purpose.

#include "../src/iris/alloc.h"
#include "../src/util/error.h"
#include "../src/util/prime.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

extern "C" {
#include <libdrm/drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
}

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

// The VA status a validator throws, or -1 for "accepted".
template <typename Validate> VAStatus status_of(Validate&& validate)
{
    try {
        validate();
        return static_cast<VAStatus>(-1);
    } catch (const iris::Error& error) {
        return error.status;
    }
}

constexpr unsigned width = 1920;
constexpr unsigned height = 1080;

uintptr_t prime1_fd = 5;

VADRMPRIMESurfaceDescriptor prime2(uint32_t fourcc)
{
    const iris::PlaneLayout planes = iris::plane_layout(fourcc, width, height);
    VADRMPRIMESurfaceDescriptor descriptor = {};
    descriptor.fourcc = fourcc;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.num_objects = 1;
    descriptor.objects[0].fd = 3;
    descriptor.objects[0].size = planes.size;
    descriptor.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    descriptor.num_layers = 1;
    const bool p010 = fourcc == VA_FOURCC_P010;
    descriptor.layers[0].drm_format = p010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
    descriptor.layers[0].num_planes = 2;
    descriptor.layers[0].object_index[0] = 0;
    descriptor.layers[0].object_index[1] = 0;
    for (unsigned i = 0; i < 2; ++i) {
        descriptor.layers[0].pitch[i] = planes.stride;
        descriptor.layers[0].offset[i] = i ? planes.chroma_offset : 0;
    }
    return descriptor;
}

// The two-layer form a compositor emits: luma plane and chroma plane named
// separately, both in the same object.
VADRMPRIMESurfaceDescriptor split_layers(const VADRMPRIMESurfaceDescriptor& composed)
{
    VADRMPRIMESurfaceDescriptor descriptor = composed;
    const bool p010 = composed.fourcc == VA_FOURCC_P010;
    descriptor.num_layers = 2;
    std::memset(descriptor.layers, 0, sizeof(descriptor.layers));
    descriptor.layers[0].drm_format = p010 ? DRM_FORMAT_R16 : DRM_FORMAT_R8;
    descriptor.layers[1].drm_format = p010 ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88;
    for (unsigned i = 0; i < 2; ++i) {
        descriptor.layers[i].num_planes = 1;
        descriptor.layers[i].object_index[0] = 0;
        descriptor.layers[i].pitch[0] = composed.layers[0].pitch[i];
        descriptor.layers[i].offset[0] = composed.layers[0].offset[i];
    }
    return descriptor;
}

VASurfaceAttribExternalBuffers prime1(uint32_t fourcc)
{
    const iris::PlaneLayout planes = iris::plane_layout(fourcc, width, height);
    VASurfaceAttribExternalBuffers buffers = {};
    buffers.pixel_format = fourcc;
    buffers.width = width;
    buffers.height = height;
    buffers.data_size = planes.size;
    buffers.num_planes = 2;
    buffers.pitches[0] = planes.stride;
    buffers.pitches[1] = planes.stride;
    buffers.offsets[1] = planes.chroma_offset;
    buffers.num_buffers = 1;
    buffers.buffers = &prime1_fd;
    return buffers;
}

void test_layout_is_shared()
{
    // One definition of the CAPTURE layout: what an import accepts is what a
    // stable buffer (Chrome's allocation) already has.
    for (uint32_t fourcc : { VA_FOURCC_NV12, VA_FOURCC_P010 }) {
        for (const auto& size : { std::pair<unsigned, unsigned> { 1280, 720 },
                 std::pair<unsigned, unsigned> { 1920, 1080 }, std::pair<unsigned, unsigned> { 3840, 2176 } }) {
            const iris::PlaneLayout planes = iris::plane_layout(fourcc, size.first, size.second);
            const iris::Layout stable = iris::stable_layout(fourcc, size.first, size.second);
            check(planes.stride == stable.stride && planes.chroma_offset == stable.chroma_offset(),
                "plane_layout and stable_layout disagree");
            check(planes.size <= stable.size && stable.size - planes.size < 4096,
                "the stable buffer must be the plane bytes rounded up to 4 KiB");
            check(stable.size % 4096 == 0 && planes.stride % (fourcc == VA_FOURCC_P010 ? 256u : 128u) == 0,
                "alignment");
        }
    }
    check(status_of([] { return iris::plane_layout(VA_FOURCC_YV12, 16, 16); })
            == VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
        "an unsupported fourcc is refused");
}

void test_prime2_accepts_the_capture_layout()
{
    for (uint32_t fourcc : { VA_FOURCC_NV12, VA_FOURCC_P010 }) {
        const iris::PlaneLayout expected = iris::plane_layout(fourcc, width, height);
        const VADRMPRIMESurfaceDescriptor composed = prime2(fourcc);
        const std::optional<iris::PlaneLayout> given = [&]() -> std::optional<iris::PlaneLayout> {
            try {
                return iris::validate_prime2(composed, width, height, fourcc, composed.objects[0].size);
            } catch (const iris::Error&) {
                return std::nullopt;
            }
        }();
        check(given && given->stride == expected.stride && given->chroma_offset == expected.chroma_offset,
            "a composed-layer descriptor in the CAPTURE layout is accepted");

        const VADRMPRIMESurfaceDescriptor split = split_layers(composed);
        check(status_of([&] { return iris::validate_prime2(split, width, height, fourcc, split.objects[0].size); })
                == -1,
            "a two-layer descriptor in the CAPTURE layout is accepted");
        // An allocation larger than the planes (a driver-allocated buffer
        // rounded to 4 KiB, or our own export round trip) is fine.
        check(status_of([&] {
                  return iris::validate_prime2(composed, width, height, fourcc, iris::stable_layout(fourcc, width, height).size);
              })
                == -1,
            "a larger allocation than the planes is accepted");
    }
}

void test_prime2_rejections()
{
    const uint32_t fourcc = VA_FOURCC_NV12;
    const unsigned invalid = VA_STATUS_ERROR_INVALID_PARAMETER;
    const auto status = [](VADRMPRIMESurfaceDescriptor descriptor, unsigned width_in = width,
                            unsigned height_in = height, uint64_t allocation = 0) {
        if (!allocation)
            allocation = descriptor.objects[0].size;
        return status_of([&] { return iris::validate_prime2(descriptor, width_in, height_in, fourcc, allocation); });
    };

    check(status(prime2(VA_FOURCC_P010)) == invalid, "a descriptor for another fourcc is refused");

    VADRMPRIMESurfaceDescriptor descriptor = prime2(fourcc);
    descriptor.width = 1920 - 16;
    check(status(descriptor) == invalid, "a descriptor for another width is refused");
    descriptor = prime2(fourcc);
    descriptor.num_objects = 2;
    check(status(descriptor) == invalid, "two objects are refused");
    descriptor = prime2(fourcc);
    descriptor.objects[0].fd = -1;
    check(status(descriptor) == invalid, "a descriptor without an fd is refused");
    descriptor = prime2(fourcc);
    descriptor.objects[0].drm_format_modifier = 0x1000000000000001ull; // tiled
    check(status(descriptor) == invalid, "a tiled modifier is refused");
    descriptor = prime2(fourcc);
    descriptor.num_layers = 0;
    check(status(descriptor) == invalid, "no layers is refused");
    descriptor = split_layers(prime2(fourcc));
    std::swap(descriptor.layers[0].drm_format, descriptor.layers[1].drm_format);
    check(status(descriptor) == invalid, "luma and chroma layers in the wrong order are refused");
    descriptor = prime2(fourcc);
    descriptor.layers[0].pitch[0] = 2048;
    check(status(descriptor) == invalid, "a client pitch the firmware does not write is refused");
    descriptor = prime2(fourcc);
    descriptor.layers[0].pitch[1] = 2048;
    check(status(descriptor) == invalid, "a chroma pitch that differs from luma is refused");
    descriptor = prime2(fourcc);
    descriptor.layers[0].offset[0] = 4096;
    check(status(descriptor) == invalid, "luma not at the object start is refused");
    descriptor = prime2(fourcc);
    descriptor.layers[0].offset[1] += 16;
    check(status(descriptor) == invalid, "chroma at the wrong offset is refused");
    descriptor = prime2(fourcc);
    descriptor.objects[0].size -= 4096;
    check(status(descriptor) == invalid, "a descriptor smaller than its planes is refused");
    descriptor = prime2(fourcc);
    check(status(descriptor, width, height, descriptor.objects[0].size - 1) == invalid,
        "an allocation smaller than the descriptor claims is refused");
}

void test_prime1()
{
    const uint32_t fourcc = VA_FOURCC_NV12;
    const unsigned invalid = VA_STATUS_ERROR_INVALID_PARAMETER;
    const auto status = [](VASurfaceAttribExternalBuffers buffers, uint64_t allocation = 0) {
        if (!allocation)
            allocation = buffers.data_size;
        return status_of([&] { return iris::validate_prime1(buffers, width, height, fourcc, allocation); });
    };

    check(status(prime1(fourcc)) == -1, "a PRIME_1 buffer in the CAPTURE layout is accepted");
    check(status(prime1(VA_FOURCC_P010)) == invalid, "a PRIME_1 buffer for another fourcc is refused");

    VASurfaceAttribExternalBuffers buffers = prime1(fourcc);
    buffers.flags = VA_SURFACE_EXTBUF_DESC_ENABLE_TILING;
    check(status(buffers) == invalid, "a tiled PRIME_1 buffer is refused");
    buffers = prime1(fourcc);
    buffers.pitches[0] = 2048;
    check(status(buffers) == invalid, "a PRIME_1 pitch the firmware does not write is refused");
    buffers = prime1(fourcc);
    buffers.offsets[0] = 4096;
    check(status(buffers) == invalid, "PRIME_1 luma not at the start is refused");
    buffers = prime1(fourcc);
    buffers.offsets[1] -= 16;
    check(status(buffers) == invalid, "PRIME_1 chroma at the wrong offset is refused");
    buffers = prime1(fourcc);
    buffers.num_buffers = 2;
    check(status(buffers) == invalid, "two PRIME_1 buffers are refused");
    buffers = prime1(fourcc);
    buffers.data_size -= 4096;
    check(status(buffers) == invalid, "a PRIME_1 table smaller than its planes is refused");
}

} // namespace

int main()
{
    test_layout_is_shared();
    test_prime2_accepts_the_capture_layout();
    test_prime2_rejections();
    test_prime1();
    if (failures) {
        std::printf("FAILED %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS prime-import\n");
    return 0;
}
