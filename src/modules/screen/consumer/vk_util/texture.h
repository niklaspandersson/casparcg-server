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

#include <common/bit_depth.h>
#include <memory>

namespace caspar { namespace accelerator { namespace vk {

class buffer;

/**
 * Vulkan texture - GPU image for rendering
 *
 * Provides:
 * - VkImage and VkImageView for shader sampling
 * - Image layout transitions for optimal usage
 * - Copy operations from/to staging buffers
 * - Support for various formats (R, RG, RGB, RGBA) and bit depths (8/16)
 */
class texture final
{
  public:
    // device, physical_device, command_pool, and queue are Vulkan handles cast to void*
    texture(void*             device,
            void*             physical_device,
            void*             command_pool,
            void*             queue,
            int               width,
            int               height,
            int               stride,
            common::bit_depth depth = common::bit_depth::bit8);
    texture(const texture&) = delete;
    texture(texture&& other);
    ~texture();

    texture& operator=(const texture&) = delete;
    texture& operator=(texture&& other);

    // Copy operations
    void copy_from(buffer& source);  // CPU -> GPU
    void copy_to(buffer& dest);      // GPU -> CPU

    // Clear the texture to zero
    void clear();

    // Vulkan handle accessors
    void* image() const;       // VkImage
    void* image_view() const;  // VkImageView

    // Properties
    int               width() const;
    int               height() const;
    int               stride() const;
    common::bit_depth depth() const;
    int               size() const;
    int               format() const;  // VkFormat as int

    // Phase 4: Compute shader support
    // Transition to GENERAL layout for compute read/write
    void transition_to_general();
    // Transition to SHADER_READ_ONLY_OPTIMAL after compute
    void transition_to_shader_read();

    // Phase 15: Update internal layout tracking (for external transitions by graphics pipeline)
    void set_layout(int layout);

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}}} // namespace caspar::accelerator::vk
