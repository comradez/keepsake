#include "ksvk.h"

#include <array>
#include <filesystem>
namespace fs = std::filesystem;

// #define IMGUI_ENABLE_FREETYPE
#include <imgui/backends/imgui_impl_glfw.h>
#include <imgui/backends/imgui_impl_vulkan.h>
#include <vulkan/utility/vk_format_utils.h>
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#define VOLK_IMPLEMENTATION
#include <volk.h>

#if defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Don't reorder these headers...
// clang-format off
#include <windows.h>
#include <vulkan/vulkan_win32.h>
// clang-format on
#elif defined(__linux__)

#endif

namespace ks
{
namespace vk
{

//-----------------------------------------------------------------------------
// [Memory allocation]
//-----------------------------------------------------------------------------

VulkanAllocator::VulkanAllocator(const VmaAllocatorCreateInfo &vmaInfo, uint32_t uploadQueueFamilyIndex,
                                 VkQueue uploadQueue)
{
    device = vmaInfo.device;
    vk_check(vmaCreateAllocator(&vmaInfo, &vma));
    this->uploadQueue = uploadQueue;
    VkCommandPoolCreateInfo cmdPoolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cmdPoolCI.queueFamilyIndex = uploadQueueFamilyIndex;
    cmdPoolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vk_check(vkCreateCommandPool(device, &cmdPoolCI, nullptr, &uploadCmdPool));
    uploadCmdBuf = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties physicalDeviceProps;
    vkGetPhysicalDeviceProperties(vmaInfo.physicalDevice, &physicalDeviceProps);
    minUniformBufferOffsetAlignment = physicalDeviceProps.limits.minUniformBufferOffsetAlignment;
    minStorageBufferOffsetAlignment = physicalDeviceProps.limits.minStorageBufferOffsetAlignment;
    minTexelBufferOffsetAlignment = physicalDeviceProps.limits.minTexelBufferOffsetAlignment;
}

void VulkanAllocator::shutdown()
{
    vkDestroyCommandPool(device, uploadCmdPool, nullptr);
    vmaDestroyAllocator(vma);
}

Buffer VulkanAllocator::createBuffer(const VkBufferCreateInfo &info, VmaMemoryUsage usage)
{
    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = usage;
    Buffer buffer;
    vk_check(vmaCreateBuffer(vma, &info, &allocCI, &buffer.buffer, &buffer.allocation, nullptr));
    return buffer;
}

Buffer VulkanAllocator::createBuffer(const VkBufferCreateInfo &info, VmaMemoryUsage usage, const uint8_t *data,
                                     VkCommandBuffer customCmdBuf)
{
    ASSERT(customCmdBuf || uploadCmdBuf);

    Buffer buf = createBuffer(info, usage);

    Buffer staging = createStagingBuffer(info.size, data, info.size);
    VkBufferCopy region;
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = info.size;
    vkCmdCopyBuffer(customCmdBuf ? customCmdBuf : uploadCmdBuf, staging.buffer, buf.buffer, 1, &region);

    return buf;
}

TexelBuffer VulkanAllocator::createTexelBuffer(const VkBufferCreateInfo &info, VmaMemoryUsage usage,
                                               VkBufferViewCreateInfo &bufferViewInfo)
{
    TexelBuffer tb;
    tb.buffer = createBuffer(info, usage);
    bufferViewInfo.buffer = tb.buffer.buffer;
    vk_check(vkCreateBufferView(device, &bufferViewInfo, nullptr, &tb.bufferView));
    return tb;
}

TexelBuffer VulkanAllocator::createTexelBuffer(const VkBufferCreateInfo &info, VmaMemoryUsage usage,
                                               VkBufferViewCreateInfo &bufferViewInfo, const uint8_t *data,
                                               VkCommandBuffer customCmdBuf /*= VK_NULL_HANDLE*/)
{
    TexelBuffer tb;
    tb.buffer = createBuffer(info, usage, data, customCmdBuf);
    bufferViewInfo.buffer = tb.buffer.buffer;
    vk_check(vkCreateBufferView(device, &bufferViewInfo, nullptr, &tb.bufferView));
    return tb;
}

std::byte *VulkanAllocator::map(VmaAllocation allocation)
{
    void *ptr = nullptr;
    vk_check(vmaMapMemory(vma, allocation, &ptr));
    return reinterpret_cast<std::byte *>(ptr);
}

void VulkanAllocator::unmap(VmaAllocation allocation) { vmaUnmapMemory(vma, allocation); }

void VulkanAllocator::flush(VmaAllocation allocation)
{
    vk_check(vmaFlushAllocation(vma, allocation, 0, VK_WHOLE_SIZE));
}

PerFrameBuffer VulkanAllocator::createPerFrameBuffer(const VkBufferCreateInfo &perFrameInfo, VmaMemoryUsage usage,
                                                     uint32_t numFrames)
{
    PerFrameBuffer buf;
    buf.numFrames = numFrames;
    VkDeviceSize alignment = 0;
    if (perFrameInfo.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) {
        alignment = std::max(alignment, minUniformBufferOffsetAlignment);
    }
    if (perFrameInfo.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
        alignment = std::max(alignment, minStorageBufferOffsetAlignment);
    }
    if (perFrameInfo.usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT ||
        perFrameInfo.usage & VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT) {
        alignment = std::max(alignment, minTexelBufferOffsetAlignment);
    }
    buf.perFrameSize = (perFrameInfo.size + alignment - 1) / alignment * alignment;

    VkBufferCreateInfo info = perFrameInfo;
    info.size = buf.perFrameSize * buf.numFrames;
    buf.buffer = createBuffer(info, usage);
    return buf;
}

void VulkanAllocator::flush(const PerFrameBuffer &buffer, uint32_t frameIndex)
{
    ASSERT(frameIndex < buffer.numFrames);
    VkDeviceSize offset = frameIndex * buffer.perFrameSize;
    vk_check(vmaFlushAllocation(vma, buffer.buffer.allocation, offset, buffer.perFrameSize));
}

void VulkanAllocator::beginStagingSession()
{
    ASSERT(!uploadCmdBuf);

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = uploadCmdPool;
    allocInfo.commandBufferCount = 1;

    vk_check(vkAllocateCommandBuffers(device, &allocInfo, &uploadCmdBuf));

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(uploadCmdBuf, &beginInfo));
}

void VulkanAllocator::endStagingSession()
{
    ASSERT(uploadCmdBuf);
    vk_check(vkEndCommandBuffer(uploadCmdBuf));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &uploadCmdBuf;

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vk_check(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    vk_check(vkQueueSubmit(uploadQueue, 1, &submitInfo, fence));
    vk_check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, uploadCmdPool, 1, &uploadCmdBuf);

    uploadCmdBuf = VK_NULL_HANDLE;
    clearStagingBuffers();
}

VkDeviceSize VulkanAllocator::imageSize(const VkImageCreateInfo &info) const
{
    uint32_t numPixels = 0;
    for (uint32_t i = 0, w = info.extent.width, h = info.extent.height; i < info.mipLevels; ++i) {
        numPixels += w * h;
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    return numPixels * vkuFormatElementSize(info.format) * info.arrayLayers;
}

#include <vulkan/utility/vk_format_utils.h>

static VkImageViewCreateInfo viewInfoFromImageInfo(const VkImageCreateInfo &imageInfo, bool cubeMap)
{
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.pNext = nullptr;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    if (imageInfo.imageType == VK_IMAGE_TYPE_2D) {
        if (imageInfo.arrayLayers == 6 && cubeMap) {
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        } else if (imageInfo.arrayLayers > 1) {
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        } else {
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        }
    } else if (imageInfo.imageType == VK_IMAGE_TYPE_1D) {
        if (imageInfo.arrayLayers > 1) {
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        } else {
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_1D;
        }
    } else {
        ASSERT_FATAL("TODO");
    }
    return viewInfo;
}

Image VulkanAllocator::createImage(const VkImageCreateInfo &info, VmaMemoryUsage usage, VmaAllocationCreateFlags flags)
{
    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = usage;
    allocCI.flags = flags;
    Image image;
    vk_check(vmaCreateImage(vma, &info, &allocCI, &image.image, &image.allocation, nullptr));
    image.layout = info.initialLayout;
    return image;
}

ImageWithView VulkanAllocator::createImageWithView(const VkImageCreateInfo &info, VkImageViewCreateInfo &viewInfo,
                                                   VmaMemoryUsage usage)
{
    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = usage;
    ImageWithView image;
    vk_check(vmaCreateImage(vma, &info, &allocCI, &image.image, &image.allocation, nullptr));
    viewInfo.image = image.image;
    vk_check(vkCreateImageView(device, &viewInfo, nullptr, &image.view));
    image.layout = info.initialLayout;
    return image;
}

ImageWithView VulkanAllocator::createImageWithView(const VkImageCreateInfo &info, VmaMemoryUsage usage, bool cubeMap)
{
    VkImageViewCreateInfo viewInfo = viewInfoFromImageInfo(info, cubeMap);
    return createImageWithView(info, viewInfo, usage);
}

ImageWithView VulkanAllocator::createColorBuffer(uint32_t width, uint32_t height, VkFormat format, bool sample,
                                                 bool storage)
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.format = format;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (sample) {
        imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (storage) {
        imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.format = format;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.components = VkComponentMapping{};
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    return createImageWithView(imageInfo, viewInfo, VMA_MEMORY_USAGE_GPU_ONLY);
}

ImageWithView VulkanAllocator::createDepthBuffer(uint32_t width, uint32_t height, bool sample, bool storage)
{
    VkImageCreateInfo depthInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depthInfo.imageType = VK_IMAGE_TYPE_2D;
    depthInfo.extent = {width, height, 1};
    depthInfo.format = VK_FORMAT_D32_SFLOAT;
    depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthInfo.mipLevels = 1;
    depthInfo.arrayLayers = 1;
    depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (sample) {
        depthInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (storage) {
        depthInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }

    VkImageViewCreateInfo depthViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.components = VkComponentMapping{};
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.baseMipLevel = 0;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.baseArrayLayer = 0;
    depthViewInfo.subresourceRange.layerCount = 1;

    return createImageWithView(depthInfo, depthViewInfo, VMA_MEMORY_USAGE_GPU_ONLY);
}

ImageWithView VulkanAllocator::createAndTransitImage(const VkImageCreateInfo &info, VkImageViewCreateInfo &viewInfo,
                                                     VmaMemoryUsage usage, VkImageLayout layout)
{
    ImageWithView image = createImageWithView(info, viewInfo, usage);

    ASSERT(uploadCmdBuf);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = info.initialLayout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = info.arrayLayers;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

    vkCmdPipelineBarrier(uploadCmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    image.layout = layout;
    return image;
}

ImageWithView VulkanAllocator::createAndTransitImage(const VkImageCreateInfo &info, VmaMemoryUsage usage,
                                                     VkImageLayout layout, bool cubeMap)
{
    VkImageViewCreateInfo viewInfo = viewInfoFromImageInfo(info, cubeMap);
    return createAndTransitImage(info, viewInfo, usage, layout);
}

ImageWithView VulkanAllocator::createAndUploadImage(const VkImageCreateInfo &info, VmaMemoryUsage usage,
                                                    const uint8_t *data, size_t byteSize, VkImageLayout layout,
                                                    MipmapOption mipmapOption, bool cubeMap)
{
    ASSERT(uploadCmdBuf);

    VkImageViewCreateInfo viewInfo = viewInfoFromImageInfo(info, cubeMap);
    ImageWithView image = createImageWithView(info, viewInfo, usage);

    Buffer staging = createStagingBuffer(imageSize(info), data, byteSize);

    VkImageMemoryBarrier allToDst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    allToDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    allToDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    allToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    allToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    allToDst.image = image.image;
    allToDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    allToDst.subresourceRange.baseMipLevel = 0;
    allToDst.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    allToDst.subresourceRange.baseArrayLayer = 0;
    allToDst.subresourceRange.layerCount = info.arrayLayers;
    allToDst.srcAccessMask = 0;
    allToDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(uploadCmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &allToDst);

    switch (mipmapOption) {
    //////////////////////////////////////////////////////////////////////////
    case MipmapOption::OnlyAllocate: {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = info.arrayLayers;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = info.extent;
        region.bufferOffset = 0;
        vkCmdCopyBufferToImage(uploadCmdBuf, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);

        VkImageMemoryBarrier allToReady{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        allToReady.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        allToReady.newLayout = layout;
        allToReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        allToReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        allToReady.image = image.image;
        allToReady.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        allToReady.subresourceRange.baseMipLevel = 0;
        allToReady.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        allToReady.subresourceRange.baseArrayLayer = 0;
        allToReady.subresourceRange.layerCount = info.arrayLayers;
        allToReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        allToReady.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(uploadCmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &allToReady);
        break;
    }
    //////////////////////////////////////////////////////////////////////////
    case MipmapOption::PreGenerated: {
        std::vector<VkBufferImageCopy> regions(info.mipLevels);
        for (uint32_t i = 0, w = info.extent.width, h = info.extent.height, offset = 0; i < info.mipLevels; ++i) {
            regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regions[i].imageSubresource.mipLevel = i;
            regions[i].imageSubresource.baseArrayLayer = 0;
            regions[i].imageSubresource.layerCount = info.arrayLayers;
            regions[i].imageOffset = {0, 0, 0};
            regions[i].imageExtent = {w, h, 1};
            regions[i].bufferOffset = offset;
            offset += (w * h) * info.arrayLayers * vkuFormatElementSize(info.format);
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
        }
        vkCmdCopyBufferToImage(uploadCmdBuf, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               (uint32_t)regions.size(), regions.data());

        VkImageMemoryBarrier allToReady{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        allToReady.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        allToReady.newLayout = layout;
        allToReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        allToReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        allToReady.image = image.image;
        allToReady.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        allToReady.subresourceRange.baseMipLevel = 0;
        allToReady.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        allToReady.subresourceRange.baseArrayLayer = 0;
        allToReady.subresourceRange.layerCount = info.arrayLayers;
        allToReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        allToReady.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(uploadCmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &allToReady);
        break;
    }
    //////////////////////////////////////////////////////////////////////////
    case MipmapOption::AutoGenerate: {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = info.arrayLayers;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = info.extent;
        region.bufferOffset = 0;
        vkCmdCopyBufferToImage(uploadCmdBuf, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);

        int mipWidth = (int)info.extent.width;
        int mipHeight = (int)info.extent.height;

        for (uint32_t i = 1; i < info.mipLevels; ++i) {
            VkImageMemoryBarrier lastToSrc = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            lastToSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            lastToSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            lastToSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            lastToSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            lastToSrc.image = image.image;
            lastToSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            lastToSrc.subresourceRange.baseMipLevel = i - 1;
            lastToSrc.subresourceRange.levelCount = 1;
            lastToSrc.subresourceRange.baseArrayLayer = 0;
            lastToSrc.subresourceRange.layerCount = info.arrayLayers;
            lastToSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            lastToSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

            vkCmdPipelineBarrier(uploadCmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &lastToSrc);

            VkImageBlit blit = {};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = i - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = info.arrayLayers;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = i;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = info.arrayLayers;

            vkCmdBlitImage(uploadCmdBuf, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

            VkImageMemoryBarrier lastToReady = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            lastToReady.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            lastToReady.newLayout = layout;
            lastToReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            lastToReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            lastToReady.image = image.image;
            lastToReady.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            lastToReady.subresourceRange.baseMipLevel = i - 1;
            lastToReady.subresourceRange.levelCount = 1;
            lastToReady.subresourceRange.baseArrayLayer = 0;
            lastToReady.subresourceRange.layerCount = info.arrayLayers;
            lastToReady.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            lastToReady.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

            vkCmdPipelineBarrier(uploadCmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &lastToReady);

            if (mipWidth > 1)
                mipWidth /= 2;
            if (mipHeight > 1)
                mipHeight /= 2;
        }

        VkImageMemoryBarrier topToReady = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        topToReady.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        topToReady.newLayout = layout;
        topToReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        topToReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        topToReady.image = image.image;
        topToReady.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        topToReady.subresourceRange.baseMipLevel = info.mipLevels - 1;
        topToReady.subresourceRange.levelCount = 1;
        topToReady.subresourceRange.baseArrayLayer = 0;
        topToReady.subresourceRange.layerCount = info.arrayLayers;
        topToReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        topToReady.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        vkCmdPipelineBarrier(uploadCmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &topToReady);
        break;
    }
    default:
        ASSERT(false);
    }

    image.layout = layout;
    return image;
}

Texture VulkanAllocator::createTexture(const ImageWithView &image, const VkSamplerCreateInfo &samplerInfo)
{
    Texture texture;
    texture.image = image;
    vk_check(vkCreateSampler(device, &samplerInfo, nullptr, &texture.sampler));
    return texture;
}

Buffer VulkanAllocator::createStagingBuffer(VkDeviceSize bufferSize, const uint8_t *data, VkDeviceSize dataSize,
                                            bool autoMapped /*= true*/)
{
    ASSERT(bufferSize >= dataSize);

    VkBufferCreateInfo bufferCI{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferCI.size = bufferSize;
    bufferCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    if (autoMapped) {
        allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    Buffer buffer;
    VmaAllocationInfo info;
    vk_check(vmaCreateBuffer(vma, &bufferCI, &allocCI, &buffer.buffer, &buffer.allocation, &info));
    memcpy(info.pMappedData, (const void *)data, dataSize);

    stagingBuffers.push_back(buffer);

    return buffer;
}

void VulkanAllocator::clearStagingBuffers()
{
    for (auto staging : stagingBuffers) {
        destroy(staging);
    }
    stagingBuffers.clear();
}

void VulkanAllocator::destroy(Buffer &buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(vma, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

void VulkanAllocator::destroy(TexelBuffer &texelBuffer)
{
    vkDestroyBufferView(device, texelBuffer.bufferView, nullptr);
    texelBuffer.bufferView = {};
    destroy(texelBuffer.buffer);
}

void VulkanAllocator::destroy(PerFrameBuffer &perFrameBuffer)
{
    destroy(perFrameBuffer.buffer);
    perFrameBuffer.numFrames = perFrameBuffer.perFrameSize = 0;
}

void VulkanAllocator::destroy(Image &image)
{
    if (image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(vma, image.image, image.allocation);
    }
    image = {};
}

void VulkanAllocator::destroy(ImageWithView &image)
{
    if (image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(vma, image.image, image.allocation);
        vkDestroyImageView(device, image.view, nullptr);
    }
    image = {};
}

void VulkanAllocator::destroy(Texture &texture, bool destroyImage)
{
    vkDestroySampler(device, texture.sampler, nullptr);
    if (destroyImage && texture.image.image != VK_NULL_HANDLE) {
        vkDestroyImageView(device, texture.image.view, nullptr);
        vmaDestroyImage(vma, texture.image.image, texture.image.allocation);
    }

    texture = {};
}

//-----------------------------------------------------------------------------
// [Basic vulkan object management]
//-----------------------------------------------------------------------------

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                        VkDebugUtilsMessageTypeFlagsEXT messageType,
                                                        const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
                                                        void *pUserData)
{
    fprintf(stderr, "%s.\n", pCallbackData->pMessage);
    return VK_FALSE;
}

static void check_required_instance_extensions(const std::vector<const char *> &rexts)
{
    uint32_t availableExtCount = 0;
    vk_check(vkEnumerateInstanceExtensionProperties(nullptr, &availableExtCount, nullptr));
    std::vector<VkExtensionProperties> exts(availableExtCount);
    vk_check(vkEnumerateInstanceExtensionProperties(nullptr, &availableExtCount, exts.data()));

    for (const char *rext : rexts) {
        bool found = false;
        for (const auto &ext : exts) {
            if (strcmp(ext.extensionName, rext) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "Vulkan instance extension not available: [%s].", rext);
            std::abort();
        }
    }
}

static void check_required_instance_layers(const std::vector<const char *> &rlayers)
{
    uint32_t availableLayerCount;
    vkEnumerateInstanceLayerProperties(&availableLayerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(availableLayerCount);
    vkEnumerateInstanceLayerProperties(&availableLayerCount, availableLayers.data());

    for (const char *rlayer : rlayers) {
        bool found = false;
        for (const auto &layer : availableLayers) {
            if (strcmp(layer.layerName, rlayer) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "Vulkan instance layer not available: [%s].", rlayer);
            std::abort();
        }
    }
}

VulkanContextCreateInfo::~VulkanContextCreateInfo()
{
    for (void *data : device_features_data) {
        free(data);
    }
}

void VulkanContextCreateInfo::enable_validation()
{
    instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    instance_layers.push_back("VK_LAYER_KHRONOS_validation");
    validation = true;

    device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
}

void VulkanContextCreateInfo::enable_swapchain() { device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME); }

void VulkanContext::shutdown()
{
    allocator.shutdown();
    vkDestroyDevice(device, nullptr);

    if (validation) {
        auto vkDestroyDebugUtilsMessengerEXT =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        vkDestroyDebugUtilsMessengerEXT(instance, debug_messenger, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
}

void VulkanContext::create_instance(const VulkanContextCreateInfo &info)
{
    vk_check(volkInitialize());

    uint32_t instVersion = 0;
    vk_check(vkEnumerateInstanceVersion(&instVersion));
    uint32_t major = VK_VERSION_MAJOR(instVersion);
    uint32_t minor = VK_VERSION_MINOR(instVersion);
    uint32_t patch = VK_VERSION_PATCH(instVersion);
    printf("Vulkan instance version: %u.%u.%u.\n", major, minor, patch);

    std::vector<const char *> rexts;
    for (const auto &ext : info.instance_extensions) {
        rexts.push_back(ext.c_str());
    }
    check_required_instance_extensions(rexts);

    std::vector<const char *> rlayers;
    for (const auto &layer : info.instance_layers) {
        rlayers.push_back(layer.c_str());
    }
    check_required_instance_layers(rlayers);

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_MAKE_VERSION(info.api_version_major, info.api_version_minor, 0);

    VkInstanceCreateInfo instanceCI{};
    instanceCI.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCI.pApplicationInfo = &appInfo;
    instanceCI.enabledExtensionCount = (uint32_t)rexts.size();
    instanceCI.ppEnabledExtensionNames = rexts.data();
    instanceCI.enabledLayerCount = (uint32_t)rlayers.size();
    instanceCI.ppEnabledLayerNames = rlayers.data();

    if (!info.validation) {
        vk_check(vkCreateInstance(&instanceCI, nullptr, &instance));
        validation = false;
    } else {
        VkDebugUtilsMessengerCreateInfoEXT debugMessagerCreateInfo{};
        debugMessagerCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugMessagerCreateInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugMessagerCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugMessagerCreateInfo.pfnUserCallback = vk_debug_callback;
        debugMessagerCreateInfo.pUserData = nullptr;

        instanceCI.pNext = &debugMessagerCreateInfo;
        vk_check(vkCreateInstance(&instanceCI, nullptr, &instance));

        auto vkCreateDebugUtilsMessengerEXT =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        vk_check(vkCreateDebugUtilsMessengerEXT(instance, &debugMessagerCreateInfo, nullptr, &debug_messenger));
        validation = true;
    }

    volkLoadInstance(instance);
}

static bool has_required_device_extensions(VkPhysicalDevice physicalDevice, const std::vector<const char *> &rexts)
{
    uint32_t extCount;
    vk_check(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr));
    std::vector<VkExtensionProperties> availableExts(extCount);
    vk_check(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, availableExts.data()));

    for (const char *rext : rexts) {
        bool currFound = false;
        for (const auto &ext : availableExts) {
            if (strcmp(rext, ext.extensionName) == 0) {
                currFound = true;
                break;
            }
        }
        if (!currFound) {
            return false;
        }
    }
    return true;
}

// TODO: It's much more work to actually check all additional features (VkPhysicalDeviceFeatures2)...
static bool has_required_device_features(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceFeatures &required)
{
    VkPhysicalDeviceFeatures available;
    vkGetPhysicalDeviceFeatures(physicalDevice, &available);

    constexpr uint32_t count = uint32_t(sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32));
    using FeatureArray = std::array<VkBool32, count>;

    FeatureArray requiredArr;
    memcpy(&requiredArr, &required, sizeof(FeatureArray));
    FeatureArray availableArr;
    memcpy(&availableArr, &available, sizeof(FeatureArray));
    for (uint32_t i = 0; i < count; ++i) {
        if (requiredArr[i] && !availableArr[i])
            return false;
    }
    return true;
}

static bool find_all_purpose_queue_family_index(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t &index)
{
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilyProps;
    queueFamilyProps.resize(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilyProps.data());
    for (uint32_t q = 0; q < (uint32_t)queueFamilyProps.size(); ++q) {
        VkBool32 supported =
            queueFamilyProps[q].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT);
        if (surface != VK_NULL_HANDLE) {
            VkBool32 presentSupport = false;
            vk_check(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, q, surface, &presentSupport));
            supported &= presentSupport;
        }
        if (supported) {
            index = q;
            return true;
        }
    }
    return false;
}

std::vector<CompatibleDevice> VulkanContext::query_compatible_devices(const VulkanContextCreateInfo &info,
                                                                      VkSurfaceKHR surface)
{
    std::vector<const char *> rexts;
    for (const std::string &rext : info.device_extensions) {
        rexts.push_back(rext.c_str());
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        fprintf(stderr, "Cannot find any vulkan physical device.\n");
        std::abort();
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    printf("Compatible devices:\n");
    std::vector<CompatibleDevice> compatibles;
    for (uint32_t i = 0; i < (uint32_t)devices.size(); ++i) {
        if (!has_required_device_extensions(devices[i], rexts)) {
            continue;
        }
        if (!has_required_device_features(devices[i], info.device_features.features)) {
            continue;
        }
        uint32_t queueFamilyIndex;
        if (!find_all_purpose_queue_family_index(devices[i], surface, queueFamilyIndex)) {
            continue;
        }

        VkPhysicalDeviceProperties prop;
        vkGetPhysicalDeviceProperties(devices[i], &prop);
        printf("GPU [%u]: %s.\n", i, prop.deviceName);
        compatibles.push_back({devices[i], i, queueFamilyIndex});
    }

    return compatibles;
}

void VulkanContext::create_device(const VulkanContextCreateInfo &info, CompatibleDevice compatible,
                                  VkSurfaceKHR surface)
{
    printf("Selected GPU index: [%u].\n", compatible.physical_device_index);

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        fprintf(stderr, "Cannot find any vulkan physical device.\n");
        std::abort();
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    physical_device = compatible.physical_device;
    vkGetPhysicalDeviceProperties(physical_device, &physical_device_properties);
    vkGetPhysicalDeviceFeatures(physical_device, &physical_device_features);
    main_queue_family_index = compatible.queue_family_index;

    std::vector<const char *> rexts;
    for (const std::string &rext : info.device_extensions) {
        rexts.push_back(rext.c_str());
    }

    VkDeviceQueueCreateInfo queueCI{};
    queueCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCI.queueFamilyIndex = main_queue_family_index;
    queueCI.queueCount = 1;
    constexpr float queuePriority = 1.0f;
    queueCI.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceCI{};
    deviceCI.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCI.pQueueCreateInfos = &queueCI;
    deviceCI.queueCreateInfoCount = 1;
    deviceCI.pEnabledFeatures = nullptr;
    deviceCI.pNext = &info.device_features;
    deviceCI.enabledExtensionCount = (uint32_t)rexts.size();
    deviceCI.ppEnabledExtensionNames = rexts.data();
    deviceCI.enabledLayerCount = 0;

    vk_check(vkCreateDevice(physical_device, &deviceCI, nullptr, &device));
    volkLoadDevice(device);

    vkGetDeviceQueue(device, main_queue_family_index, 0, &main_queue);

    VmaVulkanFunctions vmf_vk_fns{};
    vmf_vk_fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmf_vk_fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo vmaInfo{};
    vmaInfo.pVulkanFunctions = &vmf_vk_fns;
    vmaInfo.physicalDevice = physical_device;
    vmaInfo.device = device;
    vmaInfo.instance = instance;
    allocator = VulkanAllocator(vmaInfo, main_queue_family_index, main_queue);
}

//-----------------------------------------------------------------------------
// [Swap chain]
//-----------------------------------------------------------------------------

Swapchain::Swapchain(const SwapchainCreateInfo &info)
    : physicalDevice(info.physicalDevice), device(info.device), queue(info.queue), surface(info.surface),
      maxFramesAhead(info.maxFramesAhead), renderAheadIndex(0)
{
    presentCompleteSemaphores.resize(maxFramesAhead);
    renderCompleteSemaphores.resize(maxFramesAhead);
    inFlightFences.resize(maxFramesAhead);

    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < maxFramesAhead; i++) {
        vk_check(vkCreateSemaphore(info.device, &semaphoreInfo, nullptr, &presentCompleteSemaphores[i]));
        vk_check(vkCreateSemaphore(info.device, &semaphoreInfo, nullptr, &renderCompleteSemaphores[i]));
        vk_check(vkCreateFence(info.device, &fenceInfo, nullptr, &inFlightFences[i]));
    }

    createSwapchainAndImages(info.width, info.height);
}

void Swapchain::shutdown()
{
    destroySwapchainAndImages();

    for (uint32_t i = 0; i < maxFramesAhead; i++) {
        vkDestroySemaphore(device, renderCompleteSemaphores[i], nullptr);
        vkDestroySemaphore(device, presentCompleteSemaphores[i], nullptr);
        vkDestroyFence(device, inFlightFences[i], nullptr);
    }
}

void Swapchain::createSwapchainAndImages(uint32_t width, uint32_t height)
{
    VkSurfaceCapabilitiesKHR capabilities;
    vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities));

    std::vector<VkSurfaceFormatKHR> allFormats;
    uint32_t formatCount = 0;
    vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr));
    ASSERT_FATAL(formatCount > 0);
    allFormats.resize(formatCount);
    vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, allFormats.data()));

    std::vector<VkPresentModeKHR> allPresentModes;
    uint32_t presentModeCount = 0;
    vk_check(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr));
    ASSERT_FATAL(presentModeCount > 0);
    allPresentModes.resize(presentModeCount);
    vk_check(
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, allPresentModes.data()));

    VkSurfaceFormatKHR surfaceFormat;
    surfaceFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
    surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    bool fmtSupported = false;
    for (const auto &fmt : allFormats) {
        if (fmt.format == surfaceFormat.format && fmt.colorSpace == surfaceFormat.colorSpace) {
            fmtSupported = true;
            break;
        }
    }
    ASSERT_FATAL(fmtSupported, "Requested surface format not compatible!");

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    if (std::find(allPresentModes.begin(), allPresentModes.end(), presentMode) == allPresentModes.end()) {
        presentMode = VK_PRESENT_MODE_FIFO_KHR;
        ASSERT_FATAL(std::find(allPresentModes.begin(), allPresentModes.end(), presentMode) != allPresentModes.end());
    }

    extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent = {width, height};
        extent.width =
            std::max(capabilities.minImageExtent.width, std::min(capabilities.maxImageExtent.width, extent.width));
        extent.height =
            std::max(capabilities.minImageExtent.height, std::min(capabilities.maxImageExtent.height, extent.height));
    }

    // Simply sticking to this minimum means that we may sometimes have to wait on the driver to complete internal
    // operations before we can acquire another image to render to. Therefore it is recommended to request at least one
    // more image than the minimum:
    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchainCreateInfo{};
    swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainCreateInfo.surface = surface;

    swapchainCreateInfo.minImageCount = imageCount;
    swapchainCreateInfo.imageFormat = surfaceFormat.format;
    swapchainCreateInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapchainCreateInfo.imageExtent = extent;
    swapchainCreateInfo.imageArrayLayers = 1;

    // TODO: Need to change this based on what is rendered before.
    swapchainCreateInfo.imageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // We only use one queue now.

    swapchainCreateInfo.preTransform = capabilities.currentTransform;
    swapchainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainCreateInfo.presentMode = presentMode;
    swapchainCreateInfo.clipped = VK_TRUE;

    swapchainCreateInfo.oldSwapchain = VK_NULL_HANDLE;

    vk_check(vkCreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &swapchain));

    format = surfaceFormat.format;
    extent = extent;
    vk_check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr));
    images.resize(imageCount);
    vk_check(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data()));
    ASSERT_FATAL(maxFramesAhead <= imageCount);

    imageViews.resize(images.size());
    for (uint32_t i = 0; i < (uint32_t)images.size(); i++) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = images[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = format;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;
        vk_check(vkCreateImageView(device, &createInfo, nullptr, &imageViews[i]));
    }
}

void Swapchain::destroySwapchainAndImages()
{
    images.clear();
    for (size_t i = 0; i < imageViews.size(); i++) {
        vkDestroyImageView(device, imageViews[i], nullptr);
    }
    imageViews.clear();
    vkDestroySwapchainKHR(device, swapchain, nullptr);
}

bool Swapchain::acquire()
{
    vk_check(vkWaitForFences(device, 1, &inFlightFences[renderAheadIndex], VK_TRUE, UINT64_MAX));

    VkResult ret = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, presentCompleteSemaphores[renderAheadIndex],
                                         VK_NULL_HANDLE, &frameIndex);
    if (ret == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;
    }
    ASSERT_FATAL(ret == VK_SUCCESS || ret == VK_SUBOPTIMAL_KHR);
    return true;
}

bool Swapchain::submitAndPresent(const std::vector<VkCommandBuffer> &cmdBufs)
{
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    submitInfo.commandBufferCount = (uint32_t)cmdBufs.size();
    submitInfo.pCommandBuffers = cmdBufs.data();

    std::array<VkSemaphore, 1> submitWaitSemaphores = {presentCompleteSemaphores[renderAheadIndex]};
    std::array<VkPipelineStageFlags, 1> submitWaitStages = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = (uint32_t)submitWaitSemaphores.size();
    submitInfo.pWaitSemaphores = submitWaitSemaphores.data();
    submitInfo.pWaitDstStageMask = submitWaitStages.data();
    std::array<VkSemaphore, 1> submitSignalSemaphores = {renderCompleteSemaphores[renderAheadIndex]};
    submitInfo.signalSemaphoreCount = (uint32_t)submitSignalSemaphores.size();
    submitInfo.pSignalSemaphores = submitSignalSemaphores.data();

    vk_check(vkResetFences(device, 1, &inFlightFences[renderAheadIndex]));
    vk_check(vkQueueSubmit(queue, 1, &submitInfo, inFlightFences[renderAheadIndex]));

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

    std::array<VkSemaphore, 1> presentWaitSemaphores = {renderCompleteSemaphores[renderAheadIndex]};
    presentInfo.waitSemaphoreCount = (uint32_t)presentWaitSemaphores.size();
    presentInfo.pWaitSemaphores = presentWaitSemaphores.data();
    std::array<VkSwapchainKHR, 1> swapchains = {swapchain};
    presentInfo.swapchainCount = (uint32_t)swapchains.size();
    presentInfo.pSwapchains = swapchains.data();
    presentInfo.pImageIndices = &frameIndex;
    presentInfo.pResults = nullptr; // Optional

    VkResult ret = vkQueuePresentKHR(queue, &presentInfo);
    if (ret == VK_ERROR_OUT_OF_DATE_KHR || ret == VK_SUBOPTIMAL_KHR) {
        return false;
    } else {
        vk_check(ret);
    }

    renderAheadIndex = (renderAheadIndex + 1) % maxFramesAhead;

    return true;
}

void Swapchain::resize(uint32_t width, uint32_t height)
{
    vk_check(vkDeviceWaitIdle(device));
    uint32_t frameCount = (uint32_t)images.size();

    destroySwapchainAndImages();
    createSwapchainAndImages(width, height);

    ASSERT_FATAL(frameCount == images.size(), "New swapchain has different number of images!");
}

//-----------------------------------------------------------------------------
// [Command buffer management]
//-----------------------------------------------------------------------------

CmdBufManager::CmdBufManager(uint32_t frameCount, uint32_t queueFamilyIndex, VkDevice device) : device(device)
{
    frames.resize(frameCount);
    for (auto &frame : frames) {
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        poolInfo.flags = 0; // Optional
        vk_check(vkCreateCommandPool(device, &poolInfo, nullptr, &frame.pool));
    }
}

void CmdBufManager::shutdown()
{
    for (auto &frame : frames) {
        vkFreeCommandBuffers(device, frame.pool, (uint32_t)frame.cmdBufs.size(), frame.cmdBufs.data());
        vkDestroyCommandPool(device, frame.pool, nullptr);
    }
}

void CmdBufManager::startFrame(uint32_t frameIndex)
{
    this->frameIndex = frameIndex;
    vk_check(vkResetCommandPool(device, frames[frameIndex].pool, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT));
    frames[frameIndex].nextCmdBuf = 0;
}

std::vector<VkCommandBuffer> CmdBufManager::acquireCmdBufs(uint32_t count)
{
    auto &frame = frames[frameIndex];

    uint32_t numAvailable = (uint32_t)frame.cmdBufs.size() - frame.nextCmdBuf;
    if (numAvailable < count) {
        uint32_t oldCount = (uint32_t)frame.cmdBufs.size();
        frame.cmdBufs.resize(oldCount + count - numAvailable);
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = count;
        vk_check(vkAllocateCommandBuffers(device, &allocInfo, &frame.cmdBufs[oldCount]));
    }
    std::vector<VkCommandBuffer> ret;
    ret.insert(ret.end(), frame.cmdBufs.begin() + frame.nextCmdBuf, frame.cmdBufs.begin() + frame.nextCmdBuf + count);
    frame.nextCmdBuf += count;
    return ret;
}

void encodeCmdNow(VkDevice device, uint32_t queueFamilyIndex, VkQueue queue,
                  const std::function<void(VkCommandBuffer)> &func)
{
    VkCommandPoolCreateInfo cmdPoolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cmdPoolCI.queueFamilyIndex = queueFamilyIndex;
    cmdPoolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool cmdPool;
    vk_check(vkCreateCommandPool(device, &cmdPoolCI, nullptr, &cmdPool));

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = cmdPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuf;
    vk_check(vkAllocateCommandBuffers(device, &allocInfo, &cmdBuf));

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmdBuf, &beginInfo));

    func(cmdBuf);

    vk_check(vkEndCommandBuffer(cmdBuf));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vk_check(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    vk_check(vkQueueSubmit(queue, 1, &submitInfo, fence));
    vk_check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);

    vkDestroyCommandPool(device, cmdPool, nullptr);
}

//-----------------------------------------------------------------------------
// [Convenience helper for setting up descriptor sets]
//-----------------------------------------------------------------------------

VkDescriptorPool DescriptorSetHelper::createPool(VkDevice device, uint32_t maxSets) const
{
    std::vector<VkDescriptorPoolSize> poolSizes;
    for (const auto &b : bindings) {
        bool found = false;
        for (auto it = poolSizes.begin(); it != poolSizes.end(); ++it) {
            if (it->type == b.descriptorType) {
                it->descriptorCount += b.descriptorCount * maxSets;
                found = true;
                break;
            }
        }
        if (!found) {
            VkDescriptorPoolSize poolSize;
            poolSize.type = b.descriptorType;
            poolSize.descriptorCount = b.descriptorCount * maxSets;
            poolSizes.push_back(poolSize);
        }
    }

    VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.poolSizeCount = (uint32_t)poolSizes.size();
    poolCI.pPoolSizes = poolSizes.data();
    poolCI.maxSets = maxSets;
    VkDescriptorPool pool;
    vk_check(vkCreateDescriptorPool(device, &poolCI, nullptr, &pool));
    return pool;
}

VkDescriptorSetLayout DescriptorSetHelper::createSetLayout(VkDevice device) const
{
    VkDescriptorSetLayoutCreateInfo setLayoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayoutCI.bindingCount = (uint32_t)bindings.size();
    setLayoutCI.pBindings = bindings.data();
    VkDescriptorSetLayout setLayout;
    vkCreateDescriptorSetLayout(device, &setLayoutCI, nullptr, &setLayout);
    return setLayout;
}

VkWriteDescriptorSet DescriptorSetHelper::makeWrite(VkDescriptorSet dstSet, uint32_t dstBinding) const
{
    VkWriteDescriptorSet writeSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    for (size_t i = 0; i < bindings.size(); i++) {
        if (bindings[i].binding == dstBinding) {
            writeSet.descriptorCount = bindings[i].descriptorCount;
            writeSet.descriptorType = bindings[i].descriptorType;
            writeSet.dstBinding = dstBinding;
            writeSet.dstSet = dstSet;
            writeSet.dstArrayElement = 0;
            return writeSet;
        }
    }
    ASSERT(false, "binding not found");
    return writeSet;
}

VkWriteDescriptorSet DescriptorSetHelper::makeWriteArray(VkDescriptorSet dstSet, uint32_t dstBinding, uint32_t start,
                                                         uint32_t count) const
{
    VkWriteDescriptorSet writeSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    for (size_t i = 0; i < bindings.size(); i++) {
        if (bindings[i].binding == dstBinding) {
            ASSERT(start + count <= bindings[i].descriptorCount);
            writeSet.descriptorCount = count;
            writeSet.descriptorType = bindings[i].descriptorType;
            writeSet.dstBinding = dstBinding;
            writeSet.dstSet = dstSet;
            writeSet.dstArrayElement = start;
            return writeSet;
        }
    }
    ASSERT(false, "binding not found");
    return writeSet;
}

//-----------------------------------------------------------------------------
// [Top wrapper class for graphics services and resources]
//-----------------------------------------------------------------------------

GFX::GFX(const GFXArgs &args)
{
    VulkanContextCreateInfo vkctx_args{};
    vkctx_args.api_version_major = 1;
    vkctx_args.api_version_minor = 3;

    uint32_t glfw_extension_count = 0;
    const char **glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);
    for (uint32_t i = 0; i < glfw_extension_count; ++i) {
        vkctx_args.instance_extensions.push_back(glfw_extensions[i]);
    }
#ifndef NDEBUG
    vkctx_args.enable_validation();
#endif
    vkctx_args.enable_swapchain();

    vkctx.create_instance(vkctx_args);

    vk_check(glfwCreateWindowSurface(vkctx.instance, args.window, nullptr, &surface));

    auto compatibles = vkctx.query_compatible_devices(vkctx_args, surface);
    if (compatibles.empty()) {
        fprintf(stderr, "No compatible vulkan devices.\n");
        std::abort();
    }
    vkctx.create_device(vkctx_args, compatibles[0], surface);

    SwapchainCreateInfo swapchain_args{};
    swapchain_args.physicalDevice = vkctx.physical_device;
    swapchain_args.device = vkctx.device;
    swapchain_args.queue = vkctx.main_queue;
    swapchain_args.surface = surface;
    swapchain_args.width = args.width;
    swapchain_args.height = args.height;
    swapchain_args.maxFramesAhead = 2;
    swapchain = Swapchain(swapchain_args);

    cmdBufManager = CmdBufManager((uint32_t)swapchain.imageViews.size(), vkctx.main_queue_family_index, vkctx.device);
}

void GFX::shutdown()
{
    swapchain.shutdown();
    cmdBufManager.shutdown();
    vkDestroySurfaceKHR(vkctx.instance, surface, nullptr);
    vkctx.shutdown();
}

//-----------------------------------------------------------------------------
// [ImGui integration]
//-----------------------------------------------------------------------------

static void imgui_vk_debug_callback(VkResult result)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "Vulkan error in imgui: %d.\n", result);
    }
}

GUI::GUI(const GUICreateInfo &info) : window(info.window), gfx(info.gfx)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.FontGlobalScale = 1.5f; // TODO: Auto-scaling?

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    // ImGui::StyleColorsClassic();
    // ImGui::GetStyle().ScaleAllSizes(2.0f);

    ImGui_ImplVulkan_LoadFunctions(
        [](const char *function_name, void *user_data) {
            VkInstance instance = (VkInstance)user_data;
            return vkGetInstanceProcAddr(instance, function_name);
        },
        gfx->vkctx.instance);
    ImGui_ImplGlfw_InitForVulkan(info.window, true);

    createRenderPass();
    createFramebuffers();

    std::array<VkDescriptorPoolSize, 11> poolSizes = {{{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
                                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
                                                       {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
                                                       {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
                                                       {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
                                                       {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
                                                       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
                                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
                                                       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
                                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
                                                       {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}}};

    VkDescriptorPoolCreateInfo poolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                      .maxSets = 1,
                                      .poolSizeCount = (uint32_t)poolSizes.size(),
                                      .pPoolSizes = poolSizes.data()};

    vk_check(vkCreateDescriptorPool(info.gfx->vkctx.device, &poolCI, nullptr, &pool));

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = info.gfx->vkctx.instance;
    initInfo.PhysicalDevice = info.gfx->vkctx.physical_device;
    initInfo.Device = info.gfx->vkctx.device;
    initInfo.QueueFamily = info.gfx->vkctx.main_queue_family_index;
    initInfo.Queue = info.gfx->vkctx.main_queue;
    initInfo.MinImageCount = initInfo.ImageCount = (uint32_t)gfx->swapchain.images.size();
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.DescriptorPool = pool;
    initInfo.CheckVkResultFn = imgui_vk_debug_callback;

    ImGui_ImplVulkan_Init(&initInfo, renderPass);

    ImFontConfig font_config;
    font_config.RasterizerDensity = 2.0f;
    io.Fonts->AddFontFromFileTTF((fs::path(DATA_DIR) / "CascadiaCode.ttf").string().c_str(), 16.0f, &font_config);
}

void GUI::createRenderPass()
{
    const Swapchain &swapchain = gfx->swapchain;
    // TODO: Need to change this based on what is rendered before.
    VkAttachmentDescription colorAtt{};
    colorAtt.format = swapchain.format;
    colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    colorAtt.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pColorAttachments = &ref;
    subpass.colorAttachmentCount = 1;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassCI{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassCI.pAttachments = &colorAtt;
    renderPassCI.attachmentCount = 1;
    renderPassCI.pSubpasses = &subpass;
    renderPassCI.subpassCount = 1;
    renderPassCI.pDependencies = &dep;
    renderPassCI.dependencyCount = 1;

    vk_check(vkCreateRenderPass(gfx->vkctx.device, &renderPassCI, nullptr, &renderPass));
}

void GUI::createFramebuffers()
{
    const Swapchain &swapchain = gfx->swapchain;
    framebuffers.resize(swapchain.images.size());
    for (uint32_t i = 0; i < (uint32_t)swapchain.images.size(); ++i) {
        VkFramebufferCreateInfo fbCI{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbCI.attachmentCount = 1;
        fbCI.pAttachments = &swapchain.imageViews[i];
        fbCI.layers = 1;
        fbCI.renderPass = renderPass;
        fbCI.width = swapchain.extent.width;
        fbCI.height = swapchain.extent.height;
        vk_check(vkCreateFramebuffer(gfx->vkctx.device, &fbCI, nullptr, &framebuffers[i]));
    }
}

void GUI::render(VkCommandBuffer cmdBuf)
{
    ImGui::Render();
    ImDrawData *drawData = ImGui::GetDrawData();
    const bool isMinimized = (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f);
    if (isMinimized) {
        return;
    }

    VkRenderPassBeginInfo beginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    beginInfo.renderPass = renderPass;
    beginInfo.framebuffer = framebuffers[gfx->frameIndex()];
    beginInfo.renderArea.extent = gfx->swapchain.extent;
    beginInfo.clearValueCount = 0;
    RenderPassRecorder guiPass(cmdBuf, beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    if (show) {
        ImGui_ImplVulkan_RenderDrawData(drawData, cmdBuf);
    }
}

void GUI::resize()
{
    const Swapchain &swapchain = gfx->swapchain;
    ImGui_ImplVulkan_SetMinImageCount((uint32_t)swapchain.images.size());
    // recreate render pass
    destroyRenderPass();
    createRenderPass();
    // recreate framebuffer
    destroyFramebuffers();
    createFramebuffers();
}

void GUI::shutdown()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    destroyFramebuffers();
    destroyRenderPass();

    vkDestroyDescriptorPool(gfx->vkctx.device, pool, nullptr);
}

void GUI::destroyRenderPass() { vkDestroyRenderPass(gfx->vkctx.device, renderPass, nullptr); }

void GUI::destroyFramebuffers()
{
    for (auto fb : framebuffers) {
        vkDestroyFramebuffer(gfx->vkctx.device, fb, nullptr);
    }
}

void GUI::updateFrame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (update_fn)
        update_fn();
}

} // namespace vk
} // namespace ks