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

namespace caspar { namespace accelerator { namespace vk {

class texture;
class swapchain;

/**
 * Push constants structure for screen rendering shader.
 * Must match the layout in screen.vert and screen.frag
 */
struct screen_push_constants
{
    float pos_scale[2];    // Position scale for aspect ratio
    float pos_offset[2];   // Position offset
    float tex_scale[2];    // Texture coordinate scale
    float tex_offset[2];   // Texture coordinate offset
    int32_t key_only;      // Show alpha channel only
    int32_t colour_space;  // 0=RGB, 1=datavideo_full, 2=datavideo_limited
    int32_t window_width;  // Window width for DataVideo conversion
    int32_t _pad;          // Padding
};

/**
 * Vulkan graphics pipeline for screen rendering.
 * Phase 9: Screen consumer support.
 *
 * Provides:
 * - Graphics pipeline for rendering textures to swapchain
 * - Render pass management
 * - Framebuffer creation per swapchain image
 * - Command buffer recording for render operations
 */
class render_pipeline final
{
  public:
    /**
     * Create a render pipeline for screen presentation.
     *
     * @param device VkDevice handle (cast to void*)
     * @param physical_device VkPhysicalDevice handle (cast to void*)
     * @param command_pool VkCommandPool handle (cast to void*)
     * @param queue VkQueue handle (cast to void*)
     * @param swapchain Swapchain to render to
     */
    render_pipeline(void*      device,
                    void*      physical_device,
                    void*      command_pool,
                    void*      queue,
                    swapchain& swap);
    ~render_pipeline();

    render_pipeline(const render_pipeline&)            = delete;
    render_pipeline& operator=(const render_pipeline&) = delete;

    /**
     * Record and submit render commands to display a texture.
     *
     * @param src Source texture to display
     * @param image_index Swapchain image index
     * @param params Render parameters
     * @param wait_semaphore Semaphore to wait on before rendering (cast to void*)
     * @param signal_semaphore Semaphore to signal after rendering (cast to void*)
     * @param fence Fence to signal after submission (cast to void*)
     */
    void render(texture&                     src,
                uint32_t                     image_index,
                const screen_push_constants& params,
                void*                        wait_semaphore,
                void*                        signal_semaphore,
                void*                        fence);

    /**
     * Recreate framebuffers after swapchain recreation.
     */
    void recreate_framebuffers();

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}}} // namespace caspar::accelerator::vk
