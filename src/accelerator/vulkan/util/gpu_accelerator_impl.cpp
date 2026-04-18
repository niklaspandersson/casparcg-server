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

#include "gpu_accelerator_impl.h"

#include "device.h"
#include "texture.h"

#include <common/array.h>

#include <core/frame/frame.h>
#include <core/frame/pixel_format.h>

#include <any>
#include <algorithm>
#include <future>
#include <vector>

namespace caspar { namespace accelerator { namespace vulkan {

using future_texture = std::shared_future<std::shared_ptr<texture>>;

gpu_accelerator_impl::gpu_accelerator_impl(std::shared_ptr<device> vulkan)
    : vulkan_(std::move(vulkan))
{
}

gpu_accelerator_impl::~gpu_accelerator_impl() = default;

void* gpu_accelerator_impl::vk_instance() const { return vulkan_->getVkInstance(); }

void* gpu_accelerator_impl::vk_physical_device() const { return vulkan_->getVkPhysicalDevice(); }

void* gpu_accelerator_impl::vk_device() const { return static_cast<VkDevice>(vulkan_->getVkDevice()); }

uint32_t gpu_accelerator_impl::queue_family_index() const { return vulkan_->getGraphicsQueueFamilyIndex(); }

void* gpu_accelerator_impl::vk_queue() const { return vulkan_->getGraphicsQueue(); }

void* gpu_accelerator_impl::vk_get_instance_proc_addr() const
{
    return reinterpret_cast<void*>(vulkan_->getInstanceProcAddr());
}

const std::vector<std::string>& gpu_accelerator_impl::vk_enabled_device_extensions() const
{
    return vulkan_->getEnabledDeviceExtensions();
}

int gpu_accelerator_impl::vk_decode_queue_family_index() const
{
    return vulkan_->getDecodeQueueFamilyIndex();
}

bool gpu_accelerator_impl::has_extension(const std::string& name) const
{
    const auto& exts = vulkan_->getEnabledDeviceExtensions();
    return std::find(exts.begin(), exts.end(), name) != exts.end();
}

bool gpu_accelerator_impl::vk_shared_render_queue() const { return vulkan_->shared_render_queue(); }
void gpu_accelerator_impl::vk_lock_queue() { vulkan_->lock_queue(); }
void gpu_accelerator_impl::vk_unlock_queue() { vulkan_->unlock_queue(); }

core::mutable_frame gpu_accelerator_impl::import_gpu_images(const void*                              tag,
                                                             const std::vector<core::gpu_image_desc>& planes,
                                                             const core::pixel_format_desc&           desc,
                                                             array<std::int32_t>                      audio_data,
                                                             std::shared_ptr<void>                    lifetime_token)
{
    // Wrap each external VkImage as a CasparCG texture
    auto textures = std::make_shared<std::vector<future_texture>>();
    textures->reserve(planes.size());

    auto vk_device = vulkan_->getVkDevice();

    for (size_t i = 0; i < planes.size(); ++i) {
        auto& plane = planes[i];

        auto stride = desc.planes.at(i).stride;
        auto depth  = desc.planes.at(i).depth;

        external_sync sync;
        sync.aspect = plane.vk_aspect != 0
                          ? static_cast<vk::ImageAspectFlags>(static_cast<VkImageAspectFlags>(plane.vk_aspect))
                          : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
        sync.current_layout   = static_cast<vk::ImageLayout>(plane.current_layout);
        sync.target_layout    = plane.target_layout != 0
                                    ? static_cast<vk::ImageLayout>(plane.target_layout)
                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
        sync.src_queue_family = plane.src_queue_family;
        sync.src_access       = static_cast<vk::AccessFlags2>(plane.src_access_mask);
        sync.src_stage        = plane.src_stage_mask != 0
                                    ? static_cast<vk::PipelineStageFlags2>(plane.src_stage_mask)
                                    : vk::PipelineStageFlags2(vk::PipelineStageFlagBits2::eTopOfPipe);
        sync.wait_semaphore   = static_cast<VkSemaphore>(plane.wait_semaphore);
        sync.wait_value       = plane.wait_value;
        sync.signal_semaphore = static_cast<VkSemaphore>(plane.signal_semaphore);
        sync.signal_value     = plane.signal_value;
        sync.writeback        = plane.writeback;

        // The lifetime token must travel with the texture so that frame_data can
        // hold it until the GPU is actually finished with the image. Releasing it
        // at commit time would let the producer recycle the VkImage too early.
        auto tex = texture::wrap_external(vk_device,
                                          static_cast<vk::Image>(static_cast<VkImage>(plane.vk_image)),
                                          static_cast<int>(plane.width),
                                          static_cast<int>(plane.height),
                                          stride,
                                          static_cast<vk::Format>(plane.vk_format),
                                          depth,
                                          std::move(sync),
                                          lifetime_token);

        // Create an already-resolved shared_future
        std::promise<std::shared_ptr<texture>> promise;
        promise.set_value(std::move(tex));
        textures->emplace_back(promise.get_future().share());
    }

    // The lifetime token is now carried on each texture (see external_lifetime()),
    // so the commit callback only needs to keep the futures alive.
    auto commit = [textures](std::vector<array<const std::uint8_t>>) -> std::any { return textures; };

    return core::mutable_frame(tag, std::vector<array<std::uint8_t>>{}, std::move(audio_data), desc, std::move(commit));
}

}}} // namespace caspar::accelerator::vulkan
