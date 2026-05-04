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

#include "queue_transfer.h"

#include "command_context.h"
#include "queue_manager.h"
#include "texture.h"

#include <common/assert.h>

#include <memory>

namespace caspar { namespace accelerator { namespace vulkan {

namespace {

// RAII owner of the transfer's binary semaphore. Held (via shared_ptr<void>) by
// both the release side and the acquire side until their respective submits
// retire; destroyed — destroying the semaphore — when the last reference drops.
struct semaphore_holder
{
    vk::Device    device;
    vk::Semaphore semaphore;

    semaphore_holder(vk::Device d, vk::Semaphore s)
        : device(d)
        , semaphore(s)
    {
    }

    ~semaphore_holder()
    {
        if (semaphore)
            device.destroySemaphore(semaphore);
    }

    semaphore_holder(const semaphore_holder&)            = delete;
    semaphore_holder& operator=(const semaphore_holder&) = delete;
};

} // namespace

queue_ownership_transfer release_texture(command_context&        src_cc,
                                         vulkan_queue&           dst,
                                         texture&                tex,
                                         vk::ImageLayout         dst_layout,
                                         vk::PipelineStageFlags2 src_stage,
                                         vk::AccessFlags2        src_access)
{
    const auto device      = src_cc.device();
    const auto src_family  = src_cc.queue().family_index();
    const auto dst_family  = dst.family_index();
    const bool same_family = (src_family == dst_family);

    const auto src_layout = tex.current_layout();
    const auto image      = tex.image();
    const auto range      = tex.subresource_range();

    auto sem    = device.createSemaphore({});
    auto holder = std::make_shared<semaphore_holder>(device, sem);

    vk::SemaphoreSubmitInfo signal{};
    signal.semaphore = sem;
    signal.stageMask = vk::PipelineStageFlagBits2::eAllCommands;

    auto completion = src_cc.record_and_submit(
        [&](vk::CommandBuffer cmd) {
            vk::ImageMemoryBarrier2 b{};
            b.srcStageMask        = src_stage;
            b.srcAccessMask       = src_access;
            b.dstStageMask        = vk::PipelineStageFlagBits2::eNone;
            b.dstAccessMask       = vk::AccessFlagBits2::eNone;
            b.oldLayout           = src_layout;
            b.newLayout           = dst_layout;
            b.srcQueueFamilyIndex = same_family ? vk::QueueFamilyIgnored : src_family;
            b.dstQueueFamilyIndex = same_family ? vk::QueueFamilyIgnored : dst_family;
            b.image               = image;
            b.subresourceRange    = range;

            vk::DependencyInfo dep;
            dep.setImageMemoryBarriers(b);
            cmd.pipelineBarrier2(dep);
        },
        {},
        signal,
        {});

    // Keep the semaphore alive at least until the release submit retires, even
    // if no acquire follows (dropped frame).
    src_cc.retain_until(holder, completion);

    // Reflect the intended post-transfer state on the texture.
    tex.set_current_layout(dst_layout);
    tex.set_owner_family(dst_family);

    queue_ownership_transfer token;
    token.semaphore        = sem;
    token.src_family_index = src_family;
    token.dst_family_index = dst_family;
    token.src_layout       = src_layout;
    token.dst_layout       = dst_layout;
    token.dst_wait_stage   = vk::PipelineStageFlagBits2::eAllCommands;
    token.semaphore_owner  = holder;
    return token;
}

void acquire_texture(command_context&          dst_cc,
                     queue_ownership_transfer& token,
                     texture&                  tex,
                     vk::PipelineStageFlags2   dst_stage,
                     vk::AccessFlags2          dst_access)
{
    CASPAR_VERIFY(token.semaphore);

    const bool same_family = (token.src_family_index == token.dst_family_index);
    const auto image       = tex.image();
    const auto range       = tex.subresource_range();

    vk::SemaphoreSubmitInfo wait{};
    wait.semaphore = token.semaphore;
    wait.stageMask = token.dst_wait_stage;

    auto completion = dst_cc.record_and_submit(
        [&](vk::CommandBuffer cmd) {
            vk::ImageMemoryBarrier2 b{};
            b.srcStageMask        = vk::PipelineStageFlagBits2::eNone;
            b.srcAccessMask       = vk::AccessFlagBits2::eNone;
            b.dstStageMask        = dst_stage;
            b.dstAccessMask       = dst_access;
            b.oldLayout           = token.src_layout;
            b.newLayout           = token.dst_layout;
            b.srcQueueFamilyIndex = same_family ? vk::QueueFamilyIgnored : token.src_family_index;
            b.dstQueueFamilyIndex = same_family ? vk::QueueFamilyIgnored : token.dst_family_index;
            b.image               = image;
            b.subresourceRange    = range;

            vk::DependencyInfo dep;
            dep.setImageMemoryBarriers(b);
            cmd.pipelineBarrier2(dep);
        },
        wait,
        {},
        {});

    // Hold the semaphore until this acquire submit retires.
    dst_cc.retain_until(token.semaphore_owner, completion);

    token.semaphore = vk::Semaphore{};
    token.semaphore_owner.reset();
}

}}} // namespace caspar::accelerator::vulkan
