// SPDX-License-Identifier: MIT
//
// Vulkan compute backend for the VA video processor. Two surface DMA-BUFs
// (source and destination) are imported as dedicated external memory and the
// pinned NV12 nearest-neighbour kernel runs over them. This is the same
// import path the flat read/write prereq and the per-pixel scale gate prove
// on the target; the kernel is embedded from test/vulkan-vpp/nv12_scale.spv
// and samples the source at swscale's point positions, so `scale_vaapi=w=W:h=H`
// reproduces `scale=WxH:flags=neighbor` byte for byte.
//
// Built only when the Vulkan headers are present (meson's optional `vulkan`
// dependency). CI has no Vulkan, so the driver still builds and the VPP
// entrypoint is simply not advertised there.

#include "scale.h"

#ifdef IRIS_HAVE_VULKAN_VPP

#include "nv12_scale_spv.h"

#include <vulkan/vulkan.h>

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
};

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
    VkBuffer src_buffer = VK_NULL_HANDLE, dst_buffer = VK_NULL_HANDLE;
    VkDeviceMemory src_memory = VK_NULL_HANDLE, dst_memory = VK_NULL_HANDLE;
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
        if (src_memory)
            vkFreeMemory(device, src_memory, nullptr);
        if (dst_memory)
            vkFreeMemory(device, dst_memory, nullptr);
        if (src_buffer)
            vkDestroyBuffer(device, src_buffer, nullptr);
        if (dst_buffer)
            vkDestroyBuffer(device, dst_buffer, nullptr);
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

bool make_job(Job& j, const ScaleJob& job, VkPhysicalDevice* physical_out, uint32_t* family_out,
    uint32_t* memory_type_out)
{
    VkInstanceCreateInfo instance_info = {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    const char* instance_ext[] = { "VK_KHR_external_memory_capabilities", "VK_KHR_get_physical_device_properties2" };
    instance_info.enabledExtensionCount = 2;
    instance_info.ppEnabledExtensionNames = instance_ext;
    if (!j.ok(vkCreateInstance(&instance_info, nullptr, &j.instance)))
        return false;

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(j.instance, &device_count, nullptr);
    if (!device_count || !j.ok(vkEnumeratePhysicalDevices(j.instance, &device_count, physical_out)))
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
    shader_info.codeSize = static_cast<size_t>(kNv12ScaleSpvWords) * 4;
    shader_info.pCode = kNv12ScaleSpv;
    if (!j.ok(vkCreateShaderModule(j.device, &shader_info, nullptr, &j.shader)))
        return false;

    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (unsigned i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 2;
    layout_info.pBindings = bindings;
    if (!j.ok(vkCreateDescriptorSetLayout(j.device, &layout_info, nullptr, &j.set_layout)))
        return false;

    VkPushConstantRange push_range = {};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = sizeof(PushParams);
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
    VkBufferCreateInfo src_info = {};
    src_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    src_info.pNext = &external;
    src_info.size = job.src_size;
    src_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    src_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBufferCreateInfo dst_info = src_info;
    dst_info.size = job.dst_size;
    if (!j.ok(vkCreateBuffer(j.device, &src_info, nullptr, &j.src_buffer))
        || !j.ok(vkCreateBuffer(j.device, &dst_info, nullptr, &j.dst_buffer)))
        return false;

    VkMemoryRequirements src_req = {}, dst_req = {};
    vkGetBufferMemoryRequirements(j.device, j.src_buffer, &src_req);
    vkGetBufferMemoryRequirements(j.device, j.dst_buffer, &dst_req);
    *memory_type_out = external_memory_type(*physical_out, src_req.memoryTypeBits & dst_req.memoryTypeBits);
    if (*memory_type_out == UINT32_MAX)
        return false;

    j.dst_memory = import_dmabuf(j.device, *memory_type_out, dst_req.size, job.dst_fd, j.dst_buffer);
    j.src_memory = import_dmabuf(j.device, *memory_type_out, src_req.size, job.src_fd, j.src_buffer);
    return j.dst_memory && j.src_memory;
}

bool dispatch(Job& j, const ScaleJob& job, uint32_t family, uint32_t memory_type, VkPhysicalDevice physical)
{
    (void)memory_type;
    (void)physical;
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(j.device, family, 0, &queue);

    VkDescriptorPoolSize pool_size = {};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 2;
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
    VkDescriptorBufferInfo src_desc = { j.src_buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo dst_desc = { j.dst_buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet writes[2] = {};
    for (unsigned i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = i ? &dst_desc : &src_desc;
    }
    vkUpdateDescriptorSets(j.device, 2, writes, 0, nullptr);

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
    PushParams push = { job.src_stride, job.src_uv, job.dst_stride, job.dst_uv, job.src_y, job.dst_y,
        job.src_x, job.src_y_offset, job.src_width, job.src_height, job.width, job.height };
    vkCmdPushConstants(command, j.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    // One invocation per destination 32-bit word: four luma pixels of one row,
    // or two chroma pairs of one chroma row. The grid covers the luma rows and
    // the half-height chroma plane in one dispatch, and is rounded up so a
    // geometry that is not a multiple of the 8x8 workgroup is still complete.
    const uint32_t words = (job.width + 3) / 4;
    const uint32_t rows = job.height + job.height / 2;
    vkCmdDispatch(command, (words + 7) / 8, (rows + 7) / 8, 1);
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

bool scale_nv12(const ScaleJob& job)
{
    if (job.src_fd < 0 || job.dst_fd < 0 || !job.width || !job.height || !job.src_width || !job.src_height)
        return false;
    // NV12 carries one chroma pair per 2x2 luma block on both ends, and the
    // word writes assume it. The crop origin has to be even for the same
    // reason: an odd column would land between two chroma samples.
    if ((job.width | job.height | job.src_width | job.src_height | job.src_x | job.src_y_offset) & 1u)
        return false;

    Job j;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t family = 0, memory_type = 0;
    bool ok = make_job(j, job, &physical, &family, &memory_type);
    if (ok)
        ok = dispatch(j, job, family, memory_type, physical);
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

bool scale_nv12(const ScaleJob&)
{
    return false;
}

} // namespace vpp
} // namespace iris

#endif // IRIS_HAVE_VULKAN_VPP
