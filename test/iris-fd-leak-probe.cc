// SPDX-License-Identifier: MIT
//
// Hardware probe: does a VA client's file-descriptor count come back to its
// starting value across the lifecycle it repeats? The driver duplicates
// client DMA-BUFs (DRM_PRIME_2 import), allocates a stable buffer per surface
// it is asked to export, and opens both V4L2 nodes; each of those is an fd
// that has to be closed again.
//
// A leak of one fd per surface or per context is invisible in a short run and
// fatal in a long one: Chrome keeps a pool of surfaces for hours, and once
// the process hits its descriptor limit every new surface fails. The count is
// therefore taken from /proc/self/fd, which is the authority — a driver-side
// ledger can only agree with itself.
//
// Usage: iris-fd-leak-probe <render-node> [cycles]
//
// Each cycle: initialize the display, import a client DMA-BUF as a surface,
// export the surface's handle, encode or derive with it, tear everything
// down. The probe asserts that no cycle leaves a descriptor behind, and that
// two consecutive steady-state cycles open the same number.
//
// Not a Meson test: it needs the VA driver on a DRM render node.

#include "../src/iris/alloc.h"
#include "../src/util/error.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

extern "C" {
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
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

// The live descriptor count, read from the kernel rather than tracked here.
int open_fds()
{
    DIR* dir = opendir("/proc/self/fd");
    if (!dir)
        return -1;
    int count = -1; // do not count the directory handle itself
    while (readdir(dir))
        ++count;
    closedir(dir);
    return count;
}

// A client DMA-BUF in the CAPTURE layout, owned by this probe. The driver
// duplicates it at import, so the probe must close its own copy afterwards:
// that duplicate is one of the descriptors under test.
int client_buffer(const iris::Layout& layout)
{
    return iris::allocate_dma_heap(layout.size);
}

// Print where each live descriptor points. A count alone cannot say whether a
// leftover belongs to the driver or to the probe.
void list_open_fds()
{
    DIR* dir = opendir("/proc/self/fd");
    if (!dir)
        return;
    const int self = dirfd(dir);
    while (const dirent* entry = readdir(dir)) {
        const int fd = std::atoi(entry->d_name);
        if (fd == self || fd <= 1)
            continue;
        char link[64];
        char target[256] = {};
        std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        const ssize_t length = readlink(link, target, sizeof target - 1);
        std::printf("  fd %d -> %s\n", fd, length > 0 ? target : "?");
    }
    closedir(dir);
}

// Descriptors that are neither the render node the probe opened itself nor a
// standard stream. Anything else was opened by the driver and left behind.
int count_fds_other_than(int render_fd)
{
    DIR* dir = opendir("/proc/self/fd");
    if (!dir)
        return -1;
    const int self = dirfd(dir);
    int count = 0;
    while (const dirent* entry = readdir(dir)) {
        const int fd = std::atoi(entry->d_name);
        if (fd == self || fd <= 2 || fd == render_fd)
            continue;
        ++count;
    }
    closedir(dir);
    return count;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <render-node> [cycles]\n", argv[0]);
        return 2;
    }
    const int cycles = argc == 3 ? std::atoi(argv[2]) : 8;
    if (cycles < 4) {
        std::fprintf(stderr, "at least 4 cycles are needed to tell a leak from warm-up\n");
        return 2;
    }

    const int drm = open(argv[1], O_RDWR | O_CLOEXEC);
    if (drm < 0) {
        std::perror("open render node");
        return 2;
    }
    VADisplay display = vaGetDisplayDRM(drm);
    int major = 0, minor = 0;
    const VAStatus status = vaInitialize(display, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        std::printf("FAIL vaInitialize status=%#x\n", status);
        return 1;
    }
    std::printf("display %d.%d\n", major, minor);

    const unsigned width = 1280, height = 720;
    const iris::Layout layout = iris::stable_layout(VA_FOURCC_NV12, width, height);

    // Warm-up: the first cycle allocates whatever the driver keeps for the
    // life of the display. Only the steady state is a leak.
    int baseline = 0;
    std::vector<int> counts;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        const int fd = client_buffer(layout);
        if (fd < 0) {
            std::printf("FAIL could not allocate a client DMA-BUF\n");
            return 1;
        }

        // The import path the driver supports: DRM_PRIME_2 in the CAPTURE
        // layout. The driver duplicates this fd and owns the duplicate.
        VASurfaceID surface = VA_INVALID_SURFACE;
        VADRMPRIMESurfaceDescriptor descriptor = {};
        descriptor.fourcc = VA_FOURCC_NV12;
        descriptor.width = width;
        descriptor.height = height;
        descriptor.num_objects = 1;
        descriptor.objects[0].fd = fd;
        descriptor.objects[0].size = layout.size;
        descriptor.objects[0].drm_format_modifier = 0;
        descriptor.num_layers = 1;
        descriptor.layers[0].drm_format = VA_FOURCC_NV12;
        descriptor.layers[0].num_planes = 2;
        for (unsigned plane = 0; plane < 2; ++plane) {
            descriptor.layers[0].object_index[plane] = 0;
            descriptor.layers[0].pitch[plane] = layout.stride;
            descriptor.layers[0].offset[plane] = plane ? layout.chroma_offset() : 0;
        }
        VASurfaceAttrib attributes[2] = {};
        attributes[0].type = VASurfaceAttribMemoryType;
        attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attributes[0].value.type = VAGenericValueTypeInteger;
        attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
        attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
        attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attributes[1].value.type = VAGenericValueTypePointer;
        attributes[1].value.value.p = &descriptor;
        if (vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height, &surface, 1, attributes, 2)
            != VA_STATUS_SUCCESS) {
            std::printf("FAIL could not import a surface in cycle %d\n", cycle);
            close(fd);
            return 1;
        }

        // The paths that can allocate: a derived image aliases the surface's
        // buffer, an export handle duplicates its fd, and a getImage target
        // allocates a stable buffer.
        VAImage image = {};
        if (vaDeriveImage(display, surface, &image) == VA_STATUS_SUCCESS)
            vaDestroyImage(display, image.image_id);

        VADRMPRIMESurfaceDescriptor exported = {};
        if (vaExportSurfaceHandle(display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY, &exported)
            == VA_STATUS_SUCCESS) {
            for (uint32_t object = 0; object < exported.num_objects; ++object)
                close(exported.objects[object].fd);
        }

        vaDestroySurfaces(display, &surface, 1);
        close(fd);
        // Negative control: a leak the probe must catch. Without it a flat
        // count could mean the accounting is broken rather than the driver
        // clean.
        if (std::getenv("IRIS_TEST_FD_LEAK"))
            (void)client_buffer(layout);

        counts.push_back(open_fds());
        if (cycle == 0)
            baseline = counts.back();
    }

    std::printf("open fds per cycle:");
    for (int count : counts)
        std::printf(" %d", count);
    std::printf("\n");

    // Steady state is everything after the first cycle: a driver that keeps a
    // pool allocates it once, and it must not grow per surface.
    int lowest = counts[1], highest = counts[1];
    for (size_t i = 1; i < counts.size(); ++i) {
        if (counts[i] < lowest)
            lowest = counts[i];
        if (counts[i] > highest)
            highest = counts[i];
    }
    std::printf("steady state %d..%d over %zu cycles\n", lowest, highest, counts.size() - 1);
    check(highest == lowest, "the descriptor count is flat across cycles");
    check(highest <= baseline, "no cycle leaves a descriptor behind the first");

    // What is left is this probe's own: the render-node fd it opened before
    // vaInitialize and the standard streams. Report where each one points, so
    // the next reader does not have to guess, and state the expectation as
    // "nothing but the render node" rather than a count.
    vaTerminate(display);
    const int after = open_fds();
    std::printf("after vaTerminate: %d fds\n", after);
    const int driver_left = count_fds_other_than(drm);
    std::printf("descriptors that are not the render node or a standard stream: %d\n", driver_left);
    list_open_fds();
    check(driver_left == 0, "vaTerminate releases every descriptor the driver opened");

    close(drm);
    std::printf("%s fd-leak cycles=%d steady=%d\n", failures ? "FAIL" : "PASS", cycles, highest);
    return failures ? 1 : 0;
}
