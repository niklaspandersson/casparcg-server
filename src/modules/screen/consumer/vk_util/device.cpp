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

#include "device.h"
#include "buffer.h"
#include "texture.h"
#include "vk_check.h"

#include <common/array.h>
#include <common/assert.h>
#include <common/env.h>
#include <common/except.h>
#include <common/log.h>
#include <common/os/thread.h>

#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>

#include <vulkan/vulkan.h>

#include <array>
#include <future>
#include <thread>
#include <vector>

namespace caspar { namespace accelerator { namespace vk {

using namespace boost::asio;

// Validation layer callback for debug builds
#ifndef NDEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
                                                     VkDebugUtilsMessageTypeFlagsEXT             messageType,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                     void*                                       pUserData)
{
    std::wstring severity;
    switch (messageSeverity) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
            severity = L"VERBOSE";
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            severity = L"INFO";
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            severity = L"WARNING";
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            severity = L"ERROR";
            break;
        default:
            severity = L"UNKNOWN";
    }

    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        CASPAR_LOG(warning) << L"[Vulkan " << severity << L"] " << pCallbackData->pMessage;
    } else {
        CASPAR_LOG(debug) << L"[Vulkan " << severity << L"] " << pCallbackData->pMessage;
    }

    return VK_FALSE;
}
#endif

struct device::impl : public std::enable_shared_from_this<impl>
{
    using texture_queue_t = tbb::concurrent_bounded_queue<std::shared_ptr<texture>>;
    using buffer_queue_t  = tbb::concurrent_bounded_queue<std::shared_ptr<buffer>>;

    // Vulkan handles
    VkInstance       instance_        = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice         device_          = VK_NULL_HANDLE;
    VkQueue          queue_           = VK_NULL_HANDLE;
    VkCommandPool    command_pool_    = VK_NULL_HANDLE;
    uint32_t         queue_family_index_ = 0;

    // Device lost state tracking
    std::atomic<bool> device_lost_{false};
    std::atomic<int>  recovery_attempts_{0};
    static constexpr int max_recovery_attempts_ = 5;

#ifndef NDEBUG
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
#endif

    // Resource pools (matching OGL pattern)
    // device_pools_[depth_index][stride-1][size_key] -> queue of textures
    std::array<std::array<tbb::concurrent_unordered_map<size_t, texture_queue_t>, 4>, 2> device_pools_;
    // host_pools_[write?1:0][size] -> queue of buffers
    std::array<tbb::concurrent_unordered_map<size_t, buffer_queue_t>, 2> host_pools_;

    // Version string
    std::wstring version_;

    // Async dispatch via Boost.ASIO
    io_context                             io_context_;
    decltype(make_work_guard(io_context_)) work_;
    std::thread                            thread_;

    impl()
        : work_(make_work_guard(io_context_))
    {
        CASPAR_LOG(info) << L"Initializing Vulkan Device.";

        create_instance();
        select_physical_device();
        create_logical_device();
        create_command_pool();

        build_version_string();

        CASPAR_LOG(info) << L"Initialized Vulkan " << version();

        // Start the dedicated Vulkan thread
        thread_ = std::thread([this] {
            set_thread_name(L"Vulkan Device");
            io_context_.run();
        });
    }

    ~impl()
    {
        // Stop the async thread
        work_.reset();
        thread_.join();

        // Clean up resource pools
        for (auto& pool : host_pools_)
            pool.clear();

        for (auto& pools : device_pools_)
            for (auto& pool : pools)
                pool.clear();

        // Destroy Vulkan resources
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
        }

#ifndef NDEBUG
        if (debug_messenger_ != VK_NULL_HANDLE) {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_,
                                                                                   "vkDestroyDebugUtilsMessengerEXT");
            if (func != nullptr) {
                func(instance_, debug_messenger_, nullptr);
            }
        }
#endif

        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
        }

        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }

        CASPAR_LOG(info) << L"Vulkan Device destroyed.";
    }

    void create_instance()
    {
        // Application info
        VkApplicationInfo appInfo{};
        appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName   = "CasparCG";
        appInfo.applicationVersion = VK_MAKE_VERSION(2, 5, 0);
        appInfo.pEngineName        = "CasparCG";
        appInfo.engineVersion      = VK_MAKE_VERSION(2, 5, 0);
        appInfo.apiVersion         = VK_API_VERSION_1_2;

        // Extensions
        std::vector<const char*> extensions;
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef __APPLE__
        // MoltenVK portability extension required on macOS
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        extensions.push_back("VK_EXT_metal_surface");
#endif
#ifndef NDEBUG
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

        // Validation layers for debug builds
        std::vector<const char*> layers;
#ifndef NDEBUG
        // Check if validation layers are available
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
        bool        validationAvailable = false;
        for (const auto& layer : availableLayers) {
            if (strcmp(layer.layerName, validationLayerName) == 0) {
                validationAvailable = true;
                break;
            }
        }

        if (validationAvailable) {
            layers.push_back(validationLayerName);
            CASPAR_LOG(info) << L"Vulkan validation layers enabled.";
        } else {
            CASPAR_LOG(warning) << L"Vulkan validation layers requested but not available.";
        }
#endif

        // Create instance
        VkInstanceCreateInfo createInfo{};
        createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo        = &appInfo;
        createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount       = static_cast<uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames     = layers.data();
#ifdef __APPLE__
        createInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

        VK(vkCreateInstance(&createInfo, nullptr, &instance_));

#ifndef NDEBUG
        // Setup debug messenger
        if (validationAvailable) {
            VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
            debugCreateInfo.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugCreateInfo.pfnUserCallback = debug_callback;

            auto func =
                (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
            if (func != nullptr) {
                func(instance_, &debugCreateInfo, nullptr, &debug_messenger_);
            }
        }
#endif
    }

    void select_physical_device()
    {
        uint32_t deviceCount = 0;
        VK(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr));

        if (deviceCount == 0) {
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception() << msg_info("No Vulkan-capable GPU found."));
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        VK(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()));

        // Log available devices and select the best one
        CASPAR_LOG(info) << L"Found " << deviceCount << L" Vulkan device(s):";

        int  bestScore = -1;
        int  bestIndex = -1;

        for (size_t i = 0; i < devices.size(); ++i) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devices[i], &props);

            int score = 0;

            // Prefer discrete GPUs
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                score += 1000;
            } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                score += 100;
            }

            // Check for required queue family
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queueFamilyCount, queueFamilies.data());

            bool hasGraphicsQueue = false;
            for (const auto& qf : queueFamilies) {
                if (qf.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    hasGraphicsQueue = true;
                    break;
                }
            }

            if (!hasGraphicsQueue) {
                score = -1; // Unusable
            }

            std::wstring deviceType;
            switch (props.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    deviceType = L"Discrete";
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    deviceType = L"Integrated";
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                    deviceType = L"Virtual";
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    deviceType = L"CPU";
                    break;
                default:
                    deviceType = L"Unknown";
            }

            CASPAR_LOG(info) << L"  [" << i << L"] " << props.deviceName << L" (" << deviceType << L", API "
                             << VK_VERSION_MAJOR(props.apiVersion) << L"." << VK_VERSION_MINOR(props.apiVersion) << L"."
                             << VK_VERSION_PATCH(props.apiVersion) << L")";

            if (score > bestScore) {
                bestScore = score;
                bestIndex = static_cast<int>(i);
            }
        }

        if (bestIndex < 0) {
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception()
                                   << msg_info("No suitable Vulkan GPU found (need graphics queue)."));
        }

        physical_device_ = devices[bestIndex];

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        CASPAR_LOG(info) << L"Selected Vulkan device: " << props.deviceName;
    }

    void create_logical_device()
    {
        // Find graphics queue family
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queueFamilyCount, queueFamilies.data());

        queue_family_index_ = UINT32_MAX;
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queue_family_index_ = i;
                break;
            }
        }

        if (queue_family_index_ == UINT32_MAX) {
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception() << msg_info("No graphics queue family found."));
        }

        // Queue create info
        float                   queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queue_family_index_;
        queueCreateInfo.queueCount       = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        // Device features
        VkPhysicalDeviceFeatures deviceFeatures{};
        // Enable features as needed for rendering

        // Device extensions
        std::vector<const char*> deviceExtensions;
        deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef __APPLE__
        deviceExtensions.push_back("VK_KHR_portability_subset");
#endif

        // Create logical device
        VkDeviceCreateInfo createInfo{};
        createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount    = 1;
        createInfo.pQueueCreateInfos       = &queueCreateInfo;
        createInfo.pEnabledFeatures        = &deviceFeatures;
        createInfo.enabledExtensionCount   = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        VK(vkCreateDevice(physical_device_, &createInfo, nullptr, &device_));

        // Get the graphics queue
        vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);
    }

    void create_command_pool()
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queue_family_index_;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

        VK(vkCreateCommandPool(device_, &poolInfo, nullptr, &command_pool_));
    }

    void build_version_string()
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);

        std::wstringstream ss;
        ss << VK_VERSION_MAJOR(props.apiVersion) << L"." << VK_VERSION_MINOR(props.apiVersion) << L"."
           << VK_VERSION_PATCH(props.apiVersion) << L" " << props.deviceName;
#ifdef __APPLE__
        ss << L" (MoltenVK)";
#endif
        version_ = ss.str();
    }

    std::wstring version() { return version_; }

    // Template functions need to be defined before they are used
    template <typename Func>
    auto spawn_async(Func&& func)
    {
        using result_type = decltype(func(std::declval<yield_context>()));
        using task_type   = std::packaged_task<result_type(yield_context)>;

        auto task   = task_type(std::forward<Func>(func));
        auto future = task.get_future();
        boost::asio::spawn(io_context_,
                           std::move(task)
#if BOOST_VERSION >= 108000
                               ,
                           [](std::exception_ptr e) {
                               if (e)
                                   std::rethrow_exception(e);
                           }
#endif
        );
        return future;
    }

    template <typename Func>
    auto dispatch_async(Func&& func)
    {
        using result_type = decltype(func());
        using task_type   = std::packaged_task<result_type()>;

        auto task   = std::make_shared<task_type>(std::forward<Func>(func));
        auto future = task->get_future();
        boost::asio::dispatch(io_context_, [task] { (*task)(); });
        return future;
    }

    std::shared_ptr<texture> create_texture(int width, int height, int stride, common::bit_depth depth, bool clear)
    {
        CASPAR_VERIFY(stride > 0 && stride < 5);
        CASPAR_VERIFY(width > 0 && height > 0);

        auto depth_pool_index = depth == common::bit_depth::bit8 ? 0 : 1;

        auto pool = &device_pools_[depth_pool_index][stride - 1][(width << 16 & 0xFFFF0000) | (height & 0x0000FFFF)];

        std::shared_ptr<texture> tex;
        if (!pool->try_pop(tex)) {
            tex = std::make_shared<texture>(device_, physical_device_, command_pool_, queue_,
                                           width, height, stride, depth);
        }

        if (clear) {
            tex->clear();
        }

        auto ptr = tex.get();
        return std::shared_ptr<texture>(
            ptr, [tex = std::move(tex), pool, self = shared_from_this()](texture*) mutable { pool->push(tex); });
    }

    std::shared_ptr<buffer> create_buffer(int size, bool write)
    {
        CASPAR_VERIFY(size > 0);

        auto pool = &host_pools_[static_cast<int>(write ? 1 : 0)][size];

        std::shared_ptr<buffer> buf;
        if (!pool->try_pop(buf)) {
            buf = std::make_shared<buffer>(device_, physical_device_, size, write);
        }

        auto ptr = buf.get();
        return std::shared_ptr<buffer>(ptr, [buf = std::move(buf), self = shared_from_this()](buffer*) mutable {
            auto pool = &self->host_pools_[static_cast<int>(buf->write() ? 1 : 0)][buf->size()];
            pool->push(std::move(buf));
        });
    }

    array<uint8_t> create_array(int size)
    {
        auto buf = create_buffer(size, true);
        auto ptr = reinterpret_cast<uint8_t*>(buf->data());
        return array<uint8_t>(ptr, buf->size(), std::move(buf));
    }

    std::future<std::shared_ptr<texture>>
    copy_async(const array<const uint8_t>& source, int width, int height, int stride, common::bit_depth depth)
    {
        return dispatch_async([=, this, self = shared_from_this()] {
            std::shared_ptr<buffer> buf;

            auto tmp = source.storage<std::shared_ptr<buffer>>();
            if (tmp) {
                buf = *tmp;
            } else {
                buf = create_buffer(static_cast<int>(source.size()), true);
                std::memcpy(buf->data(), source.data(), source.size());
            }

            auto tex = create_texture(width, height, stride, depth, false);
            tex->copy_from(*buf);
            return tex;
        });
    }

    std::future<array<const uint8_t>> copy_async(const std::shared_ptr<texture>& source)
    {
        return spawn_async([=, this, self = shared_from_this()](yield_context /* yield */) {
            // NOTE: Removed vkDeviceWaitIdle() which was blocking ALL GPU work every frame.
            // The copy_to() function already synchronizes via vkQueueWaitIdle() after the
            // transfer command, which is sufficient for GPU->CPU readback.

            auto buf = create_buffer(source->size(), false);
            source->copy_to(*buf);

            // copy_to() already waits for completion via vkQueueWaitIdle(), so no
            // additional fence/polling is needed. The data is ready to read.

            auto ptr  = reinterpret_cast<uint8_t*>(buf->data());
            auto size = buf->size();
            return array<const uint8_t>(ptr, size, std::move(buf));
        });
    }

    boost::property_tree::wptree info() const
    {
        boost::property_tree::wptree info;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);

        info.add(L"vk.name", props.deviceName);
        info.add(L"vk.version", version_);
        info.add(L"vk.api_version",
                 std::to_wstring(VK_VERSION_MAJOR(props.apiVersion)) + L"." +
                     std::to_wstring(VK_VERSION_MINOR(props.apiVersion)) + L"." +
                     std::to_wstring(VK_VERSION_PATCH(props.apiVersion)));

        // Pool statistics
        boost::property_tree::wptree pooled_device_buffers;
        size_t                       total_pooled_device_buffer_size  = 0;
        size_t                       total_pooled_device_buffer_count = 0;

        for (size_t i = 0; i < device_pools_.size(); ++i) {
            auto& depth_pools = device_pools_.at(i);
            for (size_t j = 0; j < depth_pools.size(); ++j) {
                auto& pools  = depth_pools.at(j);
                auto  stride = j + 1;

                for (auto& pool : pools) {
                    auto width  = pool.first >> 16;
                    auto height = pool.first & 0x0000FFFF;
                    auto size   = width * height * stride;
                    auto count  = pool.second.size();

                    if (count == 0)
                        continue;

                    boost::property_tree::wptree pool_info;
                    pool_info.add(L"stride", stride);
                    pool_info.add(L"width", width);
                    pool_info.add(L"height", height);
                    pool_info.add(L"size", size);
                    pool_info.add(L"count", count);

                    total_pooled_device_buffer_size += size * count;
                    total_pooled_device_buffer_count += count;

                    pooled_device_buffers.add_child(L"device_buffer_pool", pool_info);
                }
            }
        }

        info.add_child(L"vk.details.pooled_device_buffers", pooled_device_buffers);

        boost::property_tree::wptree pooled_host_buffers;
        size_t                       total_read_size   = 0;
        size_t                       total_write_size  = 0;
        size_t                       total_read_count  = 0;
        size_t                       total_write_count = 0;

        for (size_t i = 0; i < host_pools_.size(); ++i) {
            auto& pools    = host_pools_.at(i);
            auto  is_write = i == 1;

            for (auto& pool : pools) {
                auto size  = pool.first;
                auto count = pool.second.size();

                if (count == 0)
                    continue;

                boost::property_tree::wptree pool_info;
                pool_info.add(L"usage", is_write ? L"write_only" : L"read_only");
                pool_info.add(L"size", size);
                pool_info.add(L"count", count);

                pooled_host_buffers.add_child(L"host_buffer_pool", pool_info);

                (is_write ? total_write_count : total_read_count) += count;
                (is_write ? total_write_size : total_read_size) += size * count;
            }
        }

        info.add_child(L"vk.details.pooled_host_buffers", pooled_host_buffers);
        info.add(L"vk.summary.pooled_device_buffers.total_count", total_pooled_device_buffer_count);
        info.add(L"vk.summary.pooled_device_buffers.total_size", total_pooled_device_buffer_size);
        info.add(L"vk.summary.pooled_host_buffers.total_read_count", total_read_count);
        info.add(L"vk.summary.pooled_host_buffers.total_write_count", total_write_count);
        info.add(L"vk.summary.pooled_host_buffers.total_read_size", total_read_size);
        info.add(L"vk.summary.pooled_host_buffers.total_write_size", total_write_size);
        info.add_child(L"vk.summary.all_host_buffers", buffer::info());

        return info;
    }

    std::future<void> gc()
    {
        return spawn_async([=, this, self = shared_from_this()](yield_context yield) {
            CASPAR_LOG(info) << L" vk: Running GC.";

            try {
                for (auto& depth_pools : device_pools_) {
                    for (auto& pools : depth_pools) {
                        for (auto& pool : pools)
                            pool.second.clear();
                    }
                }
                for (auto& pools : host_pools_) {
                    for (auto& pool : pools)
                        pool.second.clear();
                }
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        });
    }
};

device::device()
    : impl_(std::make_shared<impl>())
{
}

device::~device() {}

std::shared_ptr<texture> device::create_texture(int width, int height, int stride, common::bit_depth depth)
{
    return impl_->create_texture(width, height, stride, depth, true);
}

array<uint8_t> device::create_array(int size) { return impl_->create_array(size); }

std::future<std::shared_ptr<texture>>
device::copy_async(const array<const uint8_t>& source, int width, int height, int stride, common::bit_depth depth)
{
    return impl_->copy_async(source, width, height, stride, depth);
}

std::future<array<const uint8_t>> device::copy_async(const std::shared_ptr<texture>& source)
{
    return impl_->copy_async(source);
}

void device::dispatch(std::function<void()> func) { boost::asio::dispatch(impl_->io_context_, std::move(func)); }

std::wstring device::version() const { return impl_->version(); }

boost::property_tree::wptree device::info() const { return impl_->info(); }

std::future<void> device::gc() { return impl_->gc(); }

void device::log_resource_usage(const std::wstring& context) const
{
    // Count pooled textures
    size_t total_pooled_textures = 0;
    size_t total_pooled_texture_size = 0;

    for (size_t depth_idx = 0; depth_idx < impl_->device_pools_.size(); ++depth_idx) {
        auto& depth_pools = impl_->device_pools_.at(depth_idx);
        for (size_t stride_idx = 0; stride_idx < depth_pools.size(); ++stride_idx) {
            auto& pools = depth_pools.at(stride_idx);
            auto stride = stride_idx + 1;

            for (auto& pool : pools) {
                auto width = pool.first >> 16;
                auto height = pool.first & 0x0000FFFF;
                auto size = width * height * stride;
                auto count = pool.second.size();

                total_pooled_textures += count;
                total_pooled_texture_size += size * count;
            }
        }
    }

    // Count pooled buffers
    size_t total_pooled_read_buffers = 0;
    size_t total_pooled_write_buffers = 0;
    size_t total_pooled_read_size = 0;
    size_t total_pooled_write_size = 0;

    for (size_t i = 0; i < impl_->host_pools_.size(); ++i) {
        auto& pools = impl_->host_pools_.at(i);
        bool is_write = (i == 1);

        for (auto& pool : pools) {
            auto size = pool.first;
            auto count = pool.second.size();

            if (is_write) {
                total_pooled_write_buffers += count;
                total_pooled_write_size += size * count;
            } else {
                total_pooled_read_buffers += count;
                total_pooled_read_size += size * count;
            }
        }
    }

    // Get live buffer stats from buffer.cpp
    auto buffer_info = buffer::info();

    CASPAR_LOG(info) << L"[vk::device] Resource usage [" << context << L"]:"
                      << L" pooled_textures=" << total_pooled_textures
                      << L" (" << (total_pooled_texture_size / 1024 / 1024) << L"MB)"
                      << L" pooled_read_bufs=" << total_pooled_read_buffers
                      << L" (" << (total_pooled_read_size / 1024 / 1024) << L"MB)"
                      << L" pooled_write_bufs=" << total_pooled_write_buffers
                      << L" (" << (total_pooled_write_size / 1024 / 1024) << L"MB)"
                      << L" live_read_bufs=" << buffer_info.get<int>(L"total_read_count", 0)
                      << L" (" << (buffer_info.get<size_t>(L"total_read_size", 0) / 1024 / 1024) << L"MB)"
                      << L" live_write_bufs=" << buffer_info.get<int>(L"total_write_count", 0)
                      << L" (" << (buffer_info.get<size_t>(L"total_write_size", 0) / 1024 / 1024) << L"MB)";
}

device::vulkan_handles device::get_handles() const
{
    vulkan_handles handles;
    handles.instance           = impl_->instance_;
    handles.physical_device    = impl_->physical_device_;
    handles.device             = impl_->device_;
    handles.queue              = impl_->queue_;
    handles.command_pool       = impl_->command_pool_;
    handles.queue_family_index = impl_->queue_family_index_;
    return handles;
}

bool device::is_device_lost() const
{
    return impl_->device_lost_.load(std::memory_order_relaxed);
}

void device::mark_device_lost()
{
    bool expected = false;
    if (impl_->device_lost_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        CASPAR_LOG(error) << L"[vk::device] Vulkan device lost - GPU may be overloaded or unavailable. "
                          << L"Outputting black frames while attempting recovery...";
    }
}

bool device::attempt_recovery()
{
    if (!impl_->device_lost_.load(std::memory_order_relaxed)) {
        return true; // Not in device lost state
    }

    int attempts = impl_->recovery_attempts_.fetch_add(1, std::memory_order_relaxed);
    if (attempts >= impl::max_recovery_attempts_) {
        if (attempts == impl::max_recovery_attempts_) {
            CASPAR_LOG(error) << L"[vk::device] Recovery failed after " << impl::max_recovery_attempts_
                              << L" attempts. Manual restart may be required.";
        }
        return false;
    }

    CASPAR_LOG(info) << L"[vk::device] Attempting device recovery (attempt " << (attempts + 1)
                     << L"/" << impl::max_recovery_attempts_ << L")...";

    try {
        // Wait for any pending operations to complete
        if (impl_->device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(impl_->device_);
        }

        // Clear all resource pools (they hold references to invalid resources)
        for (auto& depth_pools : impl_->device_pools_) {
            for (auto& pools : depth_pools) {
                for (auto& pool : pools)
                    pool.second.clear();
            }
        }
        for (auto& pools : impl_->host_pools_) {
            for (auto& pool : pools)
                pool.second.clear();
        }

        // Destroy old command pool
        if (impl_->command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(impl_->device_, impl_->command_pool_, nullptr);
            impl_->command_pool_ = VK_NULL_HANDLE;
        }

        // Destroy old device
        if (impl_->device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(impl_->device_, nullptr);
            impl_->device_ = VK_NULL_HANDLE;
        }

        // Recreate logical device and command pool
        impl_->create_logical_device();
        impl_->create_command_pool();

        // Recovery successful
        impl_->device_lost_.store(false, std::memory_order_relaxed);
        impl_->recovery_attempts_.store(0, std::memory_order_relaxed);

        CASPAR_LOG(info) << L"[vk::device] Device recovery successful. Resuming normal operation.";
        return true;

    } catch (const std::exception& e) {
        CASPAR_LOG(warning) << L"[vk::device] Recovery attempt " << (attempts + 1) << L" failed: " << e.what();
        return false;
    } catch (...) {
        CASPAR_LOG(warning) << L"[vk::device] Recovery attempt " << (attempts + 1) << L" failed with unknown error.";
        return false;
    }
}

}}} // namespace caspar::accelerator::vk
