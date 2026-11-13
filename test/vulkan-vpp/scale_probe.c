// SPDX-License-Identifier: MIT
//
// NV12/P010 scale primitive gate. Before a VA video processor can advertise
// VAProfileNone, the compute kernel that produces its pixels must be proven
// byte-exact against a CPU oracle on the target. This probe dispatches the
// scale kernel named on its command line (nv12_scale.spv or p010_scale.spv)
// over two imported DMA-BUFs and checks every destination pixel against an
// oracle that uses swscale's point positions and VA's rotation order, plus that
// the source is unchanged. With --interp 1 the oracle is the four-tap bilinear
// blend the kernel runs instead, compared within one code because GLSL does not
// pin the rounding of that sum.
//
// The plane geometry is the Iris CAPTURE layout (src/iris/alloc.cc:
// stable_layout -> src/util/prime.cc:plane_layout): 128-byte luma pitch for
// NV12 and 256 for P010, 32-row luma, 16-row chroma, chroma immediately after
// luma. Stride, luma and chroma offsets, both visible geometries, the crop
// and the rotation are push constants, so the kernel is the one a VA VPP
// pipeline would dispatch, not a test with baked sizes. Both DMA-BUF origins
// the driver allocates (MSM GEM PRIME fd and the system DMA heap) are gated,
// exactly like the flat read/write prereq.
//
// Usage: scale_probe <scale.spv> <heap|msm> [render-node] [WxH WxH
//                     [CROP_WxH+X+Y]] [--rot 0..3] [--mirror 0..3]
//                     [--interp 0|1] [--p010]
// The size pair is [source destination] and defaults to 64x32 128x64.
// Exit 0 only when compute runs, both fds import, and every destination pixel
// matches the oracle for that origin. Build and run on the target, never in
// CI.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <libdrm/msm_drm.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>

#ifndef MSM_BO_CACHED_COHERENT
#define MSM_BO_CACHED_COHERENT 0x080000
#endif

static unsigned align_up(unsigned v, unsigned a)
{
    return (v + a - 1) & ~(a - 1);
}

struct geometry {
    unsigned src_w, src_h, dst_w, dst_h;
    // The rectangle the kernel samples: the whole source unless a crop is
    // given. src_w/src_h is the surface; crop_w/crop_h the region inside it.
    unsigned crop_x, crop_y, crop_w, crop_h;
    unsigned src_stride, src_uv, src_size;
    unsigned dst_stride, dst_uv, dst_size;
    // Bytes per sample on both ends (1 for NV12, 2 for P010) and the
    // clockwise quarter turns the kernel applies before scaling.
    unsigned bytes;
    unsigned rot;
    // VA_MIRROR_HORIZONTAL as bit 0 and VA_MIRROR_VERTICAL as bit 1, applied to
    // the rotated rectangle and before the scale.
    unsigned mirror;
    // 0 is the point map, 1 the four-tap bilinear blend.
    unsigned interp;
};

// plane_layout in src/util/prime.cc: luma pitch aligned to 128 bytes (256 for
// P010), luma rows to 32, chroma rows to 16, chroma after the luma plane.
static struct geometry geometry(unsigned src_w, unsigned src_h, unsigned dst_w, unsigned dst_h, unsigned bytes)
{
    struct geometry g;
    g.src_w = src_w;
    g.src_h = src_h;
    g.dst_w = dst_w;
    g.dst_h = dst_h;
    g.crop_x = 0;
    g.crop_y = 0;
    g.crop_w = src_w;
    g.crop_h = src_h;
    g.bytes = bytes;
    g.rot = 0;
    g.mirror = 0;
    g.interp = 0;
    g.src_stride = align_up(src_w * bytes, 128 * bytes);
    g.src_uv = g.src_stride * align_up(src_h, 32);
    // stable_layout rounds the total up to 4 KiB; the DMA-BUF the driver
    // hands a Vulkan import has that size, so gate the same object.
    g.src_size = align_up(g.src_uv + g.src_stride * align_up((src_h + 1) / 2, 16), 4096);
    g.dst_stride = align_up(dst_w * bytes, 128 * bytes);
    g.dst_uv = g.dst_stride * align_up(dst_h, 32);
    g.dst_size = align_up(g.dst_uv + g.dst_stride * align_up((dst_h + 1) / 2, 16), 4096);
    return g;
}

// swscale's point map (libswscale/utils.c initFilter, SWS_POINT): the step is
// rounded to 16.16, and a destination sample takes the source index nearest
// the centre of its support. `scale=WxH:flags=neighbor` uses the same integers,
// so the gate's oracle and the FFmpeg client agree by construction.
static unsigned point_step(unsigned src, unsigned dst)
{
    return ((src << 16) + (dst >> 1)) / dst;
}

static unsigned point_pick(unsigned i, unsigned inc)
{
    return (i * inc + (inc >> 1)) >> 16;
}

// VA mirrors after rotating and before scaling, so a point of the rotated
// plane is flipped inside that plane's own size on the way back.
static void mirror_at(unsigned mirror, unsigned size_x, unsigned size_y, unsigned* rx, unsigned* ry)
{
    if (mirror & 1u)
        *rx = size_x - 1 - *rx;
    if (mirror & 2u)
        *ry = size_y - 1 - *ry;
}

// VA rotates the source clockwise before scaling it, so the point map runs on
// the rotated size and this undo takes a point of the rotated plane back to
// the source. A quarter turn sends the source top-left corner to the
// top-right, 180 degrees to the bottom-right, 270 to the bottom-left.
static void unrotate(unsigned rot, unsigned rx, unsigned ry, unsigned w, unsigned h, unsigned* sx, unsigned* sy)
{
    switch (rot) {
    case 1:
        *sx = ry;
        *sy = h - 1 - rx;
        break;
    case 2:
        *sx = w - 1 - rx;
        *sy = h - 1 - ry;
        break;
    case 3:
        *sx = w - 1 - ry;
        *sy = rx;
        break;
    default:
        *sx = rx;
        *sy = ry;
        break;
    }
}

// One sample at a byte offset: an 8-bit byte for NV12, a 16-bit little-endian
// word for P010. A P010 sample carries its ten bits at the top, so comparing
// the whole word also catches a kernel that shifts the payload.
static unsigned sample_at(const volatile uint8_t* p, unsigned off, unsigned bytes)
{
    return bytes == 2 ? (unsigned)p[off] | ((unsigned)p[off + 1] << 8) : p[off];
}

// The ten-bit code a P010 word carries at the top, or the byte itself. The
// bilinear model blends codes, because that is what the kernel blends: a
// padded word is not a number a client reads.
static unsigned code_at(const volatile uint8_t* p, unsigned off, unsigned bytes)
{
    const unsigned w = sample_at(p, off, bytes);
    return bytes == 2 ? w >> 6 : w;
}


// Fill the source with a pattern in which every byte offset holds a different
// code, so an offset bug reads the wrong value. A non-zero `constant` fills the
// whole plane with that one code instead: a scale of a constant picture must
// stay that constant whatever the weights are, which is what proves they sum to
// one rather than merely to something close.
static void fill_source(volatile uint8_t* src, unsigned size, unsigned bytes, unsigned constant)
{
    if (bytes == 2) {
        for (unsigned i = 0; i < size; i += 2) {
            const unsigned sample = constant ? constant : (((i * 37u + 11u) ^ (i >> 5)) & 0x3ffu);
            const unsigned word = sample << 6; // legal P010: low six bits zero
            src[i] = (uint8_t)word;
            src[i + 1] = (uint8_t)(word >> 8);
        }
        return;
    }
    for (unsigned i = 0; i < size; i++)
        src[i] = (uint8_t)(constant ? constant : ((i * 37u + 11u) ^ (i >> 5)));
}

static unsigned source_changed(const volatile uint8_t* src, unsigned size, unsigned bytes, unsigned constant)
{
    unsigned changed = 0;
    if (bytes == 2) {
        for (unsigned i = 0; i < size; i += 2) {
            const unsigned sample = constant ? constant : (((i * 37u + 11u) ^ (i >> 5)) & 0x3ffu);
            const unsigned word = sample << 6;
            if (src[i] != (uint8_t)word || src[i + 1] != (uint8_t)(word >> 8))
                changed++;
        }
        return changed;
    }
    for (unsigned i = 0; i < size; i++)
        if (src[i] != (uint8_t)(constant ? constant : ((i * 37u + 11u) ^ (i >> 5))))
            changed++;
    return changed;
}

static uint32_t* read_spv(const char* path, uint32_t* words)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror("fopen spv");
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t* code = (uint32_t*)malloc((size_t)n);
    if (fread(code, 1, (size_t)n, f) != (size_t)n) {
        perror("fread spv");
        exit(2);
    }
    fclose(f);
    *words = (uint32_t)(n / 4);
    return code;
}

static void sync_cpu(int fd, uint64_t flags)
{
    struct dma_buf_sync req = { .flags = flags };
    while (ioctl(fd, DMA_BUF_IOCTL_SYNC, &req) < 0 && errno == EINTR) { }
}

static int heap_alloc(size_t size)
{
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap < 0) {
        perror("open /dev/dma_heap/system");
        exit(2);
    }
    struct dma_heap_allocation_data d = { .len = size, .fd_flags = O_CLOEXEC | O_RDWR };
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        exit(2);
    }
    close(heap);
    return d.fd;
}

// Same allocation order as src/iris/msm_alloc.c: cached-coherent GEM, then a
// write-combined retry on the two "unsupported" errnos. The GEM handle is
// closed immediately; the exported PRIME fd keeps the object alive.
static int msm_alloc(size_t size, const char* node)
{
    int drm = open(node, O_RDWR | O_CLOEXEC);
    if (drm < 0) {
        perror("open render node");
        exit(2);
    }
    struct drm_msm_gem_new req = { .size = size, .flags = MSM_BO_CACHED_COHERENT };
    if (drmIoctl(drm, DRM_IOCTL_MSM_GEM_NEW, &req) < 0) {
        if (errno != EINVAL && errno != EOPNOTSUPP) {
            perror("DRM_IOCTL_MSM_GEM_NEW");
            exit(2);
        }
        req.flags = MSM_BO_WC;
        if (drmIoctl(drm, DRM_IOCTL_MSM_GEM_NEW, &req) < 0) {
            perror("DRM_IOCTL_MSM_GEM_NEW(WC)");
            exit(2);
        }
    }
    int prime_fd = -1;
    if (drmPrimeHandleToFD(drm, req.handle, DRM_CLOEXEC | DRM_RDWR, &prime_fd) < 0) {
        perror("drmPrimeHandleToFD");
        exit(2);
    }
    struct drm_gem_close close_req = { .handle = req.handle };
    drmIoctl(drm, DRM_IOCTL_GEM_CLOSE, &close_req);
    close(drm);
    return prime_fd;
}

// Import one DMA-BUF as dedicated Vulkan memory bound to a storage buffer.
// The fd is duplicated first: the original stays with the CPU mmap and its
// DMA_BUF_IOCTL_SYNC, so fd-ownership rules of the import cannot close it.
static VkDeviceMemory import_dmabuf(VkDevice dev, uint32_t mtype, VkMemoryRequirements mr, int fd, VkBuffer buffer)
{
    int owned = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owned < 0) {
        perror("dup dma-buf for import");
        exit(2);
    }
    VkImportMemoryFdInfoKHR import = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = owned };
    VkMemoryDedicatedAllocateInfo ded
        = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &import, .buffer = buffer };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &ded,
        .allocationSize = mr.size,
        .memoryTypeIndex = mtype };
    VkDeviceMemory memory;
    VkResult r = vkAllocateMemory(dev, &mai, NULL, &memory);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "FAIL vkAllocateMemory(import dma-buf): VkResult=%d\n", (int)r);
        exit(1);
    }
    vkBindBufferMemory(dev, buffer, memory, 0);
    return memory;
}

#define CHECK(r, what)                                                                                                 \
    do {                                                                                                               \
        if ((r) != VK_SUCCESS) {                                                                                       \
            fprintf(stderr, "FAIL %s: VkResult=%d\n", (what), (int)(r));                                               \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

struct push_params {
    uint32_t src_stride, src_uv, dst_stride, dst_uv;
    uint32_t src_y, dst_y;
    uint32_t src_x, src_row;
    uint32_t src_w, src_h, dst_w, dst_h;
    uint32_t rot;
    uint32_t mirror;
    uint32_t interp;
};

// The byte offset of a source sample of a plane, from a point of the rotated
// plane. The caller mirrors, because the two methods mirror different things:
// the point map reflects the rounded index, the blend reflects the continuous
// position before it rounds. `chroma` selects the half-size plane and `comp`
// the u or v byte of an interleaved pair, so the luma and chroma loops share
// one address path.
static unsigned plane_offset(const struct geometry* g, unsigned chroma, unsigned rx, unsigned ry, unsigned comp)
{
    const unsigned w = chroma ? g->crop_w / 2 : g->crop_w;
    const unsigned h = chroma ? g->crop_h / 2 : g->crop_h;
    unsigned sx, sy;
    unrotate(g->rot, rx, ry, w, h, &sx, &sy);
    if (chroma)
        return g->src_uv + (g->crop_y / 2 + sy) * g->src_stride + g->crop_x * g->bytes + sx * 2 * g->bytes
            + comp * g->bytes;
    return (g->crop_y + sy) * g->src_stride + (g->crop_x + sx) * g->bytes;
}

// The whole sample word at a point of the rotated plane: what the point map
// compares, byte for byte.
static unsigned plane_at(const volatile uint8_t* src, const struct geometry* g, unsigned chroma, unsigned rx,
    unsigned ry, unsigned comp)
{
    return sample_at(src, plane_offset(g, chroma, rx, ry, comp), g->bytes);
}

// The code that sample carries: a P010 word's ten bits at the top, or the byte
// itself. The bilinear model blends these, because that is what the kernel
// blends.
static unsigned code_plane(const volatile uint8_t* src, const struct geometry* g, unsigned chroma, unsigned rx,
    unsigned ry, unsigned comp)
{
    return code_at(src, plane_offset(g, chroma, rx, ry, comp), g->bytes);
}

// The centre of destination sample `d` of `dst` inside a `src`-sized axis,
// clamped to the source's extent. The kernel computes the same expression in
// single precision; the model keeps it in double so the comparison measures
// the kernel and not a copy of its rounding.
static double axis_pos(unsigned d, unsigned dst, unsigned src)
{
    const double v = ((double)d + 0.5) * ((double)src / (double)dst) - 0.5;
    if (v < 0.0)
        return 0.0;
    if (v > (double)(src - 1u))
        return (double)(src - 1u);
    return v;
}

// VA mirrors in the rotated plane, so the reflection applies to the continuous
// position rather than to an already-rounded index.
static double mirror_pos(double v, unsigned size, unsigned on)
{
    return on ? (double)(size - 1u) - v : v;
}

// The four-tap blend of the kernel's bilinear path, in double.
static double plane_blend(const volatile uint8_t* src, const struct geometry* g, unsigned chroma, unsigned comp,
    unsigned dx, unsigned dy, unsigned dw, unsigned dh)
{
    const unsigned w = chroma ? g->crop_w / 2 : g->crop_w;
    const unsigned h = chroma ? g->crop_h / 2 : g->crop_h;
    const unsigned rw = (g->rot & 1u) ? h : w;
    const unsigned rh = (g->rot & 1u) ? w : h;
    const double x = mirror_pos(axis_pos(dx, dw, rw), rw, g->mirror & 1u);
    const double y = mirror_pos(axis_pos(dy, dh, rh), rh, g->mirror & 2u);
    const unsigned x0 = (unsigned)x, y0 = (unsigned)y;
    const unsigned x1 = x0 + 1 < rw ? x0 + 1 : rw - 1;
    const unsigned y1 = y0 + 1 < rh ? y0 + 1 : rh - 1;
    const double fx = x - (double)x0, fy = y - (double)y0;
    // The blend runs on codes, not on the padded words the kernel reads: a
    // P010 word keeps its ten bits at the top, and mixing the words would leave
    // bits in the low six that no client reads and no encoder expects.
    const double a = (1.0 - fx) * (double)code_plane(src, g, chroma, x0, y0, comp)
        + fx * (double)code_plane(src, g, chroma, x1, y0, comp);
    const double b = (1.0 - fx) * (double)code_plane(src, g, chroma, x0, y1, comp)
        + fx * (double)code_plane(src, g, chroma, x1, y1, comp);
    return (1.0 - fy) * a + fy * b;
}

// Per-sample oracle over every destination pixel, luma and both chroma
// channels. `src` is the CPU view of the source DMA-BUF this gate just filled,
// so a wrong stride/offset/subsample/rotation in the shader lands on a
// different pattern sample and fails here. The point map is compared exactly;
// the bilinear path allows one code, because GLSL leaves the implementation
// free to contract the four-tap sum into fused multiply-adds while the model
// sums in double. A structural mistake (a wrong tap, a missing weight, a
// transposed plane, a skipped clamp) moves a sample far further than one code.
static unsigned check_pixels(const volatile uint8_t* src, const volatile uint8_t* dst, const struct geometry* g)
{
    unsigned bad = 0, worst = 0;
    const unsigned bpp = g->bytes;
    const unsigned pair_bytes = 2 * bpp;
    const unsigned full = bpp == 2 ? 1023u : 255u;
    // A quarter turn transposes the sampled rectangle, so the point map runs
    // on that size and each point is then taken back to the source.
    const unsigned rot_w = (g->rot & 1u) ? g->crop_h : g->crop_w;
    const unsigned rot_h = (g->rot & 1u) ? g->crop_w : g->crop_h;
    // The point map is byte-exact. The blend allows one code, because GLSL may
    // contract the four-tap sum into fused multiply-adds while the model sums
    // in double. At 1:1 there is nothing to blend — the centre lands on the
    // sample and the weights are 1 and 0 — so the two methods must agree
    // exactly there, and that case is compared byte for byte.
    const unsigned one_to_one = g->dst_w == rot_w && g->dst_h == rot_h;
    const unsigned tol = (g->interp && !one_to_one) ? 1u : 0u;
    const unsigned xinc = point_step(rot_w, g->dst_w);
    const unsigned yinc = point_step(rot_h, g->dst_h);
    for (unsigned dy = 0; dy < g->dst_h; dy++) {
        for (unsigned dx = 0; dx < g->dst_w; dx++) {
            unsigned want, got;
            if (g->interp) {
                const double v = plane_blend(src, g, 0, 0, dx, dy, g->dst_w, g->dst_h);
                want = (unsigned)(v + 0.5);
                if (want > full)
                    want = full;
                got = code_at(dst, dy * g->dst_stride + dx * bpp, bpp);
            } else {
                unsigned rx = point_pick(dx, xinc), ry = point_pick(dy, yinc);
                mirror_at(g->mirror, rot_w, rot_h, &rx, &ry);
                want = plane_at(src, g, 0, rx, ry, 0);
                got = sample_at(dst, dy * g->dst_stride + dx * bpp, bpp);
            }
            const unsigned diff = got > want ? got - want : want - got;
            if (diff > worst)
                worst = diff;
            if (diff > tol) {
                if (bad < 8)
                    fprintf(stderr, "  luma (%u,%u): got %u want %u\n", dx, dy, got, want);
                bad++;
            }
        }
    }
    const unsigned dw = g->dst_w / 2, dh = g->dst_h / 2;
    const unsigned crw = (g->rot & 1u) ? (g->crop_h / 2) : (g->crop_w / 2);
    const unsigned crh = (g->rot & 1u) ? (g->crop_w / 2) : (g->crop_h / 2);
    const unsigned cxinc = point_step(crw, dw);
    const unsigned cyinc = point_step(crh, dh);
    for (unsigned cy = 0; cy < dh; cy++) {
        for (unsigned cx = 0; cx < dw; cx++) {
            const unsigned d = g->dst_uv + cy * g->dst_stride + cx * pair_bytes;
            const unsigned ru = sample_at(dst, d, bpp);
            const unsigned rv = sample_at(dst, d + bpp, bpp);
            for (unsigned comp = 0; comp < 2; comp++) {
                unsigned want, got;
                if (g->interp) {
                    const double v = plane_blend(src, g, 1, comp, cx, cy, dw, dh);
                    want = (unsigned)(v + 0.5);
                    if (want > full)
                        want = full;
                    got = code_at(dst, d + comp * bpp, bpp);
                } else {
                    unsigned rx = point_pick(cx, cxinc), ry = point_pick(cy, cyinc);
                    mirror_at(g->mirror, crw, crh, &rx, &ry);
                    want = plane_at(src, g, 1, rx, ry, comp);
                    got = comp ? rv : ru;
                }
                const unsigned diff = got > want ? got - want : want - got;
                if (diff > worst)
                    worst = diff;
                if (diff > tol) {
                    if (bad < 8)
                        fprintf(stderr, "  chroma (%u,%u) %c: got %u want %u\n", cx, cy, comp ? 'v' : 'u', got,
                            want);
                    bad++;
                }
            }
        }
    }
    if (worst > tol)
        fprintf(stderr, "  worst deviation %u code(s), tolerance %u\n", worst, tol);
    return bad;
}

int main(int argc, char** argv)
{
    const char* positional[8] = {};
    unsigned npos = 0;
    unsigned bytes = 1, rot = 0, mirror = 0, interp = 0, constant = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--p010") == 0) {
            bytes = 2;
        } else if (strcmp(argv[i], "--rot") == 0 && i + 1 < argc) {
            rot = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--mirror") == 0 && i + 1 < argc) {
            mirror = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--interp") == 0 && i + 1 < argc) {
            interp = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--constant") == 0 && i + 1 < argc) {
            constant = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (npos < sizeof(positional) / sizeof(positional[0])) {
            positional[npos++] = argv[i];
        } else {
            fprintf(stderr, "FAIL too many arguments\n");
            return 2;
        }
    }
    if (npos < 2) {
        fprintf(stderr,
            "usage: %s <scale.spv> <heap|msm> [render-node] [WxH WxH [CROP_WxH+X+Y]] [--rot 0..3] [--mirror 0..3]"
            " [--interp 0|1] [--constant N] [--p010]\n",
            argv[0]);
        return 2;
    }
    if (rot > 3) {
        fprintf(stderr, "FAIL --rot must be 0..3\n");
        return 2;
    }
    if (mirror > 3) {
        fprintf(stderr, "FAIL --mirror must be 0..3\n");
        return 2;
    }
    // The two methods the kernel implements: the point map and the four-tap
    // blend. The driver refuses the third, so the probe has nothing to model.
    if (interp > 1) {
        fprintf(stderr, "FAIL --interp must be 0 or 1\n");
        return 2;
    }
    // A constant picture proves the weights sum to one; the code has to be
    // representable in both sample widths the probe gates.
    if (constant > (bytes == 2 ? 1023u : 255u)) {
        fprintf(stderr, "FAIL --constant must fit the sample width\n");
        return 2;
    }
    const char* origin = positional[1];
    const char* node = npos >= 3 ? positional[2] : "/dev/dri/renderD128";
    unsigned src_w = 64, src_h = 32, dst_w = 128, dst_h = 64;
    if (npos >= 5 && (sscanf(positional[3], "%ux%u", &src_w, &src_h) != 2
                         || sscanf(positional[4], "%ux%u", &dst_w, &dst_h) != 2)) {
        fprintf(stderr, "FAIL sizes must be WxH WxH\n");
        return 2;
    }
    if (!src_w || !src_h || !dst_w || !dst_h || ((src_w | src_h | dst_w | dst_h) & 1u)) {
        fprintf(stderr, "FAIL sizes must be even and non-zero (4:2:0)\n");
        return 2;
    }
    struct geometry g = geometry(src_w, src_h, dst_w, dst_h, bytes);
    g.rot = rot;
    g.mirror = mirror;
    g.interp = interp;
    if (npos >= 6) {
        unsigned crop_w = 0, crop_h = 0, crop_x = 0, crop_y = 0;
        if (sscanf(positional[5], "%ux%u+%u+%u", &crop_w, &crop_h, &crop_x, &crop_y) != 4) {
            fprintf(stderr, "FAIL crop must be WxH+X+Y\n");
            return 2;
        }
        if (!crop_w || !crop_h || ((crop_w | crop_h | crop_x | crop_y) & 1u)
            || crop_x + crop_w > src_w || crop_y + crop_h > src_h) {
            fprintf(stderr, "FAIL crop must be even, non-empty and inside the source\n");
            return 2;
        }
        g.crop_w = crop_w;
        g.crop_h = crop_h;
        g.crop_x = crop_x;
        g.crop_y = crop_y;
    }

    int (*alloc)(size_t, const char*);
    if (strcmp(origin, "heap") == 0)
        alloc = NULL;
    else if (strcmp(origin, "msm") == 0)
        alloc = msm_alloc;
    else {
        fprintf(stderr, "FAIL unknown origin: %s\n", origin);
        return 2;
    }

    int src_fd = alloc ? alloc(g.src_size, node) : heap_alloc(g.src_size);
    int dst_fd = alloc ? alloc(g.dst_size, node) : heap_alloc(g.dst_size);
    volatile uint8_t* src = (volatile uint8_t*)mmap(NULL, g.src_size, PROT_READ | PROT_WRITE, MAP_SHARED, src_fd, 0);
    volatile uint8_t* dst = (volatile uint8_t*)mmap(NULL, g.dst_size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
    if (src == MAP_FAILED || dst == MAP_FAILED) {
        perror("mmap dma-buf");
        return 1;
    }
    sync_cpu(src_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    sync_cpu(dst_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    // Distinct sample per address so an offset bug reads the wrong value, and
    // a sentinel in the destination so an unwritten word cannot look correct.
    fill_source(src, g.src_size, g.bytes, constant);
    for (unsigned i = 0; i < g.dst_size; i++)
        dst[i] = 0xcdu;
    sync_cpu(src_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    sync_cpu(dst_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

    uint32_t words = 0;
    uint32_t* code = read_spv(positional[0], &words);

    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char* iext[] = { "VK_KHR_external_memory_capabilities", "VK_KHR_get_physical_device_properties2" };
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    if (!ndev) {
        fprintf(stderr, "FAIL no Vulkan physical device\n");
        return 1;
    }
    VkPhysicalDevice* devs = (VkPhysicalDevice*)calloc(ndev, sizeof(*devs));
    vkEnumeratePhysicalDevices(inst, &ndev, devs);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(devs[0], &props);
    printf("device=%s api=%u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devs[0], &nq, NULL);
    VkQueueFamilyProperties* qf = (VkQueueFamilyProperties*)calloc(nq, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(devs[0], &nq, qf);
    uint32_t fam = 0;
    int found = 0;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            fam = i;
            found = 1;
            break;
        }
    if (!found) {
        fprintf(stderr, "FAIL no compute queue\n");
        return 1;
    }

    const char* dext[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_KHR_dedicated_allocation" };
    float pri = 1.0f;
    VkDeviceQueueCreateInfo dqi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = fam,
        .queueCount = 1,
        .pQueuePriorities = &pri };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &dqi,
        .enabledExtensionCount = 3,
        .ppEnabledExtensionNames = dext };
    VkDevice dev;
    CHECK(vkCreateDevice(devs[0], &dci, NULL, &dev), "vkCreateDevice(external-memory-fd)");
    VkQueue queue;
    vkGetDeviceQueue(dev, fam, 0, &queue);

    VkShaderModuleCreateInfo smci
        = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)words * 4, .pCode = code };
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm), "vkCreateShaderModule");

    VkDescriptorSetLayoutBinding binds[2] = { { .binding = 0,
                                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                  .descriptorCount = 1,
                                                  .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } };
    VkDescriptorSetLayoutCreateInfo dlci
        = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = binds };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl), "vkCreateDescriptorSetLayout");
    VkPushConstantRange pcr
        = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(struct push_params) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl), "vkCreatePipelineLayout");
    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = sm,
            .pName = "main" },
        .layout = pl };
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe), "vkCreateComputePipelines");

    // Both storage buffers are external (DMA-BUF backed), with the real NV12
    // or P010 sizes; the destination is twice the source's geometry.
    VkExternalMemoryBufferCreateInfo embci = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo sbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &embci,
        .size = g.src_size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBufferCreateInfo dbci = sbci;
    dbci.size = g.dst_size;
    VkBuffer src_buf, dst_buf;
    CHECK(vkCreateBuffer(dev, &sbci, NULL, &src_buf), "vkCreateBuffer(external src)");
    CHECK(vkCreateBuffer(dev, &dbci, NULL, &dst_buf), "vkCreateBuffer(external dst)");

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(devs[0], &mp);
    VkMemoryRequirements smr, dmr;
    vkGetBufferMemoryRequirements(dev, src_buf, &smr);
    vkGetBufferMemoryRequirements(dev, dst_buf, &dmr);
    // One memory type must satisfy both imports (Turnip reports one HOST_VISIBLE
    // external type); intersecting the two bitmasks refuses a silent mismatch.
    uint32_t bits = smr.memoryTypeBits & dmr.memoryTypeBits;
    uint32_t mtype = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mtype = i;
            break;
        }
    VkDeviceMemory mdst = import_dmabuf(dev, mtype, dmr, dst_fd, dst_buf);
    VkDeviceMemory msrc = import_dmabuf(dev, mtype, smr, src_fd, src_buf);
    printf("ok   imported src=%uB dst=%uB (%s) mtype=%u\n", g.src_size, g.dst_size, bytes == 2 ? "P010" : "NV12",
        mtype);

    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps
    };
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dsl };
    VkDescriptorSet ds;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo sbi = { .buffer = src_buf, .offset = 0, .range = VK_WHOLE_SIZE };
    VkDescriptorBufferInfo dbi = { .buffer = dst_buf, .offset = 0, .range = VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = ds,
                                      .dstBinding = 0,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &sbi },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &dbi } };
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

    VkCommandPoolCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = fam };
    VkCommandPool cmdp;
    CHECK(vkCreateCommandPool(dev, &cpci2, NULL, &cmdp), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdp,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &cbbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    struct push_params p = { .src_stride = g.src_stride,
        .src_uv = g.src_uv,
        .dst_stride = g.dst_stride,
        .dst_uv = g.dst_uv,
        .src_y = 0,
        .dst_y = 0,
        .src_x = g.crop_x,
        .src_row = g.crop_y,
        .src_w = g.crop_w,
        .src_h = g.crop_h,
        .dst_w = g.dst_w,
        .dst_h = g.dst_h,
        .rot = g.rot,
        .mirror = g.mirror,
        .interp = g.interp };
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    // One invocation per destination word: four luma pixels and two chroma
    // pairs (NV12), or two luma samples and one chroma pair (P010). The grid
    // covers the luma rows and the half-height chroma plane.
    const unsigned words_per_row = bytes == 2 ? (g.dst_w + 1) / 2 : (g.dst_w + 3) / 4;
    const unsigned rows = g.dst_h + g.dst_h / 2;
    vkCmdDispatch(cb, (words_per_row + 7) / 8, (rows + 7) / 8, 1);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull), "vkWaitForFences");

    sync_cpu(dst_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    sync_cpu(src_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    const unsigned bad = check_pixels(src, dst, &g);
    // The source must be unchanged: the shader reads it only.
    const unsigned changed = source_changed(src, g.src_size, g.bytes, constant);
    sync_cpu(dst_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    sync_cpu(src_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    if (bad || changed) {
        fprintf(stderr, "FAIL origin=%s pixels_wrong=%u src_changed=%u\n", origin, bad, changed);
        return 1;
    }
    // `exact` names the comparison: the point map is byte-exact, the bilinear
    // blend is within one code of its model.
    printf("PASS origin=%s %s rot=%u mirror=%u interp=%s scale=%ux%u->%ux%u crop=%ux%u+%u+%u %s bytes=%u "
           "src=unchanged\n",
        origin, bytes == 2 ? "p010" : "nv12", g.rot, g.mirror, g.interp ? "bilinear" : "nearest", g.src_w, g.src_h,
        g.dst_w, g.dst_h, g.crop_w, g.crop_h, g.crop_x, g.crop_y,
        constant ? "constant-invariant" : g.interp ? "within-1" : "exact", g.dst_size);

    vkDestroyFence(dev, fence, NULL);
    vkFreeCommandBuffers(dev, cmdp, 1, &cb);
    vkDestroyCommandPool(dev, cmdp, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL);
    vkFreeMemory(dev, msrc, NULL);
    vkFreeMemory(dev, mdst, NULL);
    vkDestroyBuffer(dev, src_buf, NULL);
    vkDestroyBuffer(dev, dst_buf, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    munmap((void*)src, g.src_size);
    munmap((void*)dst, g.dst_size);
    close(src_fd);
    close(dst_fd);
    free(code);
    free(devs);
    free(qf);
    return 0;
}
