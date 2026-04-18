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
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace caspar { namespace core {

struct gpu_image_desc
{
    // --- image identity ---
    void*    vk_image;  // VkImage
    uint32_t width;
    uint32_t height;
    uint32_t vk_format; // VkFormat
    uint32_t vk_aspect; // VkImageAspectFlags (0 -> default to COLOR)

    // --- current state on hand-off (filled in by external producers) ---
    // current_layout == 0 (VK_IMAGE_LAYOUT_UNDEFINED) means "discard, don't care".
    uint32_t current_layout   = 0;  // VkImageLayout
    // VK_QUEUE_FAMILY_IGNORED (~0u) for CONCURRENT-shared images.
    uint32_t src_queue_family = ~0u;
    uint64_t src_access_mask  = 0;  // VkAccessFlags2
    uint64_t src_stage_mask   = 0;  // VkPipelineStageFlags2

    // --- target state the mixer transitions to before sampling ---
    // Defaults to 0 (UNDEFINED); the mixer treats 0 as "leave alone".
    uint32_t target_layout    = 0;  // VkImageLayout

    // --- timeline semaphore sync (optional) ---
    // wait_semaphore == nullptr ⇒ no wait/signal emitted.
    void*    wait_semaphore   = nullptr; // VkSemaphore (timeline)
    uint64_t wait_value       = 0;
    void*    signal_semaphore = nullptr; // usually equal to wait_semaphore
    uint64_t signal_value     = 0;

    // --- writeback ---
    // Called synchronously by the mixer at submit-record time so the producer
    // can update its bookkeeping (e.g. AVVkFrame::layout/queue_family/sem_value/access)
    // before the next decode reads it. Optional.
    std::function<void(uint32_t new_layout,
                       uint32_t new_queue_family,
                       uint64_t new_sem_value,
                       uint64_t new_access_mask)>
        writeback;
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

    // Returns true when the named Vulkan device extension is enabled.
    // Prefer this over iterating vk_enabled_device_extensions() directly.
    virtual bool has_extension(const std::string& name) const = 0;

    // True only when the accelerator failed to allocate a separate VkQueue for
    // external producers and is sharing the render queue with them.  In that
    // (fallback) case, an external hw-decode context must call
    // vk_lock_queue/vk_unlock_queue around its vkQueueSubmit calls.  In the
    // normal case the accelerator hands the external producer a queue it never
    // touches and no locking is required.
    virtual bool vk_shared_render_queue() const = 0;

    // Manual lock/unlock for the render queue. Only meaningful when
    // vk_shared_render_queue() is true; otherwise these are no-ops.
    virtual void vk_lock_queue()   = 0;
    virtual void vk_unlock_queue() = 0;

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
