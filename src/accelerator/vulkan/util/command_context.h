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

#include <vulkan/vulkan.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#ifdef _DEBUG
#include <thread>
#endif

namespace caspar { namespace accelerator { namespace vulkan {

class vulkan_queue;

// Thread-affine recording/submission engine: a command pool (on the bound
// queue's family), a timeline semaphore, and a deque of recycled one-time
// command buffers. This is device::submitSingleTimeCommands generalized so any
// producer/consumer can record and submit work on a vulkan_queue without
// re-implementing the pool/timeline/recycling machinery.
//
// NOT internally synchronized — VkCommandPool requires external
// synchronization, so own one command_context per recording thread. The bound
// vulkan_queue may be shared across threads; its slot mutex serializes the
// underlying VkQueue, so multiple command_contexts on different threads can
// safely submit to the same queue.
class command_context final
{
  public:
    // Allocates the pool on queue->family_index() and creates the timeline. The
    // context keeps the queue alive for its lifetime.
    command_context(vk::Device device, std::shared_ptr<vulkan_queue> queue);

    // Destroys the pool (freeing all command buffers) and the timeline. The
    // caller MUST ensure the device is idle (or the submitted work has retired)
    // first — the dtor does not waitIdle.
    ~command_context();

    command_context(const command_context&)            = delete;
    command_context& operator=(const command_context&) = delete;

    // Reuse-or-allocate a one-time command buffer, begin/record(fn)/end, and
    // submit it on the bound queue signalling this context's timeline. Returns
    // the token for the signalled value.
    completion_token record_and_submit(const std::function<void(vk::CommandBuffer)>& record);

    // As above, plus extra semaphore waits/signals and an external fence — for
    // queue-ownership transfer and swapchain integration. The context's own
    // timeline signal is appended to `signals` automatically.
    completion_token record_and_submit(const std::function<void(vk::CommandBuffer)>& record,
                                       vk::ArrayProxy<const vk::SemaphoreSubmitInfo> waits,
                                       vk::ArrayProxy<const vk::SemaphoreSubmitInfo> signals,
                                       vk::Fence                                     fence);

    // As record_and_submit(record), but first waits on the given completion
    // tokens. Tokens on THIS context's timeline are dropped (same-queue ordering
    // is covered by submission order + barriers); cross-queue tokens become
    // timeline waits. Used by the per-texture dependency tracking.
    completion_token record_and_submit(const std::function<void(vk::CommandBuffer)>& record,
                                       vk::ArrayProxy<const completion_token>        wait_tokens,
                                       vk::Fence                                     fence = {});

    // Submit an already-recorded (begun + ended) command buffer on the bound
    // queue, waiting on the cross-queue tokens, signalling this context's timeline
    // plus the external fence. Unlike record_and_submit the context does NOT own
    // or recycle `cmd` — the caller manages its lifetime (e.g. image_kernel's own
    // fence-tracked ring). Returns the token for the signalled value.
    completion_token
    submit_recorded(vk::CommandBuffer cmd, vk::ArrayProxy<const completion_token> wait_tokens, vk::Fence fence = {});

    // Non-blocking completion check; true for a default/empty token.
    bool poll(const completion_token& token) const;
    // Block until the token's value is reached (or timeout). Returns false on timeout.
    bool wait(const completion_token& token, uint64_t timeout_ns = 1'000'000'000) const;

    // Keep `owner` alive until `token` completes (drained lazily on subsequent
    // record_and_submit calls, and at destruction). Used by the ownership-transfer
    // helpers to hold a binary semaphore until the waiting submit has retired.
    void retain_until(std::shared_ptr<void> owner, const completion_token& token);

    vk::CommandPool pool() const { return pool_; }
    vulkan_queue&   queue() const { return *queue_; }
    vk::Device      device() const { return device_; }

  private:
    struct inflight_command_buffer
    {
        vk::CommandBuffer cmd;
        uint64_t          value;
    };

    // Reuse the oldest retired command buffer, or allocate a fresh one.
    vk::CommandBuffer acquire_command_buffer();

    // Convert tokens into timeline wait infos, dropping empties and any token on
    // this context's own timeline, and collapsing multiple tokens that share a
    // timeline to their highest value.
    std::vector<vk::SemaphoreSubmitInfo> to_wait_infos(vk::ArrayProxy<const completion_token> tokens) const;

    // Drop every retained owner whose token has completed.
    void drain_retained();

    vk::Device                          device_;
    std::shared_ptr<vulkan_queue>       queue_;
    vk::CommandPool                     pool_;
    vk::Semaphore                       timeline_;
    uint64_t                            value_ = 0;
    std::deque<inflight_command_buffer> inflight_;

    std::deque<std::pair<std::shared_ptr<void>, completion_token>> retained_;

#ifdef _DEBUG
    // Bound to the first thread that records; subsequent records must match.
    std::thread::id owner_thread_{};
    void            verify_thread();
#endif
};

}}} // namespace caspar::accelerator::vulkan
