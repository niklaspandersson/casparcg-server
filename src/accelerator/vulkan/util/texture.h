/*
 * Copyright 2025
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
 * Author: Niklas Andersson, niklas@niklaspandersson.se
 */

#pragma once

#include <common/bit_depth.h>
#include <core/frame/frame.h>
#include <functional>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace caspar { namespace accelerator { namespace vulkan {

// Sync metadata attached to externally-owned VkImages so the mixer can issue
// the right acquire/release barriers and timeline-semaphore waits/signals.
// All fields are optional; null/zero means "skip".
struct external_sync
{
    vk::ImageAspectFlags    aspect          = vk::ImageAspectFlagBits::eColor;
    vk::ImageLayout         current_layout  = vk::ImageLayout::eUndefined;
    vk::ImageLayout         target_layout   = vk::ImageLayout::eShaderReadOnlyOptimal;
    uint32_t                src_queue_family = VK_QUEUE_FAMILY_IGNORED;
    vk::AccessFlags2        src_access      = {};
    vk::PipelineStageFlags2 src_stage       = vk::PipelineStageFlagBits2::eTopOfPipe;

    vk::Semaphore wait_semaphore   = nullptr;
    uint64_t      wait_value       = 0;
    vk::Semaphore signal_semaphore = nullptr;
    uint64_t      signal_value     = 0;

    std::function<void(uint32_t new_layout,
                       uint32_t new_queue_family,
                       uint64_t new_sem_value,
                       uint64_t new_access)>
        writeback;
};

class texture final
{
  public:
    texture(int               width,
            int               height,
            int               stride,
            common::bit_depth depth,
            vk::Image         image,
            vk::DeviceMemory  memory,
            vk::ImageView     imageView,
            vk::Device        device);
    texture(const texture&) = delete;
    texture(texture&& other);
    ~texture();

    texture& operator=(const texture&) = delete;
    texture& operator=(texture&& other);

    vk::ImageView view() const;

    int               width() const;
    int               height() const;
    int               stride() const;
    common::bit_depth depth() const;
    void              set_depth(common::bit_depth depth);
    int               size() const;
    VkImage           id() const;

    // External-sync metadata + lifetime token (only set for textures created via
    // wrap_external). external_sync_info() returns nullptr for normal textures.
    const external_sync*         external_sync_info() const;
    const std::shared_ptr<void>& external_lifetime() const;

    // Wrap an externally-owned VkImage. Only the VkImageView is created and destroyed
    // by this texture - the image and its memory are owned by the caller.
    // The caller must ensure the VkImage outlives this texture (the lifetime_token
    // overload pins it for as long as the texture is alive).
    static std::shared_ptr<texture> wrap_external(vk::Device           device,
                                                  vk::Image            image,
                                                  int                  width,
                                                  int                  height,
                                                  int                  stride,
                                                  vk::Format           format,
                                                  common::bit_depth    depth,
                                                  vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);

    static std::shared_ptr<texture> wrap_external(vk::Device            device,
                                                  vk::Image             image,
                                                  int                   width,
                                                  int                   height,
                                                  int                   stride,
                                                  vk::Format            format,
                                                  common::bit_depth     depth,
                                                  external_sync         sync,
                                                  std::shared_ptr<void> lifetime_token);

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}}} // namespace caspar::accelerator::vulkan
