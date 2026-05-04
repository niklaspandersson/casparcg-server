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

#include "completion_token.h"

#include <common/bit_depth.h>
#include <core/frame/frame.h>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace caspar { namespace accelerator { namespace vulkan {

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

    // Typed image handle and the (color, 0..1, 0..1) subresource range, for the
    // queue-ownership-transfer helpers.
    vk::Image                 image() const;
    vk::ImageSubresourceRange subresource_range() const;

    // Tracked image layout + owning queue family. Defaulted to eUndefined /
    // VK_QUEUE_FAMILY_IGNORED. Opt-in: callers that perform layout transitions or
    // inter-queue ownership transfers keep these in sync via set_*; code that
    // passes layouts explicitly (the legacy upload/readback path) can ignore them.
    vk::ImageLayout current_layout() const;
    void            set_current_layout(vk::ImageLayout layout);
    uint32_t        owner_family() const;
    void            set_owner_family(uint32_t family_index);

    // Per-texture GPU dependency tracking for the "single write, then reads, then
    // recycle" lifecycle. Cross-queue ordering uses these tokens; same-queue
    // ordering is handled by barriers and is filtered out by command_context.
    //
    //  - note_write(t):  records the single write (t), clearing prior reads.
    //  - write_token():  that write, for a reader to wait on (RAW).
    //  - note_read(t):   records a read (kept as one entry per queue/timeline).
    //  - read_dependencies(): the tokens the next writer must wait on (WAR) — the
    //                    reads since the last write, or the write itself if none.
    const completion_token&       write_token() const;
    void                          note_write(const completion_token& token);
    void                          note_read(const completion_token& token);
    std::vector<completion_token> read_dependencies() const;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}}} // namespace caspar::accelerator::vulkan
