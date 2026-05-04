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

#include "queue_manager.h" // queue_ownership_transfer

#include <vulkan/vulkan.hpp>

namespace caspar { namespace accelerator { namespace vulkan {

class command_context;
class vulkan_queue;
class texture;

// Inter-queue texture ownership transfer, built on command_context. A producer
// records a queue-family release barrier on its own queue, then the consumer
// records the matching acquire barrier on its queue; a binary semaphore orders
// the two submits. When src and dst share a family the queue-family transfer is
// elided (QueueFamilyIgnored) but the semaphore + layout transition still apply.
//
// The texture's tracked layout/owner (texture::current_layout / owner_family)
// are updated so the next operation sees the post-transfer state.

// Producer side. Records the release barrier on src_cc (transitioning the
// texture from its tracked layout to dst_layout, releasing to dst's family) and
// submits it signalling a fresh binary semaphore. Returns the token to hand to
// acquire_texture. src_stage/src_access describe the last use on the source queue.
queue_ownership_transfer release_texture(command_context&        src_cc,
                                         vulkan_queue&           dst,
                                         texture&                tex,
                                         vk::ImageLayout         dst_layout,
                                         vk::PipelineStageFlags2 src_stage,
                                         vk::AccessFlags2        src_access);

// Consumer side. Records the matching acquire barrier on dst_cc waiting on
// token.semaphore, and keeps the semaphore alive until that submit retires.
// dst_stage/dst_access describe the first use on the destination queue. Consumes
// the token (its semaphore handle is cleared on return).
void acquire_texture(command_context&          dst_cc,
                     queue_ownership_transfer& token,
                     texture&                  tex,
                     vk::PipelineStageFlags2   dst_stage,
                     vk::AccessFlags2          dst_access);

}}} // namespace caspar::accelerator::vulkan
