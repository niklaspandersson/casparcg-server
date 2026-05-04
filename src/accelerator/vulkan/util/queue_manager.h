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

#include <vulkan/vulkan.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace caspar { namespace accelerator { namespace vulkan {

class vulkan_queue;

// Snapshot of one VkQueueFamily's capabilities + how many queues we requested
// from it via plan(). queues_free is best-effort (sampled under the manager's
// lock at the call site).
struct queue_family_info
{
    uint32_t                        family_index = 0;
    vk::QueueFlags                  queue_flags{};
    vk::VideoCodecOperationFlagsKHR video_codec_ops{};
    uint32_t                        queue_count          = 0;
    uint32_t                        queues_created       = 0;
    uint32_t                        queues_free          = 0;
    uint32_t                        timestamp_valid_bits = 0;
    std::array<uint32_t, 3>         min_image_transfer_granularity{0, 0, 0};
};

// Output of plan(): which queues to ask vkCreateDevice for.
// priorities.size() == queues to create from this family.
struct planned_queue_set
{
    uint32_t           family_index = 0;
    std::vector<float> priorities;
};

// Producer-facing acquire request.
struct queue_request
{
    vk::QueueFlags                  required_flags{};
    vk::QueueFlags                  forbidden_flags{};
    vk::VideoCodecOperationFlagsKHR required_video_codec_ops{};
    bool                            prefer_dedicated = false;
    bool                            prefer_exclusive = true;
    std::string                     label;
};

// Threaded from the source queue's release_*_to() to the destination queue's
// acquire_*_from(). Owns the binary semaphore via semaphore_owner — when the
// destination's submit completion drops it, vkDestroySemaphore is called.
// struct queue_ownership_transfer
// {
//     vk::Semaphore           semaphore;
//     uint32_t                src_family_index = 0;
//     uint32_t                dst_family_index = 0;
//     vk::PipelineStageFlags2 dst_wait_stage   = vk::PipelineStageFlagBits2::eAllCommands;
//     std::shared_ptr<void>   semaphore_owner;
// };

// Vulkan queue family scanner + queue allocator. Three-phase lifecycle:
//   1. ctor: scan VkQueueFamilyProperties on the physical device
//   2. plan(): decide creation policy; output goes to vkb::DeviceBuilder
//   3. populate(): after vkCreateDevice, pull VkQueue handles into slots
// After that, acquire_queue() hands out vulkan_queue handles to consumers.
//
// Held via shared_ptr because vulkan_queue handles can outlive whoever
// constructed the manager; the handle's deleter keeps the manager alive
// (and therefore the slot's mutex/refcount alive).
class queue_manager final : public std::enable_shared_from_this<queue_manager>
{
  public:
    queue_manager(vk::PhysicalDevice phys_dev, bool video_queue_enabled);

    queue_manager(const queue_manager&)            = delete;
    queue_manager& operator=(const queue_manager&) = delete;

    // Phase 1.5: decide which families to take queues from. Records the
    // request internally so populate() can wire matching slots. Result is the
    // list to feed to vkb::DeviceBuilder::custom_queue_setup.
    std::vector<planned_queue_set> plan();

    // Phase 2: VkDevice has been created. Pull VkQueue handles into the
    // per-family slots and cache the device for ownership-transfer helpers.
    void populate(vk::Device device);

    // True only when at least one queue was actually created (queues_created
    // > 0) on a family matching the predicate, not just exposed by the driver.
    bool has_graphics_queue() const;
    bool has_dedicated_compute_queue() const;
    bool has_dedicated_transfer_queue() const;
    bool has_video_decode_queue() const;
    bool has_video_encode_queue() const;
    bool has_video_decode_codec(vk::VideoCodecOperationFlagBitsKHR op) const;
    bool has_video_encode_codec(vk::VideoCodecOperationFlagBitsKHR op) const;

    std::vector<queue_family_info> queue_families() const;

    // Returns nullptr if no slot matches the request.
    std::shared_ptr<vulkan_queue> acquire_queue(const queue_request& req);

    // Internal use by vulkan_queue's ownership-transfer helpers; exposed via
    // friendship to keep semaphore creation in one place.
    vk::Device device() const { return device_; }

    // Holds a binary semaphore alive until both the source's release submit
    // and the destination's acquire submit have retired (each side keeps its
    // own shared_ptr in queue_slot::pending guarded by its slot fence).
    struct pending_release
    {
        std::shared_ptr<void> semaphore_owner;
        vk::Fence             fence;
    };

    struct queue_slot
    {
        uint32_t                    family_index = 0;
        uint32_t                    queue_index  = 0;
        vk::Queue                   queue;
        std::mutex                  mutex;
        std::atomic<uint32_t>       ref_count{0};
        std::deque<pending_release> pending; // protected by `mutex`
    };

  private:
    friend class vulkan_queue;

    struct queue_family_state
    {
        uint32_t                                 family_index = 0;
        vk::QueueFlags                           queue_flags{};
        vk::VideoCodecOperationFlagsKHR          video_codec_ops{};
        uint32_t                                 queue_count          = 0;
        uint32_t                                 timestamp_valid_bits = 0;
        std::array<uint32_t, 3>                  min_image_transfer_granularity{0, 0, 0};
        std::vector<std::unique_ptr<queue_slot>> slots;
    };

    std::vector<queue_family_state> families_;
    std::vector<uint32_t>           queues_to_create_;
    int                             gfx_family_idx_      = -1;
    bool                            video_queue_enabled_ = false;
    mutable std::mutex              registry_mutex_;
    vk::Device                      device_;
};

// Concrete VkQueue handle. Mirrors vk::Queue::submit, internally serializes
// against any other vulkan_queue sharing the same slot, and tracks how many
// consumers are referencing the underlying VkQueue. Ownership-transfer
// helpers orchestrate the release/acquire dance between queues.
class vulkan_queue final
{
  public:
    // Mirrors vk::Queue::submit. Internally takes the slot mutex.
    void submit(vk::ArrayProxy<const vk::SubmitInfo> const& submits, vk::Fence fence = {});
    void submit2(vk::ArrayProxy<const vk::SubmitInfo2> const& submits, vk::Fence fence = {});

    // For consumers that call vkQueueSubmit themselves (FFmpeg's lock_queue /
    // unlock_queue callbacks, vkQueueWaitIdle, …). Same mutex as submit().
    void                                       lock();
    void                                       unlock();
    bool                                       try_lock();
    [[nodiscard]] std::unique_lock<std::mutex> scoped_lock();

    vk::Queue                       vk_queue() const;
    uint32_t                        family_index() const;
    uint32_t                        queue_index() const;
    vk::QueueFlags                  queue_flags() const;
    vk::VideoCodecOperationFlagsKHR video_codec_ops() const;

    // Number of outstanding handles for the underlying slot. is_shared() is
    // the user_count() > 1 form.
    uint32_t user_count() const;
    bool     is_shared() const;

    // Records a queue-family release barrier into `cmd`, submits cmd on this
    // queue with a binary semaphore signal, and returns the token for the
    // destination's acquire_image_from / acquire_buffer_from.
    //
    // If this->family_index() == dst.family_index(), the queue-family transfer
    // is skipped — the helper records a plain layout transition (or no-op for
    // buffers) but still signals the semaphore so the destination can wait.
    //
    // The caller is expected to have begun cmd already and to NOT have ended
    // it; release_*_to ends and submits cmd.
    // queue_ownership_transfer release_image_to(vulkan_queue&             dst,
    //                                           vk::CommandBuffer         cmd,
    //                                           vk::Image                 image,
    //                                           vk::ImageSubresourceRange range,
    //                                           vk::ImageLayout           src_layout,
    //                                           vk::ImageLayout           dst_layout,
    //                                           vk::PipelineStageFlags2   src_stage,
    //                                           vk::AccessFlags2          src_access,
    //                                           vk::Fence                 fence = {});

    // // Records the matching acquire barrier into `cmd`, submits cmd on this
    // // queue with a wait on token.semaphore. The submitted batch takes
    // // ownership of token.semaphore_owner; the semaphore is destroyed when this
    // // queue's submit retires (best effort: walked on every later submit, and
    // // forced at queue destruction).
    // void acquire_image_from(queue_ownership_transfer& token,
    //                         vk::CommandBuffer         cmd,
    //                         vk::Image                 image,
    //                         vk::ImageSubresourceRange range,
    //                         vk::ImageLayout           src_layout,
    //                         vk::ImageLayout           dst_layout,
    //                         vk::PipelineStageFlags2   dst_stage,
    //                         vk::AccessFlags2          dst_access,
    //                         vk::Fence                 fence = {});

    // queue_ownership_transfer release_buffer_to(vulkan_queue&           dst,
    //                                            vk::CommandBuffer       cmd,
    //                                            vk::Buffer              buffer,
    //                                            vk::DeviceSize          offset,
    //                                            vk::DeviceSize          size,
    //                                            vk::PipelineStageFlags2 src_stage,
    //                                            vk::AccessFlags2        src_access,
    //                                            vk::Fence               fence = {});

    // void acquire_buffer_from(queue_ownership_transfer& token,
    //                          vk::CommandBuffer         cmd,
    //                          vk::Buffer                buffer,
    //                          vk::DeviceSize            offset,
    //                          vk::DeviceSize            size,
    //                          vk::PipelineStageFlags2   dst_stage,
    //                          vk::AccessFlags2          dst_access,
    //                          vk::Fence                 fence = {});

  private:
    friend class queue_manager;
    vulkan_queue(queue_manager::queue_slot*      slot,
                 vk::Device                      device,
                 vk::QueueFlags                  flags,
                 vk::VideoCodecOperationFlagsKHR video_ops);

    vulkan_queue(const vulkan_queue&)            = delete;
    vulkan_queue& operator=(const vulkan_queue&) = delete;

    queue_manager::queue_slot*      slot_;
    vk::Device                      device_;
    vk::QueueFlags                  queue_flags_;
    vk::VideoCodecOperationFlagsKHR video_codec_ops_;
};

}}} // namespace caspar::accelerator::vulkan
