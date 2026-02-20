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

#include "swapchain.h"
#include "vk_check.h"

#include <common/log.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace caspar { namespace accelerator { namespace vk {

static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

struct swapchain::impl
{
    VkInstance       instance_        = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice         device_          = VK_NULL_HANDLE;
    VkQueue          queue_           = VK_NULL_HANDLE;
    uint32_t         queue_family_index_;
    GLFWwindow*      window_ = nullptr;
    bool             vsync_  = false;

    VkSurfaceKHR   surface_   = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;

    std::vector<VkImage>     swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;
    VkFormat                 swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D               swapchain_extent_ = {0, 0};

    // Synchronization objects (per frame in flight)
    std::vector<VkSemaphore> image_available_semaphores_;
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::vector<VkFence>     in_flight_fences_;
    size_t                   current_frame_ = 0;

    impl(void*       instance,
         void*       physical_device,
         void*       device,
         void*       queue,
         uint32_t    queue_family_index,
         GLFWwindow* window,
         bool        vsync,
         void*       pre_created_surface = nullptr)
        : instance_(static_cast<VkInstance>(instance))
        , physical_device_(static_cast<VkPhysicalDevice>(physical_device))
        , device_(static_cast<VkDevice>(device))
        , queue_(static_cast<VkQueue>(queue))
        , queue_family_index_(queue_family_index)
        , window_(window)
        , vsync_(vsync)
    {
        if (pre_created_surface) {
            set_surface(static_cast<VkSurfaceKHR>(pre_created_surface));
            CASPAR_LOG(info) << L"[vk::swapchain] Using pre-created Vulkan surface";
        } else {
            create_surface();
        }
        create_swapchain();
        create_image_views();
        create_sync_objects();

        CASPAR_LOG(info) << L"[vk::swapchain] Vulkan swapchain initialized (" << swapchain_extent_.width << L"x"
                         << swapchain_extent_.height << L", " << swapchain_images_.size() << L" images"
                         << (vsync_ ? L", vsync" : L"") << L")";
    }

    ~impl()
    {
        cleanup_swapchain();
        cleanup_sync_objects();

        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }

        CASPAR_LOG(info) << L"[vk::swapchain] Vulkan swapchain destroyed";
    }

    void create_surface()
    {
        VkResult result = glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
        if (result != VK_SUCCESS) {
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception() << msg_info("Failed to create Vulkan window surface"));
        }

        // Verify the queue family supports presentation
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_index_, surface_, &presentSupport);
        if (!presentSupport) {
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception() << msg_info("Queue family does not support presentation to surface"));
        }
    }

    void set_surface(VkSurfaceKHR surface)
    {
        surface_ = surface;

        // Verify the queue family supports presentation
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_index_, surface_, &presentSupport);
        if (!presentSupport) {
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception() << msg_info("Queue family does not support presentation to surface"));
        }
    }

    VkSurfaceFormatKHR choose_surface_format()
    {
        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, nullptr);

        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &formatCount, formats.data());

        // Log available formats for debugging
        CASPAR_LOG(debug) << L"[vk::swapchain] Available surface formats: " << formatCount;
        for (const auto& format : formats) {
            CASPAR_LOG(debug) << L"[vk::swapchain]   Format: " << format.format << L", ColorSpace: " << format.colorSpace;
        }

        // Prefer BGRA8 UNORM for direct color passthrough (matches frame texture format)
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                CASPAR_LOG(info) << L"[vk::swapchain] Selected format: B8G8R8A8_UNORM";
                return format;
            }
        }

        // Fall back to BGRA8 SRGB
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                CASPAR_LOG(info) << L"[vk::swapchain] Selected format: B8G8R8A8_SRGB (fallback)";
                return format;
            }
        }

        // Try any BGRA8 format
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_SRGB) {
                CASPAR_LOG(info) << L"[vk::swapchain] Selected format: " << format.format;
                return format;
            }
        }

        // Just use the first available format
        CASPAR_LOG(warning) << L"[vk::swapchain] Using first available format: " << formats[0].format;
        return formats[0];
    }

    VkPresentModeKHR choose_present_mode()
    {
        uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &modeCount, nullptr);

        std::vector<VkPresentModeKHR> modes(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &modeCount, modes.data());

        if (vsync_) {
            // VSync: use FIFO (guaranteed to be available)
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        // No VSync: prefer mailbox (triple buffering), then immediate
        for (const auto& mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return mode;
            }
        }

        for (const auto& mode : modes) {
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                return mode;
            }
        }

        // Fall back to FIFO
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& capabilities)
    {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }

        int width, height;
        glfwGetFramebufferSize(window_, &width, &height);

        VkExtent2D extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

        extent.width  = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

        return extent;
    }

    void create_swapchain()
    {
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);

        auto surfaceFormat = choose_surface_format();
        auto presentMode   = choose_present_mode();
        auto extent        = choose_extent(capabilities);

        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
            imageCount = capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface          = surface_;
        createInfo.minImageCount    = imageCount;
        createInfo.imageFormat      = surfaceFormat.format;
        createInfo.imageColorSpace  = surfaceFormat.colorSpace;
        createInfo.imageExtent      = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform     = capabilities.currentTransform;
        createInfo.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode      = presentMode;
        createInfo.clipped          = VK_TRUE;
        createInfo.oldSwapchain     = VK_NULL_HANDLE;

        VK(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_));

        // Get swapchain images
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        swapchain_images_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchain_images_.data());

        swapchain_format_ = surfaceFormat.format;
        swapchain_extent_ = extent;
    }

    void create_image_views()
    {
        swapchain_image_views_.resize(swapchain_images_.size());

        for (size_t i = 0; i < swapchain_images_.size(); ++i) {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image                           = swapchain_images_[i];
            createInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format                          = swapchain_format_;
            createInfo.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel   = 0;
            createInfo.subresourceRange.levelCount     = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount     = 1;

            VK(vkCreateImageView(device_, &createInfo, nullptr, &swapchain_image_views_[i]));
        }
    }

    void create_sync_objects()
    {
        image_available_semaphores_.resize(MAX_FRAMES_IN_FLIGHT);
        render_finished_semaphores_.resize(MAX_FRAMES_IN_FLIGHT);
        in_flight_fences_.resize(MAX_FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            VK(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &image_available_semaphores_[i]));
            VK(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &render_finished_semaphores_[i]));
            VK(vkCreateFence(device_, &fenceInfo, nullptr, &in_flight_fences_[i]));
        }
    }

    void cleanup_swapchain()
    {
        for (auto imageView : swapchain_image_views_) {
            vkDestroyImageView(device_, imageView, nullptr);
        }
        swapchain_image_views_.clear();

        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }

    void cleanup_sync_objects()
    {
        for (auto semaphore : image_available_semaphores_) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        for (auto semaphore : render_finished_semaphores_) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        for (auto fence : in_flight_fences_) {
            vkDestroyFence(device_, fence, nullptr);
        }
        image_available_semaphores_.clear();
        render_finished_semaphores_.clear();
        in_flight_fences_.clear();
    }

    void recreate()
    {
        // Wait for window to be non-zero size
        int width = 0, height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window_, &width, &height);
            glfwWaitEvents();
        }

        vkDeviceWaitIdle(device_);

        cleanup_swapchain();
        create_swapchain();
        create_image_views();

        CASPAR_LOG(info) << L"[vk::swapchain] Swapchain recreated (" << swapchain_extent_.width << L"x"
                         << swapchain_extent_.height << L")";
    }

    uint32_t acquire_next_image()
    {
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(device_,
                                                swapchain_,
                                                std::numeric_limits<uint64_t>::max(),
                                                image_available_semaphores_[current_frame_],
                                                VK_NULL_HANDLE,
                                                &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            return std::numeric_limits<uint32_t>::max();
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception() << msg_info("Failed to acquire swapchain image"));
        }

        return imageIndex;
    }

    bool present(uint32_t imageIndex)
    {
        VkSemaphore signalSemaphores[] = {render_finished_semaphores_[current_frame_]};

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores    = signalSemaphores;
        presentInfo.swapchainCount     = 1;
        presentInfo.pSwapchains        = &swapchain_;
        presentInfo.pImageIndices      = &imageIndex;

        VkResult result = vkQueuePresentKHR(queue_, &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            return false;
        } else if (result != VK_SUCCESS) {
            CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception() << msg_info("Failed to present swapchain image"));
        }

        return true;
    }

    void wait_for_fence()
    {
        vkWaitForFences(device_, 1, &in_flight_fences_[current_frame_], VK_TRUE, std::numeric_limits<uint64_t>::max());
    }

    void reset_fence() { vkResetFences(device_, 1, &in_flight_fences_[current_frame_]); }

    void next_frame() { current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT; }
};

swapchain::swapchain(void*       instance,
                     void*       physical_device,
                     void*       device,
                     void*       queue,
                     uint32_t    queue_family_index,
                     GLFWwindow* window,
                     bool        vsync,
                     void*       pre_created_surface)
    : impl_(std::make_unique<impl>(instance, physical_device, device, queue, queue_family_index, window, vsync, pre_created_surface))
{
}

swapchain::~swapchain() = default;

uint32_t swapchain::acquire_next_image() { return impl_->acquire_next_image(); }

bool swapchain::present(uint32_t image_index) { return impl_->present(image_index); }

void* swapchain::get_image_view(uint32_t index) const { return impl_->swapchain_image_views_[index]; }

void* swapchain::get_image(uint32_t index) const { return impl_->swapchain_images_[index]; }

uint32_t swapchain::image_count() const { return static_cast<uint32_t>(impl_->swapchain_images_.size()); }

void swapchain::get_extent(uint32_t& width, uint32_t& height) const
{
    width  = impl_->swapchain_extent_.width;
    height = impl_->swapchain_extent_.height;
}

int swapchain::format() const { return static_cast<int>(impl_->swapchain_format_); }

void swapchain::recreate() { impl_->recreate(); }

void* swapchain::image_available_semaphore() const { return impl_->image_available_semaphores_[impl_->current_frame_]; }

void* swapchain::render_finished_semaphore() const { return impl_->render_finished_semaphores_[impl_->current_frame_]; }

void* swapchain::in_flight_fence() const { return impl_->in_flight_fences_[impl_->current_frame_]; }

void swapchain::wait_for_fence() { impl_->wait_for_fence(); }

void swapchain::reset_fence() { impl_->reset_fence(); }

void swapchain::next_frame() { impl_->next_frame(); }

}}} // namespace caspar::accelerator::vk
