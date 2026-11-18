// SPDX-License-Identifier: MIT
//
// Standalone probe (hardware only): read a UBWC frame the way a GPU client
// would, and compare it with a linear decode of the same picture.
//
// The driver can hand out Q08C surfaces, but nothing in this tree decompresses
// UBWC: EGL does not, and the CPU copy engine has no such format. Turnip does,
// through VK_EXT_image_drm_format_modifier. This probe imports the exported
// Q08C bytes as a DRM-modifier image, copies both planes to linear buffers,
// and compares them with the same picture decoded into NV12. A match means the
// firmware's compressed output is what the driver exported at the offsets it
// named.
//
// The same copy is done once more on a LINEAR image built from the reference
// file. That control runs the identical plane-0 and plane-1 path, so a chroma
// mismatch after both passes is the modifier's, not the comparison's.
//
// Usage: vk-ubwc-read <linear-nv12.bin> <q08c.bin> <width> <height>
//
// Set IRIS_VK_READ_DUMP=<prefix> to keep what the GPU read: the tight
// luma+chroma buffer of each image lands in <prefix>-q08c.bin and
// <prefix>-linear.bin.
//
// The linear file is the driver's NV12 CAPTURE dump (pitch-aligned rows, the
// kernel's layout); the Q08C file is the whole exported buffer, whose size and
// plane offsets are this tree's ubwc_layout() — the same formula the kernel
// sizes the buffer with and the export describes.
//
// Exit status: 0 when every sample matches, 1 on a mismatch, 2 on usage or an
// import failure.

#include "../src/iris/alloc.h"
#include "../src/util/prime.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <vulkan/vulkan.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
}

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

bool read_file(const char* path, std::vector<uint8_t>& bytes, size_t wanted)
{
    FILE* file = std::fopen(path, "rb");
    if (!file)
        return false;
    bytes.resize(wanted ? wanted : 0);
    size_t got = 0;
    if (!bytes.empty())
        got = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return wanted ? got == wanted : true;
}

// The physical memory behind an imported dma-buf is irrelevant to Turnip: the
// modifier is declared by the importer. A system-heap buffer is a valid
// carrier for both the compressed buffer and the reference, and it keeps this
// probe free of the display's GBM allocator.
struct Carrier {
    int fd = -1;
    void* mapping = nullptr;
    size_t size = 0;

    bool reserve(size_t minimum)
    {
        size = minimum;
        fd = iris::allocate_dma_heap(static_cast<unsigned>(size));
        if (fd < 0)
            return false;
        mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) {
            mapping = nullptr;
            return false;
        }
        std::memset(mapping, 0, size);
        return true;
    }

    bool make(const std::vector<uint8_t>& bytes, size_t minimum)
    {
        return reserve(std::max(bytes.size(), minimum))
            && (std::memcpy(mapping, bytes.data(), bytes.size()), true);
    }

    void place(const uint8_t* source, size_t offset, size_t pitch, size_t row_bytes, unsigned rows)
    {
        auto* base = static_cast<uint8_t*>(mapping);
        for (unsigned row = 0; row < rows; ++row)
            std::memcpy(base + offset + row * pitch, source + row * row_bytes,
                std::min(row_bytes, pitch));
    }

    ~Carrier()
    {
        if (mapping)
            munmap(mapping, size);
        if (fd >= 0)
            close(fd);
    }
};

// One imported image and the memory bound to it.
struct Imported {
    Carrier carrier;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;

    bool bind(VkDevice device, VkImage image)
    {
        VkMemoryRequirements requirements = {};
        vkGetImageMemoryRequirements(device, image, &requirements);
        size = requirements.size;
        if (carrier.fd < 0)
            return false;
        VkMemoryDedicatedAllocateInfo dedicated = {};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = image;
        VkImportMemoryFdInfoKHR import = {};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        import.fd = carrier.fd;
        VkMemoryAllocateInfo allocation = {};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.pNext = &import;
        allocation.allocationSize = requirements.size;
        return vkAllocateMemory(device, &allocation, nullptr, &memory) == VK_SUCCESS
            && vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS;
    }
};

VkSubresourceLayout plane_layout_of(VkDevice device, VkImage image, bool chroma)
{
    VkImageSubresource subresource = {};
    subresource.aspectMask = chroma ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT;
    VkSubresourceLayout layout = {};
    vkGetImageSubresourceLayout(device, image, &subresource, &layout);
    return layout;
}

// Copy both planes of an imported image into one host-visible buffer, tight:
// luma first, chroma right after it.
bool readback(VkDevice device, VkQueue queue, VkCommandPool pool, VkImage image, unsigned width,
    unsigned height, VkBuffer buffer)
{
    const VkDeviceSize luma_bytes = static_cast<VkDeviceSize>(width) * height;
    VkCommandBufferAllocateInfo command_info = {};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &command_info, &command);
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command, &begin);
    for (uint32_t plane = 0; plane < 2; ++plane) {
        VkImageSubresourceRange range = {};
        range.aspectMask = plane ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
            nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region = {};
        region.bufferOffset = plane ? luma_bytes : 0;
        region.imageSubresource.aspectMask = range.aspectMask;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { plane ? (width + 1) / 2 : width, plane ? (height + 1) / 2 : height, 1 };
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    }
    vkEndCommandBuffer(command);
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    const bool ok = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS
        && vkQueueWaitIdle(queue) == VK_SUCCESS;
    vkFreeCommandBuffers(device, pool, 1, &command);
    return ok;
}

// The linear dump is pitch-aligned, the read-back is tight: compare row by
// row, luma against luma and the interleaved chroma against chroma.
struct Difference {
    size_t luma = 0;
    size_t chroma = 0;
};

Difference compare(const std::vector<uint8_t>& reference, const iris::PlaneLayout& linear,
    const uint8_t* got, unsigned width, unsigned height)
{
    Difference differ;
    for (unsigned row = 0; row < height; ++row) {
        const uint8_t* want = reference.data() + static_cast<size_t>(row) * linear.stride;
        for (unsigned column = 0; column < width; ++column)
            differ.luma += want[column] != got[static_cast<size_t>(row) * width + column];
    }
    const uint8_t* want_chroma = reference.data() + linear.chroma_offset;
    const VkDeviceSize luma_bytes = static_cast<VkDeviceSize>(width) * height;
    const size_t chroma_pitch = static_cast<size_t>((width + 1) / 2) * 2;
    for (unsigned row = 0; row < (height + 1) / 2; ++row) {
        const uint8_t* want = want_chroma + static_cast<size_t>(row) * linear.stride;
        const uint8_t* have = got + luma_bytes + static_cast<size_t>(row) * chroma_pitch;
        for (size_t column = 0; column < chroma_pitch; ++column)
            differ.chroma += want[column] != have[column];
    }
    return differ;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <linear-nv12.bin> <q08c.bin> <width> <height>\n", argv[0]);
        return 2;
    }
    const unsigned width = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
    const unsigned height = static_cast<unsigned>(std::strtoul(argv[4], nullptr, 10));
    if (!width || !height) {
        std::fprintf(stderr, "width and height must be non-zero\n");
        return 2;
    }

    const iris::PlaneLayout linear = iris::plane_layout(VA_FOURCC_NV12, width, height);
    const iris::PlaneLayout ubwc = iris::ubwc_layout(width, height);
    std::vector<uint8_t> reference;
    std::vector<uint8_t> compressed;
    if (!read_file(argv[1], reference, linear.size)) {
        std::fprintf(stderr, "%s is not a %ux%u NV12 CAPTURE frame (%u bytes)\n", argv[1], width, height,
            linear.size);
        return 2;
    }
    if (!read_file(argv[2], compressed, ubwc.size)) {
        std::fprintf(stderr, "%s is not a %ux%u Q08C frame (%u bytes)\n", argv[2], width, height, ubwc.size);
        return 2;
    }
    std::printf("INFO linear=%s %u bytes, q08c=%s %u bytes\n", argv[1], linear.size, argv[2], ubwc.size);

    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info = {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL Vulkan instance\n");
        return 2;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (devices.empty()) {
        std::fprintf(stderr, "FAIL no Vulkan device\n");
        return 2;
    }
    VkPhysicalDevice physical = devices[0];
    VkPhysicalDeviceProperties properties = {};
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::printf("INFO device=%s\n", properties.deviceName);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* extensions[] = { "VK_EXT_image_drm_format_modifier", "VK_EXT_external_memory_dma_buf" };
    VkDeviceCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 2;
    device_info.ppEnabledExtensionNames = extensions;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL Vulkan device (Turnip is the reader here)\n");
        return 2;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &pool_info, nullptr, &pool);

    // Both planes land in one host-visible buffer; plane 1 is packed right
    // after the luma samples.
    const VkDeviceSize luma_bytes = static_cast<VkDeviceSize>(width) * height;
    const VkDeviceSize chroma_bytes = static_cast<VkDeviceSize>((width + 1) / 2) * ((height + 1) / 2) * 2;
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = luma_bytes + chroma_bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
    VkMemoryRequirements buffer_requirements = {};
    vkGetBufferMemoryRequirements(device, buffer, &buffer_requirements);
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t type = 0;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((buffer_requirements.memoryTypeBits & (1u << i))
            && (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            type = i;
            break;
        }
    }
    VkMemoryAllocateInfo buffer_allocation = {};
    buffer_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_allocation.allocationSize = buffer_requirements.size;
    buffer_allocation.memoryTypeIndex = type;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &buffer_allocation, nullptr, &buffer_memory) != VK_SUCCESS
        || vkBindBufferMemory(device, buffer, buffer_memory, 0) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL host-visible readback buffer\n");
        return 2;
    }
    void* readback_mapping = nullptr;
    vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &readback_mapping);
    const uint8_t* got = static_cast<const uint8_t*>(readback_mapping);

    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    image_info.extent = { width, height, 1 };
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // 1. The compressed image: Q08C as the driver exported it.
    uint64_t modifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;
    VkImageDrmFormatModifierListCreateInfoEXT modifier_info = {};
    modifier_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    modifier_info.drmFormatModifierCount = 1;
    modifier_info.pDrmFormatModifiers = &modifier;
    VkExternalMemoryImageCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.pNext = &modifier_info;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    image_info.pNext = &external;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL UBWC image: the driver cannot take the modifier\n");
        return 2;
    }

    Imported ubwc_import;
    if (!ubwc_import.carrier.make(compressed, 0) || !ubwc_import.bind(device, image)) {
        std::fprintf(stderr, "FAIL import the exported DMA-BUF\n");
        return 2;
    }
    std::printf("INFO ubwc memory=%llu exported=%u\n",
        static_cast<unsigned long long>(ubwc_import.size), ubwc.size);

    // 2. The control: the same reference bytes, as a linear image. If this
    // matches, plane 0 and plane 1 of the read-back path are sound.
    VkExternalMemoryImageCreateInfo linear_external = {};
    linear_external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    linear_external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo linear_info = image_info;
    linear_info.pNext = &linear_external;
    linear_info.tiling = VK_IMAGE_TILING_LINEAR;
    VkImage linear_image = VK_NULL_HANDLE;
    if (vkCreateImage(device, &linear_info, nullptr, &linear_image) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL linear control image\n");
        return 2;
    }
    const VkSubresourceLayout linear_luma = plane_layout_of(device, linear_image, false);
    const VkSubresourceLayout linear_chroma = plane_layout_of(device, linear_image, true);
    VkMemoryRequirements linear_requirements = {};
    vkGetImageMemoryRequirements(device, linear_image, &linear_requirements);
    Imported linear_import;
    if (!linear_import.carrier.reserve(linear_requirements.size)) {
        std::fprintf(stderr, "FAIL linear control carrier\n");
        return 2;
    }
    linear_import.carrier.place(reference.data(), linear_luma.offset, linear_luma.rowPitch, linear.stride,
        height);
    linear_import.carrier.place(reference.data() + linear.chroma_offset, linear_chroma.offset,
        linear_chroma.rowPitch, linear.stride, (height + 1) / 2);
    if (!linear_import.bind(device, linear_image)) {
        std::fprintf(stderr, "FAIL bind the linear control image\n");
        return 2;
    }

    const VkDeviceSize readback_bytes = luma_bytes + chroma_bytes;
    const char* dump_prefix = std::getenv("IRIS_VK_READ_DUMP");
    if (!readback(device, queue, pool, image, width, height, buffer)) {
        std::fprintf(stderr, "FAIL GPU read-back of the UBWC image\n");
        return 2;
    }
    if (dump_prefix) {
        const std::string path = std::string(dump_prefix) + "-q08c.bin";
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fwrite(got, 1, readback_bytes, file);
            std::fclose(file);
        }
        std::printf("INFO dump=%s %llu bytes\n", path.c_str(), static_cast<unsigned long long>(readback_bytes));
    }
    const Difference compressed_differ = compare(reference, linear, got, width, height);
    std::printf("INFO q08c luma differing samples=%zu/%u chroma differing samples=%zu/%u\n",
        compressed_differ.luma, width * height, compressed_differ.chroma,
        static_cast<unsigned>((width + 1) / 2 * 2 * ((height + 1) / 2)));

    if (!readback(device, queue, pool, linear_image, width, height, buffer)) {
        std::fprintf(stderr, "FAIL GPU read-back of the linear control\n");
        return 2;
    }
    const Difference linear_differ = compare(reference, linear, got, width, height);
    std::printf("INFO linear control luma differing samples=%zu/%u chroma differing samples=%zu/%u\n",
        linear_differ.luma, width * height, linear_differ.chroma,
        static_cast<unsigned>((width + 1) / 2 * 2 * ((height + 1) / 2)));
    if (dump_prefix) {
        const std::string path = std::string(dump_prefix) + "-linear.bin";
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fwrite(got, 1, readback_bytes, file);
            std::fclose(file);
        }
        std::printf("INFO dump=%s %llu bytes\n", path.c_str(), static_cast<unsigned long long>(readback_bytes));
    }

    check(linear_differ.luma == 0 && linear_differ.chroma == 0,
        "the read-back path reproduces a linear image plane for plane");
    check(compressed_differ.luma == 0, "the GPU reads the exported luma exactly as the linear decode");
    check(compressed_differ.chroma == 0, "the GPU reads the exported chroma exactly as the linear decode");

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS UBWC read-back matches the linear decode\n");
    return 0;
}
