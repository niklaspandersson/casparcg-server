/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: CasparCG Team
 */

#include "../StdAfx.h"

#include "texture.h"
#include "buffer.h"
#include "vk_check.h"

#include <common/bit_depth.h>
#include <common/log.h>

#include <vulkan/vulkan.h>

#include <tbb/parallel_for.h>

#include <algorithm>

namespace caspar { namespace accelerator { namespace vk {

// Format tables matching OGL pattern
// stride: 1=R, 2=RG, 3=RGB, 4=RGBA
// NOTE: Using RGBA format instead of BGRA for Vulkan compute shader compatibility.
// The compute shader uses 'rgba8' storage image format which requires R8G8B8A8_UNORM.
// BGRA swizzling is handled in the shader for PIXEL_FORMAT_BGRA inputs.
static VkFormat FORMAT_8BIT[]  = {VK_FORMAT_UNDEFINED, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
static VkFormat FORMAT_16BIT[] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM};

struct texture::impl
{
    VkDevice          device_         = VK_NULL_HANDLE;
    VkPhysicalDevice  physical_device_ = VK_NULL_HANDLE;
    VkCommandPool     command_pool_   = VK_NULL_HANDLE;
    VkQueue           queue_          = VK_NULL_HANDLE;
    VkImage           image_          = VK_NULL_HANDLE;
    VkDeviceMemory    memory_         = VK_NULL_HANDLE;
    VkImageView       image_view_     = VK_NULL_HANDLE;
    int               width_          = 0;
    int               height_         = 0;
    int               stride_         = 0;
    int               size_           = 0;
    common::bit_depth depth_;
    VkFormat          format_;
    VkImageLayout     current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    impl(const impl&)            = delete;
    impl& operator=(const impl&) = delete;

  public:
    impl(void*             device,
         void*             physical_device,
         void*             command_pool,
         void*             queue,
         int               width,
         int               height,
         int               stride,
         common::bit_depth depth)
        : device_(static_cast<VkDevice>(device))
        , physical_device_(static_cast<VkPhysicalDevice>(physical_device))
        , command_pool_(static_cast<VkCommandPool>(command_pool))
        , queue_(static_cast<VkQueue>(queue))
        , width_(width)
        , height_(height)
        , stride_(stride)
        , depth_(depth)
        , size_(width * height * stride * (depth == common::bit_depth::bit8 ? 1 : 2))
    {
        format_ = (depth_ == common::bit_depth::bit8) ? FORMAT_8BIT[stride_] : FORMAT_16BIT[stride_];

        if (format_ == VK_FORMAT_UNDEFINED) {
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception()
                                   << msg_info("Unsupported texture format (stride=" + std::to_string(stride_) + ")"));
        }

        // Check format properties for storage image support
        static bool format_checked = false;
        if (!format_checked && format_ == VK_FORMAT_R8G8B8A8_UNORM) {
            format_checked = true;
            VkFormatProperties formatProps;
            vkGetPhysicalDeviceFormatProperties(physical_device_, format_, &formatProps);
            CASPAR_LOG(info) << L"[vk::texture] Format R8G8B8A8_UNORM properties:"
                              << L" linearTiling=" << formatProps.linearTilingFeatures
                              << L" optimalTiling=" << formatProps.optimalTilingFeatures
                              << L" buffer=" << formatProps.bufferFeatures
                              << L" STORAGE_IMAGE_BIT=" << VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT
                              << L" hasStorageImage=" << ((formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? L"YES" : L"NO");
        }

        // Create image
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width  = static_cast<uint32_t>(width_);
        imageInfo.extent.height = static_cast<uint32_t>(height_);
        imageInfo.extent.depth  = 1;
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.format        = format_;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_STORAGE_BIT; // Phase 4: Enable compute shader access
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples     = VK_SAMPLE_COUNT_1_BIT;

        VK(vkCreateImage(device_, &imageInfo, nullptr, &image_));

        // Get memory requirements
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device_, image_, &memRequirements);

        // Find suitable memory type (device local)
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memProperties);

        uint32_t memoryTypeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            if ((memRequirements.memoryTypeBits & (1 << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memoryTypeIndex = i;
                break;
            }
        }

        if (memoryTypeIndex == UINT32_MAX) {
            vkDestroyImage(device_, image_, nullptr);
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception()
                                   << msg_info("Failed to find suitable memory type for texture."));
        }

        // Allocate memory
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memRequirements.size;
        allocInfo.memoryTypeIndex = memoryTypeIndex;

        VK(vkAllocateMemory(device_, &allocInfo, nullptr, &memory_));

        // Bind memory to image
        VK(vkBindImageMemory(device_, image_, memory_, 0));

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                           = image_;
        viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                          = format_;
        // Explicit component swizzle for proper color channel mapping
        viewInfo.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;

        VK(vkCreateImageView(device_, &viewInfo, nullptr, &image_view_));
    }

    ~impl()
    {
        if (device_ != VK_NULL_HANDLE) {
            if (image_view_ != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, image_view_, nullptr);
            }
            if (memory_ != VK_NULL_HANDLE) {
                vkFreeMemory(device_, memory_, nullptr);
            }
            if (image_ != VK_NULL_HANDLE) {
                vkDestroyImage(device_, image_, nullptr);
            }
        }
    }

    impl(impl&& other)
        : device_(other.device_)
        , physical_device_(other.physical_device_)
        , command_pool_(other.command_pool_)
        , queue_(other.queue_)
        , image_(other.image_)
        , memory_(other.memory_)
        , image_view_(other.image_view_)
        , width_(other.width_)
        , height_(other.height_)
        , stride_(other.stride_)
        , size_(other.size_)
        , depth_(other.depth_)
        , format_(other.format_)
        , current_layout_(other.current_layout_)
    {
        other.device_     = VK_NULL_HANDLE;
        other.image_      = VK_NULL_HANDLE;
        other.memory_     = VK_NULL_HANDLE;
        other.image_view_ = VK_NULL_HANDLE;
    }

    impl& operator=(impl&& other)
    {
        if (this != &other) {
            // Clean up existing resources
            if (device_ != VK_NULL_HANDLE) {
                if (image_view_ != VK_NULL_HANDLE) {
                    vkDestroyImageView(device_, image_view_, nullptr);
                }
                if (memory_ != VK_NULL_HANDLE) {
                    vkFreeMemory(device_, memory_, nullptr);
                }
                if (image_ != VK_NULL_HANDLE) {
                    vkDestroyImage(device_, image_, nullptr);
                }
            }

            // Move resources
            device_          = other.device_;
            physical_device_ = other.physical_device_;
            command_pool_    = other.command_pool_;
            queue_           = other.queue_;
            image_           = other.image_;
            memory_          = other.memory_;
            image_view_      = other.image_view_;
            width_           = other.width_;
            height_          = other.height_;
            stride_          = other.stride_;
            size_            = other.size_;
            depth_           = other.depth_;
            format_          = other.format_;
            current_layout_  = other.current_layout_;

            other.device_     = VK_NULL_HANDLE;
            other.image_      = VK_NULL_HANDLE;
            other.memory_     = VK_NULL_HANDLE;
            other.image_view_ = VK_NULL_HANDLE;
        }
        return *this;
    }

    VkCommandBuffer begin_single_time_commands()
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool        = command_pool_;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer cmdBuffer;
        VK(vkAllocateCommandBuffers(device_, &allocInfo, &cmdBuffer));

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        VK(vkBeginCommandBuffer(cmdBuffer, &beginInfo));

        return cmdBuffer;
    }

    void end_single_time_commands(VkCommandBuffer cmdBuffer)
    {
        VK(vkEndCommandBuffer(cmdBuffer));

        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &cmdBuffer;

        VK(vkQueueSubmit(queue_, 1, &submitInfo, VK_NULL_HANDLE));
        VK(vkQueueWaitIdle(queue_));

        vkFreeCommandBuffers(device_, command_pool_, 1, &cmdBuffer);
    }

    void transition_image_layout(VkCommandBuffer cmdBuffer, VkImageLayout oldLayout, VkImageLayout newLayout)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = oldLayout;
        barrier.newLayout                       = newLayout;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = image_;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;

        VkPipelineStageFlags sourceStage;
        VkPipelineStageFlags destinationStage;

        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceStage           = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage      = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage           = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage      = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            // PRIORITY 1 FIX: Wait for ALL possible write operations that could have
            // written to this image. With graphics pipelines, data is written via
            // color attachment output, not shader writes. Include all relevant stages
            // and access masks to ensure proper synchronization for GPU readback.
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                    VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
            sourceStage           = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            destinationStage      = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            // Transition from shader read to transfer destination (for copy_from on subsequent frames)
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceStage           = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            destinationStage      = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage           = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage      = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            sourceStage           = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage      = VK_PIPELINE_STAGE_TRANSFER_BIT;
        // Phase 4: Compute shader layout transitions
        } else if (newLayout == VK_IMAGE_LAYOUT_GENERAL) {
            barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            sourceStage           = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            destinationStage      = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage           = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            destinationStage      = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        // Phase 10: GENERAL → TRANSFER_SRC for GPU readback (copy_to)
        } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            // Wait for compute shader writes to complete before transfer read
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            sourceStage           = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            destinationStage      = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else {
            // General fallback
            barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            sourceStage           = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            destinationStage      = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }

        vkCmdPipelineBarrier(cmdBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        current_layout_ = newLayout;
    }

    void copy_from(buffer& src)
    {
        auto cmdBuffer = begin_single_time_commands();

        // Transition to transfer destination
        transition_image_layout(cmdBuffer, current_layout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        // Copy buffer to image
        VkBufferImageCopy region{};
        region.bufferOffset                    = 0;
        region.bufferRowLength                 = 0;
        region.bufferImageHeight               = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset                     = {0, 0, 0};
        region.imageExtent                     = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};

        vkCmdCopyBufferToImage(
            cmdBuffer, static_cast<VkBuffer>(src.handle()), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Transition to shader read
        transition_image_layout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        end_single_time_commands(cmdBuffer);
    }

    void copy_to(buffer& dst)
    {
        auto cmdBuffer = begin_single_time_commands();

        // Transition to transfer source
        transition_image_layout(cmdBuffer, current_layout_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        // Copy image to buffer
        VkBufferImageCopy region{};
        region.bufferOffset                    = 0;
        region.bufferRowLength                 = 0;
        region.bufferImageHeight               = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset                     = {0, 0, 0};
        region.imageExtent                     = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};

        vkCmdCopyImageToBuffer(
            cmdBuffer, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBuffer>(dst.handle()), 1, &region);

        // Add buffer memory barrier to ensure GPU writes are visible to host
        // This is critical for correct GPU->CPU readback
        VkBufferMemoryBarrier bufBarrier{};
        bufBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bufBarrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bufBarrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
        bufBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufBarrier.buffer              = static_cast<VkBuffer>(dst.handle());
        bufBarrier.offset              = 0;
        bufBarrier.size                = VK_WHOLE_SIZE;

        vkCmdPipelineBarrier(cmdBuffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT,
                             0,
                             0,
                             nullptr,
                             1,
                             &bufBarrier,
                             0,
                             nullptr);

        // Transition back to shader read
        transition_image_layout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        end_single_time_commands(cmdBuffer);

        // Swizzle RGBA to BGRA for consumer compatibility
        // Internal textures use RGBA format (Vulkan convention), but CasparCG consumers expect BGRA
        // Optimized: Use parallel processing for large frames and process 4 pixels at once
        if (stride_ == 4) {
            auto* data = static_cast<uint8_t*>(dst.data());
            const int pixel_count = width_ * height_;
            const int row_count = height_;

            // Process rows in parallel - each row is independent
            tbb::parallel_for(0, row_count, [&](int row) {
                auto* row_data = data + row * width_ * 4;
                // Process pixels in chunks of 4 for better cache efficiency
                int pixels_in_row = width_;
                for (int i = 0; i < pixels_in_row; ++i) {
                    auto* pixel = row_data + i * 4;
                    std::swap(pixel[0], pixel[2]);  // Swap R and B
                }
            });
        }
    }

    void clear()
    {
        auto cmdBuffer = begin_single_time_commands();

        // Transition to transfer destination
        transition_image_layout(cmdBuffer, current_layout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        // Clear the image
        VkClearColorValue clearColor = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkImageSubresourceRange range{};
        range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel   = 0;
        range.levelCount     = 1;
        range.baseArrayLayer = 0;
        range.layerCount     = 1;

        vkCmdClearColorImage(cmdBuffer, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

        // Transition to shader read
        transition_image_layout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        end_single_time_commands(cmdBuffer);
    }

    // Phase 4: Transition to GENERAL layout for compute shader access
    void transition_to_general()
    {
        if (current_layout_ == VK_IMAGE_LAYOUT_GENERAL)
            return;

        auto cmdBuffer = begin_single_time_commands();
        transition_image_layout(cmdBuffer, current_layout_, VK_IMAGE_LAYOUT_GENERAL);
        end_single_time_commands(cmdBuffer);
    }

    // Phase 4: Transition back to shader read optimal
    void transition_to_shader_read()
    {
        if (current_layout_ == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            return;

        auto cmdBuffer = begin_single_time_commands();
        transition_image_layout(cmdBuffer, current_layout_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        end_single_time_commands(cmdBuffer);
    }

    VkFormat get_format() const { return format_; }
};

texture::texture(void*             device,
                 void*             physical_device,
                 void*             command_pool,
                 void*             queue,
                 int               width,
                 int               height,
                 int               stride,
                 common::bit_depth depth)
    : impl_(new impl(device, physical_device, command_pool, queue, width, height, stride, depth))
{
}

texture::texture(texture&& other)
    : impl_(std::move(other.impl_))
{
}

texture::~texture() {}

texture& texture::operator=(texture&& other)
{
    impl_ = std::move(other.impl_);
    return *this;
}

void texture::copy_from(buffer& source) { impl_->copy_from(source); }
void texture::copy_to(buffer& dest) { impl_->copy_to(dest); }
void texture::clear() { impl_->clear(); }

void*             texture::image() const { return impl_->image_; }
void*             texture::image_view() const { return impl_->image_view_; }
int               texture::width() const { return impl_->width_; }
int               texture::height() const { return impl_->height_; }
int               texture::stride() const { return impl_->stride_; }
common::bit_depth texture::depth() const { return impl_->depth_; }
int               texture::size() const { return impl_->size_; }
int               texture::format() const { return static_cast<int>(impl_->get_format()); }
void              texture::transition_to_general() { impl_->transition_to_general(); }
void              texture::transition_to_shader_read() { impl_->transition_to_shader_read(); }
void              texture::set_layout(int layout) { impl_->current_layout_ = static_cast<VkImageLayout>(layout); }

}}} // namespace caspar::accelerator::vk
