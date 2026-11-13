// SPDX-License-Identifier: MIT
//
// NV12 scale primitive gate. Before a VA video processor can advertise
// VAProfileNone, the compute kernel that produces its pixels must be proven
// byte-exact against a CPU oracle on the target. This probe dispatches
// nv12_scale.spv over two imported DMA-BUFs and checks every destination byte
// against a nearest-neighbour oracle that uses swscale's point positions, plus
// that the source is unchanged.
//
// The plane geometry is the Iris CAPTURE layout (src/iris/alloc.cc:
// stable_layout -> src/util/prime.cc:plane_layout for NV12): 128-byte stride,
// 32-row luma, 16-row chroma, chroma immediately after luma. Stride, luma and
// chroma offsets, and both visible geometries are push constants, so the
// kernel is the one a VA VPP pipeline would dispatch, not a test with baked
// sizes. Both DMA-BUF origins the driver allocates (MSM GEM PRIME fd and the
// system DMA heap) are gated, exactly like the flat read/write prereq.
//
// Usage: nv12_scale_probe <nv12_scale.spv> <heap|msm> [render-node] [WxH WxH]
// The size pair is [source destination] and defaults to 64x32 128x64.
// Exit 0 only when compute runs, both fds import, and all destination bytes
// match the oracle for that origin. Build and run on the target, never in CI.

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
    unsigned src_stride, src_uv, src_size;
    unsigned dst_stride, dst_uv, dst_size;
};

static struct geometry geometry(unsigned src_w, unsigned src_h, unsigned dst_w, unsigned dst_h)
{
    struct geometry g;
    g.src_w = src_w;
    g.src_h = src_h;
    g.dst_w = dst_w;
    g.dst_h = dst_h;
    g.src_stride = align_up(src_w, 128);
    g.src_uv = g.src_stride * align_up(src_h, 32);
    // stable_layout rounds the total up to 4 KiB; the DMA-BUF the driver
    // hands a Vulkan import has that size, so gate the same object.
    g.src_size = align_up(g.src_uv + g.src_stride * align_up((src_h + 1) / 2, 16), 4096);
    g.dst_stride = align_up(dst_w, 128);
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
    uint32_t src_w, src_h, dst_w, dst_h;
};

// Byte-exact nearest-neighbour oracle over every destination byte. Checks
// luma and both chroma channels. `src` is the CPU view of the source DMA-BUF
// this gate just filled, so a wrong stride/offset/subsample in the shader
// lands on a different pattern byte and fails here.
static unsigned check_pixels(const volatile uint8_t* src, const volatile uint8_t* dst, const struct geometry* g)
{
    unsigned bad = 0;
    const unsigned yinc = point_step(g->src_h, g->dst_h);
    const unsigned xinc = point_step(g->src_w, g->dst_w);
    for (unsigned dy = 0; dy < g->dst_h; dy++) {
        unsigned so = point_pick(dy, yinc) * g->src_stride;
        for (unsigned dx = 0; dx < g->dst_w; dx++) {
            uint8_t want = src[so + point_pick(dx, xinc)];
            uint8_t got = dst[dy * g->dst_stride + dx];
            if (got != want) {
                if (bad < 8)
                    fprintf(stderr, "  luma (%u,%u): got %u want %u\n", dx, dy, got, want);
                bad++;
            }
        }
    }
    const unsigned cyinc = point_step(g->src_h / 2, g->dst_h / 2);
    const unsigned cxinc = point_step(g->src_w / 2, g->dst_w / 2);
    for (unsigned cy = 0; cy < g->dst_h / 2; cy++) {
        unsigned su = g->src_uv + point_pick(cy, cyinc) * g->src_stride;
        unsigned du = g->dst_uv + cy * g->dst_stride;
        for (unsigned cx = 0; cx < g->dst_w / 2; cx++) {
            unsigned pair = su + point_pick(cx, cxinc) * 2;
            uint8_t wu = src[pair], wv = src[pair + 1];
            uint8_t gu = dst[du + cx * 2], gv = dst[du + cx * 2 + 1];
            if (gu != wu || gv != wv) {
                if (bad < 8)
                    fprintf(stderr, "  chroma (%u,%u): got (%u,%u) want (%u,%u)\n", cx, cy, gu, gv, wu, wv);
                bad += (gu != wu) + (gv != wv);
            }
        }
    }
    return bad;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <nv12_scale.spv> <heap|msm> [render-node] [WxH WxH]\n", argv[0]);
        return 2;
    }
    const char* origin = argv[2];
    const char* node = argc >= 4 ? argv[3] : "/dev/dri/renderD128";
    unsigned src_w = 64, src_h = 32, dst_w = 128, dst_h = 64;
    if (argc >= 6 && (sscanf(argv[4], "%ux%u", &src_w, &src_h) != 2
                         || sscanf(argv[5], "%ux%u", &dst_w, &dst_h) != 2)) {
        fprintf(stderr, "FAIL sizes must be WxH WxH\n");
        return 2;
    }
    if (!src_w || !src_h || !dst_w || !dst_h || ((src_w | src_h | dst_w | dst_h) & 1u)) {
        fprintf(stderr, "FAIL sizes must be even and non-zero (NV12)\n");
        return 2;
    }
    const struct geometry g = geometry(src_w, src_h, dst_w, dst_h);

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
    // Distinct byte per address so an offset bug reads the wrong value, and a
    // sentinel in the destination so an unwritten word cannot look correct.
    for (unsigned i = 0; i < g.src_size; i++)
        src[i] = (uint8_t)((i * 37u + 11u) ^ (i >> 5));
    for (unsigned i = 0; i < g.dst_size; i++)
        dst[i] = 0xcdu;
    sync_cpu(src_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    sync_cpu(dst_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

    uint32_t words = 0;
    uint32_t* code = read_spv(argv[1], &words);

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
    // sizes; the destination is twice the source's geometry.
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
    printf("ok   imported src=%uB dst=%uB (NV12) mtype=%u\n", g.src_size, g.dst_size, mtype);

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
        .src_w = g.src_w,
        .src_h = g.src_h,
        .dst_w = g.dst_w,
        .dst_h = g.dst_h };
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    // One invocation per destination word (4 luma pixels, or 2 chroma pairs);
    // the grid covers the luma rows and the half-height chroma plane.
    const unsigned words_per_row = (g.dst_w + 3) / 4;
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
    unsigned changed = 0;
    for (unsigned i = 0; i < g.src_size; i++)
        if (src[i] != (uint8_t)((i * 37u + 11u) ^ (i >> 5)))
            changed++;
    sync_cpu(dst_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    sync_cpu(src_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    if (bad || changed) {
        fprintf(stderr, "FAIL origin=%s pixels_wrong=%u src_changed=%u\n", origin, bad, changed);
        return 1;
    }
    printf("PASS origin=%s scale=%ux%u->%ux%u exact bytes=%u src=unchanged\n", origin, g.src_w, g.src_h,
        g.dst_w, g.dst_h, g.dst_size);

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
