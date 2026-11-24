// SPDX-License-Identifier: MIT
//
// VPP prerequisite gate. Before a zero-copy Vulkan video processor can read a
// decoded surface and write an output surface, three facts must hold on the
// target:
//
//   1. A compute pipeline from a SPIR-V module runs and its result is exact.
//   2. A DMA-BUF fd -- the form surfaces are exported as -- imports as Vulkan
//      external memory and can be READ by that pipeline (VPP source).
//   3. A second DMA-BUF fd imports the same way and can be WRITTEN by the
//      pipeline, with the bytes visible through a coherent CPU mmap (VPP
//      destination).
//
// The driver allocates stable surfaces from MSM GEM first and falls back to
// the system DMA heap (src/iris/alloc.cc:allocate_stable). Both origins are
// gated because import support can differ between the two allocators. The GEM
// ioctl sequence mirrors src/iris/msm_alloc.c; the shader is a byte
// copy-plus-one so the result is fully predictable and measures the ICD's
// external-memory contract, not colour maths. Both the source and destination
// are imported DMA-BUFs: a gate that wrote to a private Vulkan buffer would
// prove nothing about writing a client surface.
//
// Usage: probe <shader.spv> <heap|msm> [render-node]
//   heap  allocate both DMA-BUFs from /dev/dma_heap/system
//   msm   allocate both as MSM GEM objects on <render-node>, export PRIME fds
//
// One invocation gates one origin; the gate script runs the binary twice.
// Exit 0 only when compute runs, both fds import, the destination reads back
// all 256 words, and the source is unchanged. Build and run on the target,
// never in CI (CI has no GPU).

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

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <shader.spv> <heap|msm> [render-node]\n", argv[0]);
        return 2;
    }
    const char* origin = argv[2];
    const size_t bytes = 256 * sizeof(uint32_t);

    int (*alloc)(size_t, const char*);
    const char* node = argc >= 4 ? argv[3] : "/dev/dri/renderD128";
    if (strcmp(origin, "heap") == 0)
        alloc = NULL;
    else if (strcmp(origin, "msm") == 0)
        alloc = msm_alloc;
    else {
        fprintf(stderr, "FAIL unknown origin: %s\n", origin);
        return 2;
    }

    // Two DMA-BUFs from the same origin: source and destination surfaces.
    int src_fd = alloc ? alloc(bytes, node) : heap_alloc(bytes);
    int dst_fd = alloc ? alloc(bytes, node) : heap_alloc(bytes);
    volatile uint32_t* src_cpu = (volatile uint32_t*)mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, src_fd, 0);
    volatile uint32_t* dst_cpu = (volatile uint32_t*)mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
    if (src_cpu == MAP_FAILED || dst_cpu == MAP_FAILED) {
        perror("mmap dma-buf");
        return 1;
    }
    sync_cpu(src_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    sync_cpu(dst_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    for (uint32_t i = 0; i < 256; i++) {
        src_cpu[i] = 0x11111111u;
        dst_cpu[i] = 0xdeadbeefu;
    }
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
    VkPipelineLayoutCreateInfo plci
        = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
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

    // Both storage buffers are external (DMA-BUF backed): binding 0 is read by
    // the shader, binding 1 is written by it.
    VkExternalMemoryBufferCreateInfo embci = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &embci,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer src, dst;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &src), "vkCreateBuffer(external src)");
    CHECK(vkCreateBuffer(dev, &bci, NULL, &dst), "vkCreateBuffer(external dst)");

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, src, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(devs[0], &mp);
    uint32_t mtype = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i))
            && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mtype = i;
            break;
        }
    VkDeviceMemory msrc = import_dmabuf(dev, mtype, mr, src_fd, src);
    VkDeviceMemory mdst = import_dmabuf(dev, mtype, mr, dst_fd, dst);
    printf("ok   imported two dma-bufs (read+write) mtype=%u\n", mtype);

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
    VkDescriptorBufferInfo sbi = { .buffer = src, .offset = 0, .range = VK_WHOLE_SIZE };
    VkDescriptorBufferInfo dbi = { .buffer = dst, .offset = 0, .range = VK_WHOLE_SIZE };
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
    vkCmdDispatch(cb, 4, 1, 1);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull), "vkWaitForFences");

    // The destination DMA-BUF must show the shader's writes through a coherent
    // CPU mmap; the source must be unchanged.
    sync_cpu(dst_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    sync_cpu(src_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    uint32_t bad_dst = 0, bad_src = 0;
    for (uint32_t i = 0; i < 256; i++) {
        if (dst_cpu[i] != 0x11111112u) {
            if (bad_dst < 4)
                fprintf(stderr, "  dst word %u: got 0x%08x want 0x11111112\n", i, dst_cpu[i]);
            bad_dst++;
        }
        if (src_cpu[i] != 0x11111111u)
            bad_src++;
    }
    sync_cpu(dst_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    sync_cpu(src_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    if (bad_dst || bad_src) {
        fprintf(stderr, "FAIL origin=%s dst_wrong=%u src_changed=%u\n", origin, bad_dst, bad_src);
        return 1;
    }
    printf("PASS origin=%s read-import=exact write-import=exact cpu-coherent=256/256\n", origin);

    vkDestroyFence(dev, fence, NULL);
    vkFreeCommandBuffers(dev, cmdp, 1, &cb);
    vkDestroyCommandPool(dev, cmdp, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL);
    vkFreeMemory(dev, msrc, NULL);
    vkFreeMemory(dev, mdst, NULL);
    vkDestroyBuffer(dev, src, NULL);
    vkDestroyBuffer(dev, dst, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    munmap((void*)src_cpu, bytes);
    munmap((void*)dst_cpu, bytes);
    close(src_fd);
    close(dst_fd);
    free(code);
    free(devs);
    free(qf);
    return 0;
}
