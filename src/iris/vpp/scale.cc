// SPDX-License-Identifier: MIT
//
// Vulkan compute backend for the VA video processor. Two surface DMA-BUFs
// (source and destination) are imported as dedicated external memory and the
// pinned nearest-neighbour kernel for the surface's sample width runs over
// them (nv12_scale.spv, or p010_scale.spv for 10-bit surfaces). This is the same
// import path the flat read/write prereq and the per-pixel scale gate prove
// on the target; the kernel is embedded from test/vulkan-vpp/nv12_scale.spv
// and samples the source at swscale's point positions, so `scale_vaapi=w=W:h=H`
// reproduces `scale=WxH:flags=neighbor` byte for byte. Rotation is a clockwise
// quarter turn applied before the scale, as VA specifies, and the same kernel
// reproduces FFmpeg's `transpose` filter. A third kernel, tonemap.spv, converts
// a P010 HDR10 surface to SDR NV12; it does not scale, crop or rotate, and its
// pipeline is written down in test/vulkan-vpp/tonemap.comp. A fourth,
// overlay.spv, composites an overlay surface into one rectangle of the main
// surface with a global alpha, importing three DMA-BUFs instead of two (see
// test/vulkan-vpp/overlay.comp).
//
// Built only when the Vulkan headers are present (meson's optional `vulkan`
// dependency). CI has no Vulkan, so the driver still builds and the VPP
// entrypoint is simply not advertised there.

#include "scale.h"

#ifdef IRIS_HAVE_VULKAN_VPP

#include "nv12_scale_spv.h"
#include "overlay_spv.h"
#include "p010_scale_spv.h"
#include "tonemap_spv.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace iris {
namespace vpp {

namespace {

// Duplicate the fd before import: VkImportMemoryFdInfoKHR takes ownership of
// the descriptor it is given, while the surface keeps its own for the CPU
// mapping and DMA_BUF_IOCTL_SYNC. A duplicate keeps both sides valid.
int dup_fd(int fd)
{
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

// Must mirror the push-constant block in test/vulkan-vpp/nv12_scale.comp.
struct PushParams {
    uint32_t src_stride, src_uv, dst_stride, dst_uv;
    uint32_t src_y, dst_y;
    uint32_t src_x, src_row;
    uint32_t src_width, src_height, dst_width, dst_height;
    uint32_t rotation;
};

// Must mirror the push-constant block in test/vulkan-vpp/tonemap.comp.
struct ToneMapParams {
    uint32_t src_stride, src_uv, dst_stride, dst_uv;
    uint32_t src_y, dst_y;
    uint32_t width, height;
    float src_pq, knee, max_lum;
};

// The memory a job imports, in binding order, independent of what the kernel
// does with it: one, two or three DMA-BUFs.
struct PlaneBuffers {
    static constexpr unsigned kMax = 3;
    int fd[kMax] = { -1, -1, -1 };
    uint32_t size[kMax] = {};
    unsigned count = 0;

    PlaneBuffers(int src_fd, int dst_fd, uint32_t src_size, uint32_t dst_size)
        : fd { src_fd, dst_fd, -1 }
        , size { src_size, dst_size, 0 }
        , count(2)
    {
    }
    PlaneBuffers(int main_fd, int overlay_fd, int dst_fd, uint32_t main_size, uint32_t overlay_size,
        uint32_t dst_size)
        : fd { main_fd, overlay_fd, dst_fd }
        , size { main_size, overlay_size, dst_size }
        , count(3)
    {
    }
};

// Must mirror the push-constant block in test/vulkan-vpp/overlay.comp.
struct OverlayParams {
    uint32_t main_stride, main_uv, main_y;
    uint32_t overlay_stride, overlay_uv, overlay_y;
    uint32_t dst_stride, dst_uv, dst_y;
    uint32_t overlay_width, overlay_height;
    uint32_t x, y, width, height;
    uint32_t dst_width, dst_height;
    uint32_t bytes;
    float alpha;
};

// PQ (SMPTE ST 2084) code for a luminance in cd/m², and the BT.2390
// parameters derived from a source peak and the 100 cd/m² SDR target.
double pq_encode(double nits)
{
    const double m1 = 0.1593017578125, m2 = 78.84375;
    const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const double t = std::pow(std::max(nits, 0.0) / 10000.0, m1);
    return std::pow((c1 + c2 * t) / (1.0 + c3 * t), m2);
}

// A peak below 100 cd/m² needs no compression; the knee is clamped so the
// shader never divides by zero.
void bt2390_params(float peak_nits, float* src_pq, float* knee, float* max_lum)
{
    const double source = pq_encode(std::min(std::max(static_cast<double>(peak_nits), 100.0), 10000.0));
    const double target = pq_encode(100.0);
    const double lum = std::min(target / source, 1.0);
    *src_pq = static_cast<float>(source);
    *max_lum = static_cast<float>(lum);
    *knee = static_cast<float>(std::min(std::max(1.5 * lum - 0.5, 0.0), 0.999));
}

// Import one DMA-BUF as dedicated Vulkan memory bound to a storage buffer.
VkDeviceMemory import_dmabuf(VkDevice device, uint32_t memory_type, VkDeviceSize size, int fd, VkBuffer buffer)
{
    const int owned = dup_fd(fd);
    if (owned < 0)
        return VK_NULL_HANDLE;
    VkImportMemoryFdInfoKHR import = {};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = owned;

    VkMemoryDedicatedAllocateInfo dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = &import;
    dedicated.buffer = buffer;

    VkMemoryAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.pNext = &dedicated;
    alloc.allocationSize = size;
    alloc.memoryTypeIndex = memory_type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        close(owned);
        return VK_NULL_HANDLE;
    }
    vkBindBufferMemory(device, buffer, memory, 0);
    return memory;
}

// One compute queue family that can host the dispatch.
bool compute_family(VkPhysicalDevice physical, uint32_t* family)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    if (!count)
        return false;
    VkQueueFamilyProperties* props = new VkQueueFamilyProperties[count];
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, props);
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            *family = i;
            found = true;
            break;
        }
    }
    delete[] props;
    return found;
}

// A host-visible memory type both external buffers can import from. The
// target's Turnip reports one such type; intersecting the two requirement
// masks refuses a silent mismatch.
uint32_t external_memory_type(VkPhysicalDevice physical, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties props = {};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            return i;
    return UINT32_MAX;
}

// Every Vulkan object the job creates. Destroying this in reverse order is
// the single cleanup path; a partial setup cannot leak a device or an fd.
struct Job {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer buffers[PlaneBuffers::kMax] = {};
    VkDeviceMemory memories[PlaneBuffers::kMax] = {};
    // How many of the slots are in use: 2 for a scale or a tone map, 3 for a
    // composite. The descriptor set, the imports and the cleanup follow it.
    unsigned buffer_count = 0;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    ~Job()
    {
        if (fence)
            vkDestroyFence(device, fence, nullptr);
        if (command_pool)
            vkDestroyCommandPool(device, command_pool, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        for (unsigned i = 0; i < PlaneBuffers::kMax; ++i) {
            if (memories[i])
                vkFreeMemory(device, memories[i], nullptr);
            if (buffers[i])
                vkDestroyBuffer(device, buffers[i], nullptr);
        }
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (pipeline_layout)
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (set_layout)
            vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        if (shader)
            vkDestroyShaderModule(device, shader, nullptr);
        if (device)
            vkDestroyDevice(device, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }

    bool ok(VkResult result) const { return result == VK_SUCCESS; }
};

bool make_job(Job& j, const PlaneBuffers& buffers, const uint32_t* code, size_t code_words, uint32_t push_size,
    VkPhysicalDevice* physical_out, uint32_t* family_out, uint32_t* memory_type_out)
{
    j.buffer_count = buffers.count;
    VkInstanceCreateInfo instance_info = {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    const char* instance_ext[] = { "VK_KHR_external_memory_capabilities", "VK_KHR_get_physical_device_properties2" };
    instance_info.enabledExtensionCount = 2;
    instance_info.ppEnabledExtensionNames = instance_ext;
    if (!j.ok(vkCreateInstance(&instance_info, nullptr, &j.instance)))
        return false;

    // The first call reports how many devices there are; the second writes
    // exactly that many handles. A one-element slot here let a second device
    // land on the next stack word, which happened to be the job's own buffer
    // descriptor, and the picture then failed on a garbage fd.
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(j.instance, &device_count, nullptr);
    if (!device_count)
        return false;
    VkPhysicalDevice* devices = new VkPhysicalDevice[device_count];
    const VkResult enumerated = vkEnumeratePhysicalDevices(j.instance, &device_count, devices);
    if (j.ok(enumerated) && device_count)
        *physical_out = devices[0];
    delete[] devices;
    if (!j.ok(enumerated) || !device_count)
        return false;

    if (!compute_family(*physical_out, family_out))
        return false;

    const char* device_ext[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
        "VK_KHR_dedicated_allocation" };
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = *family_out;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 3;
    device_info.ppEnabledExtensionNames = device_ext;
    if (!j.ok(vkCreateDevice(*physical_out, &device_info, nullptr, &j.device)))
        return false;

    VkShaderModuleCreateInfo shader_info = {};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = code_words * 4;
    shader_info.pCode = code;
    if (!j.ok(vkCreateShaderModule(j.device, &shader_info, nullptr, &j.shader)))
        return false;

    VkDescriptorSetLayoutBinding bindings[PlaneBuffers::kMax] = {};
    for (unsigned i = 0; i < buffers.count; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = buffers.count;
    layout_info.pBindings = bindings;
    if (!j.ok(vkCreateDescriptorSetLayout(j.device, &layout_info, nullptr, &j.set_layout)))
        return false;

    VkPushConstantRange push_range = {};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = push_size;
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &j.set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (!j.ok(vkCreatePipelineLayout(j.device, &pipeline_layout_info, nullptr, &j.pipeline_layout)))
        return false;

    VkComputePipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = j.shader;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = j.pipeline_layout;
    if (!j.ok(vkCreateComputePipelines(j.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &j.pipeline)))
        return false;

    VkExternalMemoryBufferCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.pNext = &external;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    for (unsigned i = 0; i < buffers.count; ++i) {
        info.size = buffers.size[i];
        if (!j.ok(vkCreateBuffer(j.device, &info, nullptr, &j.buffers[i])))
            return false;
    }

    // One memory type has to serve every import: the smallest requirement set
    // is the intersection, and a job whose surfaces disagree has no such type.
    uint32_t bits = UINT32_MAX;
    for (unsigned i = 0; i < buffers.count; ++i) {
        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(j.device, j.buffers[i], &req);
        bits &= req.memoryTypeBits;
    }
    *memory_type_out = external_memory_type(*physical_out, bits);
    if (*memory_type_out == UINT32_MAX)
        return false;

    // Imports run from the last binding back, so the destination is allocated
    // before the sources it reads, as the two-buffer path always did; a
    // failure leaves the destructor to free what was already imported.
    for (unsigned i = buffers.count; i-- > 0;) {
        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(j.device, j.buffers[i], &req);
        j.memories[i] = import_dmabuf(j.device, *memory_type_out, req.size, buffers.fd[i], j.buffers[i]);
        if (!j.memories[i])
            return false;
    }
    return true;
}

// The backend stays free of VA headers, so the clockwise quarter turns are
// plain numbers here (VA_ROTATION_NONE/90/180/270 is 0/1/2/3).
constexpr uint32_t kRotate270 = 3;

bool dispatch(Job& j, const void* push_data, size_t push_size, uint32_t groups_x, uint32_t groups_y, uint32_t family)
{
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(j.device, family, 0, &queue);

    VkDescriptorPoolSize pool_size = {};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = j.buffer_count;
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (!j.ok(vkCreateDescriptorPool(j.device, &pool_info, nullptr, &j.pool)))
        return false;
    VkDescriptorSetAllocateInfo set_info = {};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = j.pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &j.set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!j.ok(vkAllocateDescriptorSets(j.device, &set_info, &set)))
        return false;
    VkDescriptorBufferInfo desc[PlaneBuffers::kMax] = {};
    VkWriteDescriptorSet writes[PlaneBuffers::kMax] = {};
    for (unsigned i = 0; i < j.buffer_count; ++i) {
        desc[i] = { j.buffers[i], 0, VK_WHOLE_SIZE };
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &desc[i];
    }
    vkUpdateDescriptorSets(j.device, j.buffer_count, writes, 0, nullptr);

    VkCommandPoolCreateInfo cp_info = {};
    cp_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_info.queueFamilyIndex = family;
    if (!j.ok(vkCreateCommandPool(j.device, &cp_info, nullptr, &j.command_pool)))
        return false;
    VkCommandBufferAllocateInfo cb_info = {};
    cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_info.commandPool = j.command_pool;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (!j.ok(vkAllocateCommandBuffers(j.device, &cb_info, &command)))
        return false;

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command, &begin);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, j.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, j.pipeline_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(command, j.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, static_cast<uint32_t>(push_size),
        push_data);
    // The grid is rounded up so a geometry that is not a multiple of the 8x8
    // workgroup is still complete; the kernel's own bounds test drops the
    // extra invocations.
    vkCmdDispatch(command, groups_x, groups_y, 1);
    vkEndCommandBuffer(command);

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (!j.ok(vkCreateFence(j.device, &fence_info, nullptr, &j.fence)))
        return false;
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (!j.ok(vkQueueSubmit(queue, 1, &submit, j.fence)))
        return false;
    return vkWaitForFences(j.device, 1, &j.fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
}

} // namespace

bool compiled_in()
{
    return true;
}

bool scale_nv12_or_p010(const ScaleJob& job)
{
    if (job.src_fd < 0 || job.dst_fd < 0 || !job.width || !job.height || !job.src_width || !job.src_height)
        return false;
    // NV12 and P010 carry one chroma pair per 2x2 luma block on both ends, and
    // the word writes assume it. The crop origin has to be even for the same
    // reason: an odd column would land between two chroma samples.
    if ((job.width | job.height | job.src_width | job.src_height | job.src_x | job.src_y_offset) & 1u)
        return false;
    if (job.bytes != 1 && job.bytes != 2)
        return false;
    if (job.rotation > kRotate270)
        return false;

    // One kernel per sample width. Both read the same push-constant block;
    // only the word packing differs.
    const bool wide = job.bytes == 2;
    const PlaneBuffers buffers = { job.src_fd, job.dst_fd, job.src_size, job.dst_size };

    Job j;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = 0, memory_type = 0;
    bool ok = make_job(j, buffers, wide ? kP010ScaleSpv : kNv12ScaleSpv,
        wide ? kP010ScaleSpvWords : kNv12ScaleSpvWords, sizeof(PushParams), &physical, &family, &memory_type);
    if (ok) {
        const PushParams push = { job.src_stride, job.src_uv, job.dst_stride, job.dst_uv, job.src_y, job.dst_y,
            job.src_x, job.src_y_offset, job.src_width, job.src_height, job.width, job.height, job.rotation };
        // One invocation per destination 32-bit word: four luma pixels of one
        // row and two chroma pairs (8-bit), or two luma samples and one chroma
        // pair (P010). The grid covers the luma rows and the half-height
        // chroma plane in one dispatch.
        const uint32_t words = wide ? (job.width + 1) / 2 : (job.width + 3) / 4;
        const uint32_t rows = job.height + job.height / 2;
        ok = dispatch(j, &push, sizeof(push), (words + 7) / 8, (rows + 7) / 8, family);
    }
    return ok;
}

bool tonemap_hdr10_to_sdr(const ToneMapJob& job)
{
    if (job.src_fd < 0 || job.dst_fd < 0 || !job.width || !job.height)
        return false;
    // The kernel writes whole 32-bit words on both planes, which assumes the
    // 4:2:0 geometry the VA layer already enforces.
    if ((job.width | job.height) & 1u)
        return false;

    ToneMapParams push = {};
    push.src_stride = job.src_stride;
    push.src_uv = job.src_uv;
    push.dst_stride = job.dst_stride;
    push.dst_uv = job.dst_uv;
    push.src_y = job.src_y;
    push.dst_y = job.dst_y;
    push.width = job.width;
    push.height = job.height;
    bt2390_params(job.source_peak_nits, &push.src_pq, &push.knee, &push.max_lum);

    const PlaneBuffers buffers = { job.src_fd, job.dst_fd, job.src_size, job.dst_size };
    Job j;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = 0, memory_type = 0;
    bool ok = make_job(j, buffers, kTonemapSpv, kTonemapSpvWords, sizeof(ToneMapParams), &physical, &family,
        &memory_type);
    if (ok)
        // One invocation owns a 4x2 destination block on both planes.
        ok = dispatch(j, &push, sizeof(push), ((job.width + 3) / 4 + 7) / 8, ((job.height + 1) / 2 + 7) / 8, family);
    return ok;
}

bool overlay(const OverlayJob& job)
{
    if (job.main_fd < 0 || job.overlay_fd < 0 || job.dst_fd < 0)
        return false;
    if (!job.dst_width || !job.dst_height || !job.overlay_width || !job.overlay_height || !job.width || !job.height)
        return false;
    if (job.bytes != 1 && job.bytes != 2)
        return false;
    // Word writes assume the 4:2:0 geometry, and an even rectangle keeps every
    // chroma cell wholly inside it or wholly outside it.
    if ((job.dst_width | job.dst_height | job.overlay_width | job.overlay_height | job.x | job.y | job.width
            | job.height)
        & 1u)
        return false;
    if (job.x + job.width > job.dst_width || job.y + job.height > job.dst_height)
        return false;
    if (!(job.alpha >= 0.0f && job.alpha <= 1.0f))
        return false;

    OverlayParams push = {};
    push.main_stride = job.main_stride;
    push.main_uv = job.main_uv;
    push.main_y = job.main_y;
    push.overlay_stride = job.overlay_stride;
    push.overlay_uv = job.overlay_uv;
    push.overlay_y = job.overlay_y;
    push.dst_stride = job.dst_stride;
    push.dst_uv = job.dst_uv;
    push.dst_y = job.dst_y;
    push.overlay_width = job.overlay_width;
    push.overlay_height = job.overlay_height;
    push.x = job.x;
    push.y = job.y;
    push.width = job.width;
    push.height = job.height;
    push.dst_width = job.dst_width;
    push.dst_height = job.dst_height;
    push.bytes = job.bytes;
    push.alpha = job.alpha;

    const PlaneBuffers buffers = { job.main_fd, job.overlay_fd, job.dst_fd, job.main_size, job.overlay_size,
        job.dst_size };
    Job j;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = 0, memory_type = 0;
    bool ok = make_job(j, buffers, kOverlaySpv, kOverlaySpvWords, sizeof(OverlayParams), &physical, &family,
        &memory_type);
    if (ok) {
        // One invocation per destination word, over the luma rows and the
        // half-height chroma plane.
        const uint32_t words = (job.dst_width * job.bytes + 3) / 4;
        const uint32_t rows = job.dst_height + job.dst_height / 2;
        ok = dispatch(j, &push, sizeof(push), (words + 7) / 8, (rows + 7) / 8, family);
    }
    return ok;
}

} // namespace vpp
} // namespace iris

#else // IRIS_HAVE_VULKAN_VPP

namespace iris {
namespace vpp {

bool compiled_in()
{
    return false;
}

bool scale_nv12_or_p010(const ScaleJob&)
{
    return false;
}

bool tonemap_hdr10_to_sdr(const ToneMapJob&)
{
    return false;
}

bool overlay(const OverlayJob&)
{
    return false;
}

} // namespace vpp
} // namespace iris

#endif // IRIS_HAVE_VULKAN_VPP
