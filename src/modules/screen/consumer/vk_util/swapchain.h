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

#include <cstdint>
#include <memory>
#include <vector>

// Forward declare GLFW types
struct GLFWwindow;

namespace caspar { namespace accelerator { namespace vk {

class texture;

/**
 * Vulkan swapchain for window presentation.
 * Phase 9: Screen consumer support.
 *
 * Provides:
 * - Vulkan surface creation from GLFW window
 * - Swapchain creation and management
 * - Image acquisition and presentation
 * - VSync control
 * - Resize handling
 */
class swapchain final
{
  public:
    /**
     * Create a swapchain for the given GLFW window.
     *
     * @param instance VkInstance handle (cast to void*)
     * @param physical_device VkPhysicalDevice handle (cast to void*)
     * @param device VkDevice handle (cast to void*)
     * @param queue VkQueue handle (cast to void*)
     * @param queue_family_index Queue family index for presentation
     * @param window GLFW window pointer
     * @param vsync Enable vertical sync
     * @param pre_created_surface Optional pre-created VkSurfaceKHR (for macOS Metal layer control)
     */
    swapchain(void*       instance,
              void*       physical_device,
              void*       device,
              void*       queue,
              uint32_t    queue_family_index,
              GLFWwindow* window,
              bool        vsync,
              void*       pre_created_surface = nullptr);
    ~swapchain();

    swapchain(const swapchain&)            = delete;
    swapchain& operator=(const swapchain&) = delete;

    /**
     * Acquire the next swapchain image.
     *
     * @return Image index, or UINT32_MAX if swapchain needs recreation
     */
    uint32_t acquire_next_image();

    /**
     * Present the current swapchain image.
     *
     * @param image_index Index returned from acquire_next_image
     * @return true if successful, false if swapchain needs recreation
     */
    bool present(uint32_t image_index);

    /**
     * Get the swapchain image view at the given index.
     *
     * @param index Image index
     * @return VkImageView handle (cast to void*)
     */
    void* get_image_view(uint32_t index) const;

    /**
     * Get the swapchain image at the given index.
     *
     * @param index Image index
     * @return VkImage handle (cast to void*)
     */
    void* get_image(uint32_t index) const;

    /**
     * Get the number of swapchain images.
     */
    uint32_t image_count() const;

    /**
     * Get the swapchain extent (width, height).
     */
    void get_extent(uint32_t& width, uint32_t& height) const;

    /**
     * Get the swapchain format.
     *
     * @return VkFormat as int
     */
    int format() const;

    /**
     * Recreate the swapchain (e.g., after window resize).
     */
    void recreate();

    /**
     * Get the image available semaphore for the current frame.
     *
     * @return VkSemaphore handle (cast to void*)
     */
    void* image_available_semaphore() const;

    /**
     * Get the render finished semaphore for the current frame.
     *
     * @return VkSemaphore handle (cast to void*)
     */
    void* render_finished_semaphore() const;

    /**
     * Get the in-flight fence for the current frame.
     *
     * @return VkFence handle (cast to void*)
     */
    void* in_flight_fence() const;

    /**
     * Wait for the current frame's fence to signal.
     */
    void wait_for_fence();

    /**
     * Reset the current frame's fence.
     */
    void reset_fence();

    /**
     * Advance to the next frame for synchronization objects.
     */
    void next_frame();

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}}} // namespace caspar::accelerator::vk
