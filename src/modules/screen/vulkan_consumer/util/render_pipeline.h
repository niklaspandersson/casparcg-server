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

#include <accelerator/vulkan/util/completion_token.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace caspar { namespace accelerator { namespace vulkan {
class texture;
}}} // namespace caspar::accelerator::vulkan

namespace caspar { namespace screen { namespace vulkan {

class swapchain;

/**
 * Push constants structure for screen rendering shader.
 * Must match the layout in screen.vert and screen.frag
 */
struct screen_push_constants
{
    float   pos_scale[2];  // Position scale for aspect ratio
    float   pos_offset[2]; // Position offset
    float   tex_scale[2];  // Texture coordinate scale
    float   tex_offset[2]; // Texture coordinate offset
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
     * @param device Logical device
     * @param physical_device Physical device
     * @param command_pool Command pool to allocate command buffers from
     * @param queue Queue to submit rendering to
     * @param swapchain Swapchain to render to
     */
    render_pipeline(vk::Device         device,
                    vk::PhysicalDevice physical_device,
                    vk::CommandPool    command_pool,
                    vk::Queue          queue,
                    swapchain&         swap);
    ~render_pipeline();

    render_pipeline(const render_pipeline&)            = delete;
    render_pipeline& operator=(const render_pipeline&) = delete;

    /**
     * Sample the channel's composited texture directly and present it.
     *
     * GPU-direct: `src` is the texture produced by the mixer on the accelerator's
     * renderer queue — no host copy or buffer->image upload. The submit waits on
     * `wait_tokens` (the renderer's cross-queue completion of `src`, converted to
     * timeline-semaphore waits) in addition to the swapchain's image-available
     * semaphore, and a barrier transitions `src` from `src_layout` to
     * shader-read-only on the present queue before the draw samples it.
     *
     * Assumes the renderer and present queues share a queue family (the common
     * single-graphics-family case); a cross-family `src` would additionally need
     * a queue-family ownership transfer recorded on the producer side.
     *
     * @param src Composited texture sampled by the shader
     * @param src_layout Layout `src` is currently in (from texture::current_layout)
     * @param wait_tokens Renderer completion tokens to wait on (RAW dependency)
     * @param image_index Swapchain image index
     * @param frame_slot Frame-in-flight slot (selects command buffer + descriptor)
     * @param params Render parameters
     * @param wait_semaphore image-available semaphore to wait on
     * @param signal_semaphore render-finished semaphore to signal
     * @param fence Fence to signal after submission
     */
    void render(accelerator::vulkan::texture&                             src,
                vk::ImageLayout                                           src_layout,
                const std::vector<accelerator::vulkan::completion_token>& wait_tokens,
                uint32_t                                                  image_index,
                uint32_t                                                  frame_slot,
                const screen_push_constants&                              params,
                vk::Semaphore                                             wait_semaphore,
                vk::Semaphore                                             signal_semaphore,
                vk::Fence                                                 fence);

    /**
     * Recreate framebuffers after swapchain recreation.
     */
    void recreate_framebuffers();

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}}} // namespace caspar::screen::vulkan
