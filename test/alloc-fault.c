// SPDX-License-Identifier: MIT
//
// Test-only shim: make the MSM GEM allocations that a stable buffer tries
// first fail, so the fallback chain behind them can be exercised on a real
// render node. The chain is in src/iris/msm_alloc.c and src/iris/alloc.cc;
// nothing in it is reachable from a host without an MSM render node.
//
// Build: meson builds this as a shared library. Run the probe as
//
//   LD_PRELOAD=<shim> IRIS_TEST_ALLOC_FAULT=<mode> IRIS_TEST_ALLOC_LOG=<file>
//     iris-alloc-probe <render-node> <expected-origin>
//
// Modes:
//   coherent         MSM_BO_CACHED_COHERENT -> EINVAL, so the driver retries
//                    write-combined. This is the only retry the kernel
//                    contract allows.
//   both             cached-coherent and write-combined -> EINVAL, so the
//                    driver falls through to the system DMA heap.
//   coherent-enomem  cached-coherent -> ENOMEM. A real failure, not an
//                    unsupported cache mode: the driver must NOT retry
//                    write-combined, and must still land on the DMA heap.
//
// Every GEM allocation attempt is appended to IRIS_TEST_ALLOC_LOG so the
// probe can assert which rungs of the chain were actually tried. Without
// that log the shim still injects; it just records nothing.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xf86drm.h>
#include <libdrm/msm_drm.h>

#ifndef MSM_BO_CACHED_COHERENT
#define MSM_BO_CACHED_COHERENT 0x080000
#endif

typedef int (*DrmIoctl)(int, unsigned long, void *);

static void note(const char *line)
{
    const char *path = getenv("IRIS_TEST_ALLOC_LOG");
    if (!path)
        return;
    FILE *log = fopen(path, "a");
    if (!log)
        return;
    fprintf(log, "%s\n", line);
    fclose(log);
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
    DrmIoctl next = (DrmIoctl)dlsym(RTLD_NEXT, "drmIoctl");
    if (!next) {
        errno = ENOSYS;
        return -1;
    }
    const char *mode = getenv("IRIS_TEST_ALLOC_FAULT");
    if (mode && request == DRM_IOCTL_MSM_GEM_NEW && arg) {
        const unsigned flags = ((struct drm_msm_gem_new *)arg)->flags;
        if (flags & MSM_BO_CACHED_COHERENT) {
            if (!strcmp(mode, "coherent") || !strcmp(mode, "both")) {
                note("coherent-einval");
                errno = EINVAL;
                return -1;
            }
            if (!strcmp(mode, "coherent-enomem")) {
                note("coherent-enomem");
                errno = ENOMEM;
                return -1;
            }
        } else if (flags & MSM_BO_WC) {
            if (!strcmp(mode, "both")) {
                note("wc-einval");
                errno = EINVAL;
                return -1;
            }
            const int result = next(fd, request, arg);
            note(result == 0 ? "wc-ok" : "wc-failed");
            return result;
        }
    }
    return next(fd, request, arg);
}
