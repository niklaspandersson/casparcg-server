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

#pragma once

#include <common/array.h>
#include <common/bit_depth.h>

#include <boost/property_tree/ptree_fwd.hpp>

#include <functional>
#include <future>

namespace caspar { namespace accelerator { namespace vk {

class buffer;
class texture;

/**
 * Vulkan device for screen consumer presentation.
 *
 * This is a self-contained Vulkan device used by the screen consumer
 * for window presentation. It is independent of the main accelerator
 * device used for image mixing.
 *
 * Provides:
 * - Vulkan instance creation with MoltenVK on macOS
 * - Physical device selection and logical device creation
 * - Command pool and command buffer management
 * - Async dispatch queue using Boost.ASIO
 * - Resource pooling for buffers and textures
 * - Validation layers for debug builds
 */
class device final
    : public std::enable_shared_from_this<device>
{
  public:
    device();
    ~device();

    device(const device&) = delete;
    device& operator=(const device&) = delete;

    // Texture creation
    std::shared_ptr<texture> create_texture(int width, int height, int stride, common::bit_depth depth);

    // Buffer/array creation
    array<uint8_t> create_array(int size);

    // Copy operations
    std::future<std::shared_ptr<texture>>
    copy_async(const array<const uint8_t>& source, int width, int height, int stride, common::bit_depth depth);
    std::future<array<const uint8_t>> copy_async(const std::shared_ptr<texture>& source);

    // Async dispatch - executes work on the dedicated Vulkan thread
    template <typename Func>
    auto dispatch_async(Func&& func) -> std::future<decltype(func())>;

    template <typename Func>
    auto dispatch_sync(Func&& func) -> decltype(func());

    // Version information
    std::wstring version() const;

    boost::property_tree::wptree info() const;
    std::future<void>            gc();

    // Resource usage logging (for diagnostics)
    void log_resource_usage(const std::wstring& context) const;

    // Device lost state management
    bool is_device_lost() const;
    void mark_device_lost();

    // Attempt to recover from device lost state
    // Returns true if recovery was successful
    bool attempt_recovery();

    // Vulkan handle accessors (for internal use by buffer/texture)
    struct vulkan_handles
    {
        void* instance;        // VkInstance
        void* physical_device; // VkPhysicalDevice
        void* device;          // VkDevice
        void* queue;           // VkQueue
        void* command_pool;    // VkCommandPool
        uint32_t queue_family_index;
    };
    vulkan_handles get_handles() const;

  private:
    void dispatch(std::function<void()> func);

    struct impl;
    std::shared_ptr<impl> impl_;
};

// Template implementations
template <typename Func>
auto device::dispatch_async(Func&& func) -> std::future<decltype(func())>
{
    using result_type = decltype(func());
    using task_type   = std::packaged_task<result_type()>;

    auto task   = std::make_shared<task_type>(std::forward<Func>(func));
    auto future = task->get_future();
    dispatch([=] { (*task)(); });
    return future;
}

template <typename Func>
auto device::dispatch_sync(Func&& func) -> decltype(func())
{
    return dispatch_async(std::forward<Func>(func)).get();
}

}}} // namespace caspar::accelerator::vk
