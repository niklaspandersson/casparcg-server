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
 * Author: Robert Nagy, ronag89@gmail.com
 */
#include "device.h"

#include "buffer.h"
#include "pipeline.h"
#include "texture.h"

#include <common/array.h>
#include <common/assert.h>
#include <common/env.h>
#include <common/except.h>
#include <common/os/thread.h>

#include <VkBootstrap.h>
#include <vulkan/vulkan.hpp>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable : 4189)
#include <vk_mem_alloc.h>
#pragma warning(pop)

#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>

#include <array>
#include <future>
#include <thread>

namespace caspar { namespace accelerator { namespace vulkan {

using namespace boost::asio;

inline VKAPI_ATTR VkBool32 VKAPI_CALL default_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                             VkDebugUtilsMessageTypeFlagsEXT        messageType,
                                                             const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                             void*)
{
    auto ms = vkb::to_string_message_severity(messageSeverity);
    auto mt = vkb::to_string_message_type(messageType);
    if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
        CASPAR_LOG(info) << "[" << ms << ": " << mt << "] - " << pCallbackData->pMessageIdName << ", "
                         << pCallbackData->pMessage;
        // printf("[%s: %s] - %s\n%s\n", ms, mt, pCallbackData->pMessageIdName, pCallbackData->pMessage);
    } else {
        if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
            CASPAR_LOG(info) << "[" << ms << ": " << mt << "] " << pCallbackData->pMessage;
            // printf("[%s: %s]\n%s\n", ms, mt, pCallbackData->pMessage);
        }
    }

    return VK_FALSE; // Applications must return false here (Except Validation, if return true, will skip calling to
                     // driver)
}

void transitionImageLayout(const vk::Image&        image,
                           vk::Format              format,
                           vk::ImageLayout         oldLayout,
                           vk::AccessFlags2        srcAccessMask,
                           vk::PipelineStageFlags2 srcStage,
                           vk::ImageLayout         newLayout,
                           vk::AccessFlags2        dstAccessMask,
                           vk::PipelineStageFlags2 dstStage,
                           vk::CommandBuffer       cmdBuffer)
{
    vk::PipelineStageFlags2 sourceStage;
    vk::PipelineStageFlags2 destinationStage;

    auto range = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

    vk::ImageMemoryBarrier2 barrier{};
    barrier.oldLayout = oldLayout, barrier.newLayout = newLayout, barrier.srcQueueFamilyIndex = vk::QueueFamilyIgnored,
    barrier.dstQueueFamilyIndex = vk::QueueFamilyIgnored, barrier.image = image, barrier.subresourceRange = range;

    barrier.srcAccessMask = srcAccessMask;
    barrier.srcStageMask  = srcStage;

    barrier.dstAccessMask = dstAccessMask;
    barrier.dstStageMask  = dstStage;

    vk::DependencyInfo dep_info;
    dep_info.setImageMemoryBarriers(barrier);

    cmdBuffer.pipelineBarrier2(dep_info);
}

void submitSingleTimeCommands(vk::Device                                    device,
                              vk::CommandPool                               commandPool,
                              vk::Queue                                     queue,
                              std::function<void(const vk::CommandBuffer&)> func,
                              vk::Fence*                                    pFence = nullptr)
{
    vk::CommandBufferAllocateInfo allocInfo = {};
    allocInfo.commandPool = commandPool, allocInfo.level = vk::CommandBufferLevel::ePrimary,
    allocInfo.commandBufferCount = 1;
    auto commandBuffer           = device.allocateCommandBuffers(allocInfo)[0];

    vk::CommandBufferBeginInfo beginInfo = {};
    beginInfo.flags                      = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

    commandBuffer.begin(beginInfo);

    func(commandBuffer);

    commandBuffer.end();

    vk::SubmitInfo submitInfo = {};

    submitInfo.setCommandBuffers(commandBuffer);

    if (pFence)
        queue.submit(submitInfo, *pFence);
    else
        queue.submit(submitInfo);

    queue.waitIdle();

    device.freeCommandBuffers(commandPool, commandBuffer);
}

struct device::impl : public std::enable_shared_from_this<impl>
{
    using texture_queue_t = tbb::concurrent_bounded_queue<std::shared_ptr<texture>>;
    using buffer_queue_t  = tbb::concurrent_bounded_queue<std::shared_ptr<buffer>>;

    std::array<tbb::concurrent_unordered_map<size_t, texture_queue_t>, 2>                attachment_pools_;
    std::array<std::array<tbb::concurrent_unordered_map<size_t, texture_queue_t>, 4>, 2> device_pools_;
    std::array<tbb::concurrent_unordered_map<size_t, buffer_queue_t>, 2>                 host_pools_;

    std::wstring version_;

    vkb::Instance       _vkb_instance;
    vkb::PhysicalDevice _vkb_physical_device;
    vk::PhysicalDevice  _physical_device;
    vk::Device          _device;
    vk::Queue           _queue;
    vk::CommandPool     _command_pool;
    VmaAllocator        _allocator;

    io_context                             io_context_;
    decltype(make_work_guard(io_context_)) work_;
    std::thread                            thread_;

    impl()
        : work_(make_work_guard(io_context_))
    {
        CASPAR_LOG(info) << L"Initializing Vulkan Device.";

        // auto pDebugFn = debug_callback;

        auto instance_builder = vkb::InstanceBuilder()
                                    .enable_validation_layers(true)
                                    .set_app_name("CasparCG")
                                    .set_headless(true)
                                    .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                                    .set_debug_messenger_type(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
                                    .set_debug_callback(default_debug_callback)
                                    .set_engine_name("CasparCG")
                                    .require_api_version(VK_API_VERSION_1_3);
        auto instance_ret = instance_builder.build();
        if (!instance_ret) {
            CASPAR_THROW_EXCEPTION(caspar_exception()
                                   << msg_info("Failed to create Vulkan instance: " + instance_ret.error().message()));
        }
        _vkb_instance = instance_ret.value();

        // Find suitable physical device
        auto gpu_selector = vkb::PhysicalDeviceSelector(_vkb_instance);

        vk::PhysicalDeviceVulkan13Features features;
        features.dynamicRendering = true;
        features.synchronization2 = true;

        vk::PhysicalDeviceRobustness2FeaturesEXT robustness2_features;
        robustness2_features.nullDescriptor = true;

        auto gpu_res = gpu_selector
                           .set_minimum_version(1, 3)
                           //.add_required_extensions({"VK_EXT_robustness2"})
                           //.add_required_extension_features(robustness2_features)
                           .set_required_features_13(features)
                           .select();
        if (!gpu_res) {
            CASPAR_THROW_EXCEPTION(caspar_exception()
                                   << msg_info("Failed to select physical device: " + gpu_res.error().message()));
        }
        _vkb_physical_device = gpu_res.value();

        // Create the logical device
        auto device_builder = vkb::DeviceBuilder(_vkb_physical_device);
        _physical_device    = vk::PhysicalDevice(_vkb_physical_device.physical_device);

        auto device_res = device_builder.build();
        if (!device_res) {
            CASPAR_THROW_EXCEPTION(caspar_exception()
                                   << msg_info("Failed to create device: " + device_res.error().message()));
        }
        auto vkb_device   = device_res.value();
        _device           = vk::Device(vkb_device.device);
        _queue            = vk::Queue(vkb_device.get_queue(vkb::QueueType::graphics).value());
        auto queue_family = vkb_device.get_queue_index(vkb::QueueType::graphics).value();

        vk::CommandPoolCreateInfo pool_info;
        pool_info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        pool_info.queueFamilyIndex = queue_family;

        _command_pool = _device.createCommandPool(pool_info);

        VmaVulkanFunctions vulkanFunctions    = {};
        vulkanFunctions.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
        vulkanFunctions.vkGetDeviceProcAddr   = &vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo allocatorCreateInfo = {};
        allocatorCreateInfo.flags                  = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        allocatorCreateInfo.vulkanApiVersion       = VK_API_VERSION_1_3;
        allocatorCreateInfo.physicalDevice         = _physical_device;
        allocatorCreateInfo.device                 = _device;
        allocatorCreateInfo.instance               = _vkb_instance.instance;
        allocatorCreateInfo.pVulkanFunctions       = &vulkanFunctions;

        vmaCreateAllocator(&allocatorCreateInfo, &_allocator);

        thread_ = std::thread([&] {
            set_thread_name(L"Vulkan Device");
            io_context_.run();
        });
    }

    ~impl()
    {
        work_.reset();
        thread_.join();

        for (auto& pool : host_pools_)
            pool.clear();

        for (auto& pools : device_pools_)
            for (auto& pool : pools)
                pool.clear();
    }

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

        auto task   = task_type(std::forward<Func>(func));
        auto future = task.get_future();
        boost::asio::dispatch(io_context_, std::move(task));
        return future;
    }

    template <typename Func>
    auto dispatch_sync(Func&& func) -> decltype(func())
    {
        return dispatch_async(std::forward<Func>(func)).get();
    }

    void submit_render_pass(std::shared_ptr<texture>                            attachment,
                            std::function<void(vk::CommandBuffer, vk::Device)>& func)
    {
        dispatch_async([=] {
            auto cmd_buffer = _device.allocateCommandBuffers(
                vk::CommandBufferAllocateInfo(_command_pool, vk::CommandBufferLevel::ePrimary, 1))[0];
            cmd_buffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

            vk::ClearValue clearColor{vk::ClearColorValue(std::array<float, 4>{0.0f, 1.0f, 0.0f, 1.0f})};

            // Viewport and scissor
            vk::Viewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(attachment->width()),
                                  static_cast<float>(attachment->height()),
                                  0.0f,
                                  1.0f};

            vk::Extent2D extent = {static_cast<uint32_t>(attachment->width()),
                                   static_cast<uint32_t>(attachment->height())};
            vk::Rect2D   scissor{{0, 0}, extent};

            // setup "renderpass" dynamically
            vk::RenderingAttachmentInfo attachment_info{};
            attachment_info.imageView   = attachment->view();
            attachment_info.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
            attachment_info.loadOp      = vk::AttachmentLoadOp::eClear;
            attachment_info.storeOp     = vk::AttachmentStoreOp::eStore;
            attachment_info.clearValue  = clearColor;

            vk::RenderingInfo rendering_info{};
            rendering_info.renderArea = scissor;
            rendering_info.layerCount = 1;
            rendering_info.setColorAttachments(attachment_info);

            cmd_buffer.beginRendering(rendering_info);
            cmd_buffer.setViewport(0, viewport);
            cmd_buffer.setScissor(0, scissor);
            func(cmd_buffer, _device);
            cmd_buffer.endRendering();
            cmd_buffer.end();

            vk::SubmitInfo2 submit_info;
            submit_info.setCommandBufferInfos(vk::CommandBufferSubmitInfo().setCommandBuffer(cmd_buffer));
            _queue.submit2(submit_info);
        });
    }

    std::shared_ptr<pipeline> create_pipeline()
    {
        return dispatch_sync([&]() { return std::make_shared<pipeline>(_device); });
    }

    std::wstring version() { return version_; }

    uint32_t findDedicatedMemoryType(uint32_t typeMask, vk::MemoryPropertyFlags properties)
    {
        auto memProperties = _physical_device.getMemoryProperties();
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            if ((typeMask & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags == properties)) {
                return i;
            }
        }
        throw std::runtime_error("Failed to find suitable memory type");
    }

    template <typename T>
    using Res = std::pair<T, vk::DeviceMemory>;

    Res<vk::Buffer> upload_vertex_buffer()
    {
        return dispatch_sync([&]() -> Res<vk::Buffer> {
            auto data = reinterpret_cast<const double*>(core::frame_geometry::get_default().data().data());

            std::vector<float> fl;
            std::transform(
                data, data + 6 * 4, std::back_inserter(fl), [](double val) { return static_cast<float>(val); });

            size_t size = fl.size() * sizeof(float);

            // staging buffer
            vk::BufferCreateInfo stagingInfo{};
            stagingInfo.size        = size;
            stagingInfo.usage       = vk::BufferUsageFlagBits::eTransferSrc;
            stagingInfo.sharingMode = vk::SharingMode::eExclusive;

            auto stagingBuffer = _device.createBuffer(stagingInfo);

            auto stagingMemReq = _device.getBufferMemoryRequirements(stagingBuffer);

            vk::MemoryAllocateInfo stagingAlloc{};
            stagingAlloc.allocationSize  = stagingMemReq.size;
            stagingAlloc.memoryTypeIndex = findDedicatedMemoryType(stagingMemReq.memoryTypeBits,
                                                                   vk::MemoryPropertyFlagBits::eHostVisible |
                                                                       vk::MemoryPropertyFlagBits::eHostCoherent);

            auto stagingBufferMemory = _device.allocateMemory(stagingAlloc);
            _device.bindBufferMemory(stagingBuffer, stagingBufferMemory, 0);

            void* dest = _device.mapMemory(stagingBufferMemory, 0, size);
            memcpy(dest, fl.data(), size);
            _device.unmapMemory(stagingBufferMemory);

            // vertex buffer
            vk::BufferCreateInfo bufferInfo{};
            bufferInfo.size        = size;
            bufferInfo.usage       = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer;
            bufferInfo.sharingMode = vk::SharingMode::eExclusive;

            auto vertexBuffer = _device.createBuffer(bufferInfo);

            auto memReq = _device.getBufferMemoryRequirements(vertexBuffer);

            vk::MemoryAllocateInfo allocInfo{};
            allocInfo.allocationSize = memReq.size;
            allocInfo.memoryTypeIndex =
                findDedicatedMemoryType(memReq.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

            auto vertexBufferMemory = _device.allocateMemory(allocInfo);
            _device.bindBufferMemory(vertexBuffer, vertexBufferMemory, 0);

            submitSingleTimeCommands(_device, _command_pool, _queue, [&](vk::CommandBuffer cmd) {
                cmd.copyBuffer(stagingBuffer, vertexBuffer, vk::BufferCopy(0, 0, size));
            });

            _device.freeMemory(stagingBufferMemory);
            _device.destroyBuffer(stagingBuffer);

            return {vertexBuffer, vertexBufferMemory};
        });
    }

    std::shared_ptr<texture> create_attachment(int width, int height, common::bit_depth depth)
    {
        CASPAR_VERIFY(width > 0 && height > 0);

        auto depth_pool_index = depth == common::bit_depth::bit8 ? 0 : 1;
        auto format = depth == common::bit_depth::bit8 ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR16G16B16A16Unorm;

        // TODO (perf) Shared pool.
        auto pool   = &attachment_pools_[depth_pool_index][(width << 16 & 0xFFFF0000) | (height & 0x0000FFFF)];
        auto extent = vk::Extent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

        std::shared_ptr<texture> tex;
        if (!pool->try_pop(tex)) {
            vk::ImageCreateInfo imageInfo{};
            imageInfo.imageType     = vk::ImageType::e2D;
            imageInfo.format        = format;
            imageInfo.extent        = extent;
            imageInfo.mipLevels     = 1;
            imageInfo.arrayLayers   = 1;
            imageInfo.initialLayout = vk::ImageLayout::eUndefined;
            imageInfo.samples       = vk::SampleCountFlagBits::e1;
            imageInfo.tiling        = vk::ImageTiling::eOptimal;
            imageInfo.usage         = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eInputAttachment |
                              vk::ImageUsageFlagBits::eColorAttachment;
            imageInfo.sharingMode = vk::SharingMode::eExclusive;
            auto image            = _device.createImage(imageInfo);

            auto memReq = _device.getImageMemoryRequirements(image);

            vk::MemoryAllocateInfo allocInfo{};
            allocInfo.allocationSize = memReq.size;
            allocInfo.memoryTypeIndex =
                findDedicatedMemoryType(memReq.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

            auto imageMemory = _device.allocateMemory(allocInfo);
            _device.bindImageMemory(image, imageMemory, 0);
            auto clearValue = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
            auto range      = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

            vk::ImageViewCreateInfo createInfo(
                {}, image, vk::ImageViewType::e2D, format, vk::ComponentMapping(), range);

            auto imageView = _device.createImageView(createInfo);

            tex = std::make_shared<texture>(width, height, 4, depth, image, imageMemory, imageView, _device);
        }
        tex->set_depth(depth);

        auto ptr = tex.get();
        return std::shared_ptr<texture>(
            ptr, [tex = std::move(tex), pool, self = shared_from_this()](texture*) mutable { pool->push(tex); });
    }

    std::shared_ptr<texture> create_texture(int width, int height, int stride, common::bit_depth depth, bool clear)
    {
        CASPAR_VERIFY(stride > 0 && stride < 5);
        CASPAR_VERIFY(width > 0 && height > 0);

        static vk::Format INTERNAL_FORMAT[][5] = {{vk::Format::eUndefined,
                                                   vk::Format::eR8Unorm,
                                                   vk::Format::eR8G8Unorm,
                                                   vk::Format::eR8G8B8Unorm,
                                                   vk::Format::eR8G8B8A8Unorm},
                                                  {vk::Format::eUndefined,
                                                   vk::Format::eR16Unorm,
                                                   vk::Format::eR16G16Unorm,
                                                   vk::Format::eR16G16B16Unorm,
                                                   vk::Format::eR16G16B16A16Unorm}};

        auto depth_pool_index = depth == common::bit_depth::bit8 ? 0 : 1;
        auto format           = INTERNAL_FORMAT[depth_pool_index][stride];

        // TODO (perf) Shared pool.
        auto pool   = &device_pools_[depth_pool_index][stride - 1][(width << 16 & 0xFFFF0000) | (height & 0x0000FFFF)];
        auto extent = vk::Extent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        std::shared_ptr<texture> tex;
        if (!pool->try_pop(tex)) {
            vk::ImageCreateInfo imageInfo{};
            imageInfo.imageType     = vk::ImageType::e2D;
            imageInfo.format        = format;
            imageInfo.extent        = extent;
            imageInfo.mipLevels     = 1;
            imageInfo.arrayLayers   = 1;
            imageInfo.initialLayout = vk::ImageLayout::eUndefined;
            imageInfo.samples       = vk::SampleCountFlagBits::e1;
            imageInfo.tiling        = vk::ImageTiling::eOptimal;
            imageInfo.usage         = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
            imageInfo.sharingMode   = vk::SharingMode::eExclusive;
            auto image              = _device.createImage(imageInfo);

            auto memReq = _device.getImageMemoryRequirements(image);

            vk::MemoryAllocateInfo allocInfo{};
            allocInfo.allocationSize = memReq.size;
            allocInfo.memoryTypeIndex =
                findDedicatedMemoryType(memReq.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

            auto imageMemory = _device.allocateMemory(allocInfo);
            _device.bindImageMemory(image, imageMemory, 0);
            auto clearValue = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
            auto range      = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

            vk::ImageViewCreateInfo createInfo(
                {}, image, vk::ImageViewType::e2D, format, vk::ComponentMapping(), range);

            auto imageView = _device.createImageView(createInfo);

            tex = std::make_shared<texture>(width, height, stride, depth, image, imageMemory, imageView, _device);
        }
        tex->set_depth(depth);

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

        // TODO (perf) Shared pool.
        auto pool = &host_pools_[static_cast<int>(write ? 1 : 0)][size];

        std::shared_ptr<buffer> buf;
        if (!pool->try_pop(buf)) {
            // TODO (perf) Avoid blocking in create_array.
            buf = std::make_shared<buffer>(size, write, _allocator);
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
        return dispatch_async([=] {
            std::shared_ptr<buffer> buf;

            auto tmp = source.storage<std::shared_ptr<buffer>>();
            if (tmp) {
                buf = *tmp;
            } else {
                buf = create_buffer(static_cast<int>(source.size()), true);
                std::memcpy(buf->data(), source.data(), source.size());
            }

            static vk::Format INTERNAL_FORMAT[][5] = {{vk::Format::eUndefined,
                                                       vk::Format::eR8Unorm,
                                                       vk::Format::eR8G8Unorm,
                                                       vk::Format::eR8G8B8Unorm,
                                                       vk::Format::eR8G8B8A8Unorm},
                                                      {vk::Format::eUndefined,
                                                       vk::Format::eR16Unorm,
                                                       vk::Format::eR16G16Unorm,
                                                       vk::Format::eR16G16B16Unorm,
                                                       vk::Format::eR16G16B16A16Unorm}};
            auto              depth_pool_index     = depth == common::bit_depth::bit8 ? 0 : 1;

            auto tex    = create_texture(width, height, stride, depth, false);
            auto format = INTERNAL_FORMAT[depth_pool_index][stride];
            // tex->copy_from(*buf);
            vk::BufferImageCopy region(0,
                                       0,
                                       0,
                                       vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                                       vk::Offset3D(0, 0, 0),
                                       vk::Extent3D(width, height, 1));

            submitSingleTimeCommands(_device, _command_pool, _queue, [&](vk::CommandBuffer cmd) {
                transitionImageLayout(tex->id(),
                                      format,
                                      vk::ImageLayout::eUndefined,
                                      vk::AccessFlagBits2::eNone,
                                      vk::PipelineStageFlagBits2::eTopOfPipe,

                                      vk::ImageLayout::eTransferDstOptimal,
                                      vk::AccessFlagBits2::eTransferWrite,
                                      vk::PipelineStageFlagBits2::eTransfer,
                                      cmd);

                cmd.copyBufferToImage(buf->id(), tex->id(), vk::ImageLayout::eTransferDstOptimal, region);

                transitionImageLayout(tex->id(),
                                      format,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      vk::AccessFlagBits2::eTransferWrite,
                                      vk::PipelineStageFlagBits2::eTransfer,

                                      vk::ImageLayout::eShaderReadOnlyOptimal,
                                      vk::AccessFlagBits2::eShaderRead,
                                      vk::PipelineStageFlagBits2::eFragmentShader,
                                      cmd);
            });
            // TODO (perf) save tex on source
            return tex;
        });
    }

    std::future<array<const uint8_t>> copy_async(const std::shared_ptr<texture>& source)
    {
        return spawn_async([=](yield_context yield) {
            auto buf = create_buffer(source->size(), false);
            source->copy_to(*buf);

            vk::CopyImageToBufferInfo2 copyInfo{};
            copyInfo.dstBuffer      = buf->id();
            copyInfo.srcImage       = source->id();
            copyInfo.srcImageLayout = vk::ImageLayout::eTransferSrcOptimal;

            vk::BufferImageCopy2 region{};
            region.bufferOffset     = 0;
            region.imageSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
            region.imageOffset      = vk::Offset3D{0, 0, 0};
            region.imageExtent =
                vk::Extent3D{static_cast<uint32_t>(source->width()), static_cast<uint32_t>(source->height()), 1};
            copyInfo.setRegions(region);

            auto fence = _device.createFence(vk::FenceCreateInfo());

            submitSingleTimeCommands(
                _device,
                _command_pool,
                _queue,
                [&](vk::CommandBuffer cmd) {
                    transitionImageLayout(source->id(),
                                          vk::Format::eR8G8B8A8Unorm,
                                          vk::ImageLayout::eUndefined,
                                          vk::AccessFlagBits2::eNone,
                                          vk::PipelineStageFlagBits2::eTopOfPipe,

                                          vk::ImageLayout::eTransferSrcOptimal,
                                          vk::AccessFlagBits2::eHostRead,
                                          vk::PipelineStageFlagBits2::eHost,
                                          cmd);
                    cmd.copyImageToBuffer2(copyInfo);
                },
                &fence);

            deadline_timer timer(io_context_);
            for (auto n = 0; true; ++n) {
                // TODO (perf) Smarter non-polling solution?
                timer.expires_from_now(boost::posix_time::milliseconds(2));
                timer.async_wait(yield);

                auto wait = _device.waitForFences(fence, VK_TRUE, 0);
                if (wait == vk::Result::eSuccess) {
                    break;
                }
            }

            _device.destroyFence(fence);

            auto ptr  = reinterpret_cast<uint8_t*>(buf->data());
            auto size = buf->size();
            return array<const uint8_t>(ptr, size, std::move(buf));
        });
    }

    boost::property_tree::wptree info() const
    {
        boost::property_tree::wptree info;

        boost::property_tree::wptree pooled_device_buffers;
        size_t                       total_pooled_device_buffer_size  = 0;
        size_t                       total_pooled_device_buffer_count = 0;

        for (size_t i = 0; i < device_pools_.size(); ++i) {
            auto& depth_pools = device_pools_.at(i);
            for (size_t j = 0; j < depth_pools.size(); ++j) {
                auto& pools      = depth_pools.at(j);
                bool  mipmapping = j > 3;
                auto  stride     = mipmapping ? j - 3 : j + 1;

                for (auto& pool : pools) {
                    auto width  = pool.first >> 16;
                    auto height = pool.first & 0x0000FFFF;
                    auto size   = width * height * stride;
                    auto count  = pool.second.size();

                    if (count == 0)
                        continue;

                    boost::property_tree::wptree pool_info;

                    pool_info.add(L"stride", stride);
                    pool_info.add(L"mipmapping", mipmapping);
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

        info.add_child(L"gl.details.pooled_device_buffers", pooled_device_buffers);

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

        info.add_child(L"gl.details.pooled_host_buffers", pooled_host_buffers);
        info.add(L"gl.summary.pooled_device_buffers.total_count", total_pooled_device_buffer_count);
        info.add(L"gl.summary.pooled_device_buffers.total_size", total_pooled_device_buffer_size);
        // info.add_child(L"gl.summary.all_device_buffers", texture::info());
        info.add(L"gl.summary.pooled_host_buffers.total_read_count", total_read_count);
        info.add(L"gl.summary.pooled_host_buffers.total_write_count", total_write_count);
        info.add(L"gl.summary.pooled_host_buffers.total_read_size", total_read_size);
        info.add(L"gl.summary.pooled_host_buffers.total_write_size", total_write_size);
        info.add_child(L"gl.summary.all_host_buffers", buffer::info());

        return info;
    }

    std::future<void> gc()
    {
        return spawn_async([=](yield_context yield) {
            CASPAR_LOG(info) << " vulkan: Running GC.";

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
    : impl_(new impl())
{
}
device::~device() {}

void device::submit_render_pass(std::shared_ptr<texture>                            attachment,
                                std::function<void(vk::CommandBuffer, vk::Device)>& func)
{
    impl_->submit_render_pass(attachment, func);
}
std::pair<vk::Buffer, vk::DeviceMemory> device::upload_vertex_buffer() { return impl_->upload_vertex_buffer(); }

std::shared_ptr<pipeline> device::create_pipeline() { return impl_->create_pipeline(); }

std::shared_ptr<texture> device::create_attachment(int width, int height, common::bit_depth depth)
{
    return impl_->create_attachment(width, height, depth);
}

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
std::wstring                 device::version() const { return impl_->version(); }
boost::property_tree::wptree device::info() const { return impl_->info(); }
std::future<void>            device::gc() { return impl_->gc(); }
}}} // namespace caspar::accelerator::vulkan
