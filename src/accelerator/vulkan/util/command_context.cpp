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

#include "command_context.h"

#include "queue_manager.h"

#include <common/assert.h>

#include <algorithm>
#include <vector>

namespace caspar { namespace accelerator { namespace vulkan {

command_context::command_context(vk::Device device, std::shared_ptr<vulkan_queue> queue)
    : device_(device)
    , queue_(std::move(queue))
{
    CASPAR_VERIFY(queue_);

    vk::CommandPoolCreateInfo pool_info{};
    pool_info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    pool_info.queueFamilyIndex = queue_->family_index();
    pool_                      = device_.createCommandPool(pool_info);

    vk::SemaphoreTypeCreateInfo timeline_info{};
    timeline_info.semaphoreType = vk::SemaphoreType::eTimeline;
    timeline_info.initialValue  = 0;
    vk::SemaphoreCreateInfo semaphore_info{};
    semaphore_info.pNext = &timeline_info;
    timeline_            = device_.createSemaphore(semaphore_info);
}

command_context::~command_context()
{
    // The caller is responsible for ensuring the submitted work has retired
    // (device.waitIdle) before destroying the context — we just release the
    // pool (which frees its command buffers) and the timeline. Retained owners
    // (binary semaphores) are dropped here while the device is still valid.
    retained_.clear();
    inflight_.clear();
    if (pool_)
        device_.destroyCommandPool(pool_);
    if (timeline_)
        device_.destroySemaphore(timeline_);
}

#ifdef _DEBUG
void command_context::verify_thread()
{
    if (owner_thread_ == std::thread::id{})
        owner_thread_ = std::this_thread::get_id();
    else
        CASPAR_VERIFY(owner_thread_ == std::this_thread::get_id());
}
#endif

void command_context::drain_retained()
{
    auto it = retained_.begin();
    while (it != retained_.end()) {
        if (poll(it->second)) {
            it = retained_.erase(it);
        } else {
            ++it;
        }
    }
}

void command_context::retain_until(std::shared_ptr<void> owner, const completion_token& token)
{
#ifdef _DEBUG
    verify_thread();
#endif
    if (owner)
        retained_.emplace_back(std::move(owner), token);
}

vk::CommandBuffer command_context::acquire_command_buffer()
{
    vk::CommandBuffer cmd = nullptr;

    if (inflight_.size() > 1) {
        auto completed = device_.getSemaphoreCounterValue(timeline_);

        // Reuse the oldest command buffer once its submit has retired.
        if (inflight_.front().value <= completed) {
            cmd = inflight_.front().cmd;
            cmd.reset();
            inflight_.pop_front();
        }
    }

    if (!cmd) {
        vk::CommandBufferAllocateInfo allocInfo{};
        allocInfo.commandPool        = pool_;
        allocInfo.level              = vk::CommandBufferLevel::ePrimary;
        allocInfo.commandBufferCount = 1;

        cmd = device_.allocateCommandBuffers(allocInfo)[0];
    }

    return cmd;
}

completion_token command_context::record_and_submit(const std::function<void(vk::CommandBuffer)>& record)
{
    return record_and_submit(record, {}, {}, {});
}

completion_token command_context::record_and_submit(const std::function<void(vk::CommandBuffer)>& record,
                                                    vk::ArrayProxy<const vk::SemaphoreSubmitInfo> waits,
                                                    vk::ArrayProxy<const vk::SemaphoreSubmitInfo> signals,
                                                    vk::Fence                                     fence)
{
#ifdef _DEBUG
    verify_thread();
#endif

    drain_retained();

    auto cmd = acquire_command_buffer();

    cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    record(cmd);
    cmd.end();

    auto signal_value = ++value_;

    // Append the context's timeline signal to whatever the caller asked for.
    std::vector<vk::SemaphoreSubmitInfo> signal_infos(signals.begin(), signals.end());
    vk::SemaphoreSubmitInfo              timeline_signal{};
    timeline_signal.semaphore = timeline_;
    timeline_signal.value     = signal_value;
    timeline_signal.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
    signal_infos.push_back(timeline_signal);

    std::vector<vk::SemaphoreSubmitInfo> wait_infos(waits.begin(), waits.end());

    vk::CommandBufferSubmitInfo cmd_info{};
    cmd_info.commandBuffer = cmd;

    vk::SubmitInfo2 submit{};
    submit.setCommandBufferInfos(cmd_info);
    submit.setWaitSemaphoreInfos(wait_infos);
    submit.setSignalSemaphoreInfos(signal_infos);

    queue_->submit2(submit, fence);

    inflight_.push_back({cmd, signal_value});

    return {timeline_, signal_value};
}

std::vector<vk::SemaphoreSubmitInfo> command_context::to_wait_infos(vk::ArrayProxy<const completion_token> tokens) const
{
    // Collapse tokens sharing a timeline to their highest value; drop empties and
    // anything on our own timeline (same-queue ordering needs no semaphore).
    std::vector<vk::SemaphoreSubmitInfo> out;
    for (const auto& t : tokens) {
        if (!t || t.timeline == timeline_)
            continue;

        bool merged = false;
        for (auto& w : out) {
            if (w.semaphore == t.timeline) {
                w.value = std::max<uint64_t>(w.value, t.value);
                merged  = true;
                break;
            }
        }
        if (!merged) {
            vk::SemaphoreSubmitInfo w{};
            w.semaphore = t.timeline;
            w.value     = t.value;
            w.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
            out.push_back(w);
        }
    }
    return out;
}

completion_token command_context::record_and_submit(const std::function<void(vk::CommandBuffer)>& record,
                                                    vk::ArrayProxy<const completion_token>        wait_tokens,
                                                    vk::Fence                                     fence)
{
    return record_and_submit(record, to_wait_infos(wait_tokens), {}, fence);
}

completion_token command_context::submit_recorded(vk::CommandBuffer                      cmd,
                                                  vk::ArrayProxy<const completion_token> wait_tokens,
                                                  vk::Fence                              fence)
{
#ifdef _DEBUG
    verify_thread();
#endif

    drain_retained();

    auto signal_value = ++value_;

    auto                    waits = to_wait_infos(wait_tokens);
    vk::SemaphoreSubmitInfo signal{};
    signal.semaphore = timeline_;
    signal.value     = signal_value;
    signal.stageMask = vk::PipelineStageFlagBits2::eAllCommands;

    vk::CommandBufferSubmitInfo cmd_info{};
    cmd_info.commandBuffer = cmd;

    vk::SubmitInfo2 submit{};
    submit.setCommandBufferInfos(cmd_info);
    submit.setWaitSemaphoreInfos(waits);
    submit.setSignalSemaphoreInfos(signal);

    queue_->submit2(submit, fence);

    // NB: `cmd` is owned by the caller (not tracked in inflight_) — it manages
    // recycling via its own fence.
    return {timeline_, signal_value};
}

bool command_context::poll(const completion_token& token) const
{
    if (!token)
        return true;
    return device_.getSemaphoreCounterValue(token.timeline) >= token.value;
}

bool command_context::wait(const completion_token& token, uint64_t timeout_ns) const
{
    if (!token)
        return true;

    vk::SemaphoreWaitInfo waitInfo{};
    waitInfo.setSemaphores(token.timeline);
    waitInfo.setValues(token.value);

    return device_.waitSemaphores(waitInfo, timeout_ns) == vk::Result::eSuccess;
}

}}} // namespace caspar::accelerator::vulkan
