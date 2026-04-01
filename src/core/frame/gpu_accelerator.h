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
 * Author: Niklas Andersson, niklas@niklaspandersson.se
 */

#pragma once

#include <common/array.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace caspar { namespace core {

struct gpu_image_desc
{
    void*    vk_image;  // VkImage
    uint32_t width;
    uint32_t height;
    uint32_t vk_format; // VkFormat
};

class gpu_accelerator
{
  public:
    virtual ~gpu_accelerator() = default;

    // --- Vulkan device access ---
    // Producers use these to set up their own hw decode pipelines.
    // All returned handles are owned by the accelerator - producers must not destroy them.
    virtual void*    vk_instance() const              = 0; // VkInstance
    virtual void*    vk_physical_device() const      = 0; // VkPhysicalDevice
    virtual void*    vk_device() const               = 0; // VkDevice
    virtual uint32_t queue_family_index() const      = 0;
    virtual void*    vk_queue() const                = 0; // VkQueue
    virtual void*    vk_get_instance_proc_addr() const          = 0; // PFN_vkGetInstanceProcAddr
    virtual const std::vector<std::string>& vk_enabled_device_extensions() const = 0;
    virtual int    vk_decode_queue_family_index() const = 0; // -1 if not available

    // --- VkImage import ---
    // Import externally-owned VkImages (e.g. from hw decode) as a frame.
    // The images must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    // before calling this.
    //
    // lifetime_token: shared_ptr that keeps the source images alive until
    //   the mixer is done with them. Typically wraps the decoded frame
    //   so its GPU surfaces aren't recycled prematurely.
    //
    // Returns a mutable_frame with a commit callback that wraps the
    // VkImages as mixer-compatible textures. No CPU data is copied.
    virtual class mutable_frame import_gpu_images(const void*                        tag,
                                                  const std::vector<gpu_image_desc>& planes,
                                                  const struct pixel_format_desc&    desc,
                                                  array<std::int32_t>                audio_data,
                                                  std::shared_ptr<void>              lifetime_token) = 0;
};

}} // namespace caspar::core
