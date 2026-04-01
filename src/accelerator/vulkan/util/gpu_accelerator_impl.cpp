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

        auto tex = texture::wrap_external(vk_device,
                                          static_cast<vk::Image>(static_cast<VkImage>(plane.vk_image)),
                                          static_cast<int>(plane.width),
                                          static_cast<int>(plane.height),
                                          stride,
                                          static_cast<vk::Format>(plane.vk_format),
                                          depth);

        // Create an already-resolved shared_future
        std::promise<std::shared_ptr<texture>> promise;
        promise.set_value(std::move(tex));
        textures->emplace_back(promise.get_future().share());
    }

    // Capture textures and lifetime_token in the commit callback.
    // The lifetime_token prevents the source GPU surfaces from being
    // recycled by the producer's decode framework until the mixer is done.
    auto commit = [textures, lifetime_token = std::move(lifetime_token)](
                      std::vector<array<const std::uint8_t>>) -> std::any { return textures; };

    return core::mutable_frame(tag, std::vector<array<std::uint8_t>>{}, std::move(audio_data), desc, std::move(commit));
}

}}} // namespace caspar::accelerator::vulkan
