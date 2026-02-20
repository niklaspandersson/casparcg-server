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

#include "buffer.h"
#include "vk_check.h"

#include <boost/property_tree/ptree.hpp>

#include <vulkan/vulkan.h>

#include <atomic>

namespace caspar { namespace accelerator { namespace vk {

static std::atomic<int>         g_w_total_count{0};
static std::atomic<std::size_t> g_w_total_size{0};
static std::atomic<int>         g_r_total_count{0};
static std::atomic<std::size_t> g_r_total_size{0};

struct buffer::impl
{
    VkDevice       device_ = VK_NULL_HANDLE;
    VkBuffer       buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize   size_   = 0;
    void*          data_   = nullptr;
    bool           write_  = false;

    impl(const impl&)            = delete;
    impl& operator=(const impl&) = delete;

  public:
    impl(void* device, void* physical_device, int size, bool write)
        : device_(static_cast<VkDevice>(device))
        , size_(size)
        , write_(write)
    {
        auto vk_physical_device = static_cast<VkPhysicalDevice>(physical_device);

        // Create buffer
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size  = size_;
        bufferInfo.usage = write_ ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VK(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer_));

        // Get memory requirements
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device_, buffer_, &memRequirements);

        // Find suitable memory type (host visible and coherent)
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &memProperties);

        VkMemoryPropertyFlags requiredFlags =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        uint32_t memoryTypeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            if ((memRequirements.memoryTypeBits & (1 << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & requiredFlags) == requiredFlags) {
                memoryTypeIndex = i;
                break;
            }
        }

        if (memoryTypeIndex == UINT32_MAX) {
            vkDestroyBuffer(device_, buffer_, nullptr);
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception()
                                   << msg_info("Failed to find suitable memory type for buffer."));
        }

        // Allocate memory
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memRequirements.size;
        allocInfo.memoryTypeIndex = memoryTypeIndex;

        VK(vkAllocateMemory(device_, &allocInfo, nullptr, &memory_));

        // Bind memory to buffer
        VK(vkBindBufferMemory(device_, buffer_, memory_, 0));

        // Persistent map
        VK(vkMapMemory(device_, memory_, 0, size_, 0, &data_));

        // Update statistics
        (write_ ? g_w_total_count : g_r_total_count)++;
        (write_ ? g_w_total_size : g_r_total_size) += size_;
    }

    ~impl()
    {
        if (device_ != VK_NULL_HANDLE) {
            if (data_ != nullptr) {
                vkUnmapMemory(device_, memory_);
            }
            if (memory_ != VK_NULL_HANDLE) {
                vkFreeMemory(device_, memory_, nullptr);
            }
            if (buffer_ != VK_NULL_HANDLE) {
                vkDestroyBuffer(device_, buffer_, nullptr);
            }

            (write_ ? g_w_total_size : g_r_total_size) -= size_;
            (write_ ? g_w_total_count : g_r_total_count)--;
        }
    }

    impl(impl&& other)
        : device_(other.device_)
        , buffer_(other.buffer_)
        , memory_(other.memory_)
        , size_(other.size_)
        , data_(other.data_)
        , write_(other.write_)
    {
        other.device_ = VK_NULL_HANDLE;
        other.buffer_ = VK_NULL_HANDLE;
        other.memory_ = VK_NULL_HANDLE;
        other.data_   = nullptr;
    }

    impl& operator=(impl&& other)
    {
        if (this != &other) {
            // Clean up existing resources
            if (device_ != VK_NULL_HANDLE) {
                if (data_ != nullptr) {
                    vkUnmapMemory(device_, memory_);
                }
                if (memory_ != VK_NULL_HANDLE) {
                    vkFreeMemory(device_, memory_, nullptr);
                }
                if (buffer_ != VK_NULL_HANDLE) {
                    vkDestroyBuffer(device_, buffer_, nullptr);
                }
            }

            // Move resources
            device_ = other.device_;
            buffer_ = other.buffer_;
            memory_ = other.memory_;
            size_   = other.size_;
            data_   = other.data_;
            write_  = other.write_;

            other.device_ = VK_NULL_HANDLE;
            other.buffer_ = VK_NULL_HANDLE;
            other.memory_ = VK_NULL_HANDLE;
            other.data_   = nullptr;
        }
        return *this;
    }
};

buffer::buffer(void* device, void* physical_device, int size, bool write)
    : impl_(new impl(device, physical_device, size, write))
{
}

buffer::buffer(buffer&& other)
    : impl_(std::move(other.impl_))
{
}

buffer::~buffer() {}

buffer& buffer::operator=(buffer&& other)
{
    impl_ = std::move(other.impl_);
    return *this;
}

void* buffer::handle() const { return impl_->buffer_; }
void* buffer::data() { return impl_->data_; }
int   buffer::size() const { return static_cast<int>(impl_->size_); }
bool  buffer::write() const { return impl_->write_; }

boost::property_tree::wptree buffer::info()
{
    boost::property_tree::wptree info;

    info.add(L"total_read_count", g_r_total_count.load());
    info.add(L"total_write_count", g_w_total_count.load());
    info.add(L"total_read_size", g_r_total_size.load());
    info.add(L"total_write_size", g_w_total_size.load());

    return info;
}

}}} // namespace caspar::accelerator::vk
