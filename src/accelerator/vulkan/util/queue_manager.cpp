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

#include "queue_manager.h"

#include <common/assert.h>
#include <common/except.h>

#include <algorithm>

namespace caspar { namespace accelerator { namespace vulkan {

// Walks slot.pending and drops every entry whose fence is signaled. Caller
// must hold slot.mutex.
void flush_pending_locked(queue_manager::queue_slot& slot, vk::Device device)
{
    auto& p  = slot.pending;
    auto  it = p.begin();
    while (it != p.end()) {
        if (it->fence && device.getFenceStatus(it->fence) == vk::Result::eSuccess) {
            it = p.erase(it);
        } else {
            ++it;
        }
    }
}

// =============================================================================
// queue_manager
// =============================================================================

queue_manager::queue_manager(vk::PhysicalDevice phys_dev, bool video_queue_enabled)
    : video_queue_enabled_(video_queue_enabled)
{
    uint32_t count = 0;
    phys_dev.getQueueFamilyProperties2(&count, nullptr);

    std::vector<vk::QueueFamilyProperties2>        qf(count);
    std::vector<vk::QueueFamilyVideoPropertiesKHR> qf_video(count);

    if (video_queue_enabled_) {
        for (uint32_t i = 0; i < count; ++i) {
            qf[i].pNext = &qf_video[i];
        }
    }

    phys_dev.getQueueFamilyProperties2(&count, qf.data());

    for (uint32_t i = 0; i < qf.size(); ++i) {
        auto const& props       = qf[i].queueFamilyProperties;
        auto const& video_props = qf_video[i];

        queue_family_state f{};
        f.family_index         = i;
        f.queue_flags          = props.queueFlags;
        f.video_codec_ops      = vk::Flags<vk::VideoCodecOperationFlagBitsKHR>(video_props.videoCodecOperations);
        f.queue_count          = props.queueCount;
        f.timestamp_valid_bits = props.timestampValidBits;
        f.min_image_transfer_granularity = {props.minImageTransferGranularity.width,
                                            props.minImageTransferGranularity.height,
                                            props.minImageTransferGranularity.depth};
        families_.push_back(std::move(f));
    }
}

std::vector<planned_queue_set> queue_manager::plan()
{
    queues_to_create_.assign(families_.size(), 0);

    auto request_from = [&](uint32_t family_index, uint32_t desired) {
        const auto& f       = families_[family_index];
        uint32_t    already = queues_to_create_[family_index];
        uint32_t    room    = (f.queue_count > already) ? (f.queue_count - already) : 0u;
        uint32_t    add     = std::min(desired, room);
        if (add == 0)
            return;
        queues_to_create_[family_index] += add;
    };

    // Graphics: request two queues from the first graphics family so two
    // simultaneous graphics consumers (e.g. renderer + a future shader-driven
    // uploader) can each be exclusive. Falls back to 1 when only one is
    // exposed; in that case acquires share the slot via the slot mutex.
    gfx_family_idx_ = -1;
    for (auto& f : families_) {
        if (f.queue_flags & vk::QueueFlagBits::eGraphics) {
            gfx_family_idx_ = static_cast<int>(f.family_index);
            break;
        }
    }
    if (gfx_family_idx_ < 0) {
        CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("No graphics queue family on selected Vulkan device"));
    }
    request_from(static_cast<uint32_t>(gfx_family_idx_), 2);

    auto first_matching = [&](vk::QueueFlags required, vk::QueueFlags forbidden) -> int {
        for (auto& f : families_) {
            if (static_cast<int>(f.family_index) == gfx_family_idx_)
                continue;
            if ((f.queue_flags & required) != required)
                continue;
            if ((f.queue_flags & forbidden))
                continue;
            return static_cast<int>(f.family_index);
        }
        return -1;
    };

    // Dedicated transfer-only DMA family (e.g. NVIDIA copy engine).
    if (int idx = first_matching(vk::QueueFlagBits::eTransfer,
                                 vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
                                     vk::QueueFlagBits::eVideoDecodeKHR | vk::QueueFlagBits::eVideoEncodeKHR);
        idx >= 0) {
        request_from(static_cast<uint32_t>(idx), 1);
    }
    // Video decode/encode — only when VK_KHR_video_queue is enabled, since
    // creating queues from video families without the extension yields
    // queues that can't actually run video commands.
    if (video_queue_enabled_) {
        if (int idx = first_matching(vk::QueueFlagBits::eVideoDecodeKHR, {}); idx >= 0) {
            request_from(static_cast<uint32_t>(idx), 1);
        }
        if (int idx = first_matching(vk::QueueFlagBits::eVideoEncodeKHR, {}); idx >= 0) {
            request_from(static_cast<uint32_t>(idx), 1);
        }
    }
    // Dedicated compute (no graphics).
    if (int idx = first_matching(vk::QueueFlagBits::eCompute, vk::QueueFlagBits::eGraphics); idx >= 0) {
        request_from(static_cast<uint32_t>(idx), 1);
    }

    std::vector<planned_queue_set> out;
    out.reserve(families_.size());
    for (uint32_t i = 0; i < families_.size(); ++i) {
        if (queues_to_create_[i] == 0)
            continue;
        planned_queue_set p;
        p.family_index = i;
        p.priorities.assign(queues_to_create_[i], 1.0f);
        out.push_back(std::move(p));
    }
    return out;
}

void queue_manager::populate(vk::Device device)
{
    CASPAR_VERIFY(!queues_to_create_.empty());
    device_ = device;

    for (uint32_t fi = 0; fi < families_.size(); ++fi) {
        const uint32_t n = queues_to_create_[fi];
        families_[fi].slots.reserve(n);
        for (uint32_t qi = 0; qi < n; ++qi) {
            auto s          = std::make_unique<queue_slot>();
            s->family_index = fi;
            s->queue_index  = qi;
            s->queue        = device.getQueue(fi, qi);
            families_[fi].slots.push_back(std::move(s));
        }
    }
}

bool queue_manager::has_graphics_queue() const
{
    for (auto& f : families_)
        if (!f.slots.empty() && (f.queue_flags & vk::QueueFlagBits::eGraphics))
            return true;
    return false;
}

bool queue_manager::has_dedicated_compute_queue() const
{
    for (auto& f : families_)
        if (!f.slots.empty() && (f.queue_flags & vk::QueueFlagBits::eCompute) &&
            !(f.queue_flags & vk::QueueFlagBits::eGraphics))
            return true;
    return false;
}

bool queue_manager::has_dedicated_transfer_queue() const
{
    for (auto& f : families_)
        if (!f.slots.empty() && (f.queue_flags & vk::QueueFlagBits::eTransfer) &&
            !(f.queue_flags & (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute)))
            return true;
    return false;
}

bool queue_manager::has_video_decode_queue() const
{
    for (auto& f : families_)
        if (!f.slots.empty() && (f.queue_flags & vk::QueueFlagBits::eVideoDecodeKHR))
            return true;
    return false;
}

bool queue_manager::has_video_encode_queue() const
{
    for (auto& f : families_)
        if (!f.slots.empty() && (f.queue_flags & vk::QueueFlagBits::eVideoEncodeKHR))
            return true;
    return false;
}

bool queue_manager::has_video_decode_codec(vk::VideoCodecOperationFlagBitsKHR op) const
{
    for (auto& f : families_)
        if (!f.slots.empty() && (f.queue_flags & vk::QueueFlagBits::eVideoDecodeKHR) && (f.video_codec_ops & op))
            return true;
    return false;
}

bool queue_manager::has_video_encode_codec(vk::VideoCodecOperationFlagBitsKHR op) const
{
    for (auto& f : families_)
        if (!f.slots.empty() && (f.queue_flags & vk::QueueFlagBits::eVideoEncodeKHR) && (f.video_codec_ops & op))
            return true;
    return false;
}

std::vector<queue_family_info> queue_manager::queue_families() const
{
    std::vector<queue_family_info> out;
    out.reserve(families_.size());
    for (auto& f : families_) {
        queue_family_info info{};
        info.family_index                   = f.family_index;
        info.queue_flags                    = f.queue_flags;
        info.video_codec_ops                = f.video_codec_ops;
        info.queue_count                    = f.queue_count;
        info.queues_created                 = static_cast<uint32_t>(f.slots.size());
        info.timestamp_valid_bits           = f.timestamp_valid_bits;
        info.min_image_transfer_granularity = f.min_image_transfer_granularity;
        uint32_t free                       = 0;
        for (auto& s : f.slots)
            if (s->ref_count.load(std::memory_order_acquire) == 0)
                ++free;
        info.queues_free = free;
        out.push_back(info);
    }
    return out;
}

std::shared_ptr<vulkan_queue> queue_manager::acquire_queue(const queue_request& req)
{
    std::lock_guard<std::mutex> lock(registry_mutex_);

    std::vector<queue_family_state*> candidates;
    candidates.reserve(families_.size());
    for (auto& f : families_) {
        if (f.slots.empty())
            continue;
        if ((f.queue_flags & req.required_flags) != req.required_flags)
            continue;
        if ((f.queue_flags & req.forbidden_flags))
            continue;
        if (req.required_video_codec_ops &&
            (f.video_codec_ops & req.required_video_codec_ops) != req.required_video_codec_ops)
            continue;
        candidates.push_back(&f);
    }
    if (candidates.empty())
        return nullptr;

    if (req.prefer_dedicated) {
        auto is_dedicated = [&](queue_family_state* f) { return !(f->queue_flags & ~req.required_flags); };
        std::stable_sort(candidates.begin(), candidates.end(), [&](queue_family_state* a, queue_family_state* b) {
            return is_dedicated(a) && !is_dedicated(b);
        });
    }

    queue_slot* free_slot   = nullptr;
    queue_slot* shared_slot = nullptr;
    for (auto* fam : candidates) {
        for (auto& s : fam->slots) {
            if (s->ref_count.load(std::memory_order_acquire) == 0) {
                free_slot = s.get();
                break;
            }
            if (!shared_slot)
                shared_slot = s.get();
        }
        if (free_slot)
            break;
    }

    queue_slot* pick =
        req.prefer_exclusive ? (free_slot ? free_slot : shared_slot) : (shared_slot ? shared_slot : free_slot);
    if (!pick)
        return nullptr;
    pick->ref_count.fetch_add(1, std::memory_order_acq_rel);

    const auto& fam = families_[pick->family_index];
    auto*       q   = new vulkan_queue(pick, device_, fam.queue_flags, fam.video_codec_ops);

    auto self = shared_from_this();
    return std::shared_ptr<vulkan_queue>(q, [self, slot = pick](vulkan_queue* p) {
        slot->ref_count.fetch_sub(1, std::memory_order_acq_rel);
        delete p;
    });
}

// =============================================================================
// vulkan_queue
// =============================================================================

vulkan_queue::vulkan_queue(queue_manager::queue_slot*      slot,
                           vk::Device                      device,
                           vk::QueueFlags                  flags,
                           vk::VideoCodecOperationFlagsKHR video_ops)
    : slot_(slot)
    , device_(device)
    , queue_flags_(flags)
    , video_codec_ops_(video_ops)
{
}

void vulkan_queue::submit(vk::ArrayProxy<const vk::SubmitInfo> const& submits, vk::Fence fence)
{
    std::lock_guard<std::mutex> g(slot_->mutex);
    flush_pending_locked(*slot_, device_);
    slot_->queue.submit(submits, fence);
}

void vulkan_queue::submit2(vk::ArrayProxy<const vk::SubmitInfo2> const& submits, vk::Fence fence)
{
    std::lock_guard<std::mutex> g(slot_->mutex);
    flush_pending_locked(*slot_, device_);
    slot_->queue.submit2(submits, fence);
}

void vulkan_queue::lock() { slot_->mutex.lock(); }
void vulkan_queue::unlock() { slot_->mutex.unlock(); }
bool vulkan_queue::try_lock() { return slot_->mutex.try_lock(); }

std::unique_lock<std::mutex> vulkan_queue::scoped_lock() { return std::unique_lock<std::mutex>(slot_->mutex); }

vk::Queue                       vulkan_queue::vk_queue() const { return slot_->queue; }
uint32_t                        vulkan_queue::family_index() const { return slot_->family_index; }
uint32_t                        vulkan_queue::queue_index() const { return slot_->queue_index; }
vk::QueueFlags                  vulkan_queue::queue_flags() const { return queue_flags_; }
vk::VideoCodecOperationFlagsKHR vulkan_queue::video_codec_ops() const { return video_codec_ops_; }

uint32_t vulkan_queue::user_count() const { return slot_->ref_count.load(std::memory_order_acquire); }
bool     vulkan_queue::is_shared() const { return user_count() > 1; }

}}} // namespace caspar::accelerator::vulkan
