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

#include <core/frame/gpu_accelerator.h>

#include <memory>

namespace caspar { namespace accelerator { namespace vulkan {

class device;

class gpu_accelerator_impl final : public core::gpu_accelerator
{
  public:
    explicit gpu_accelerator_impl(std::shared_ptr<device> vulkan);
    ~gpu_accelerator_impl() override;

    void*    vk_instance() const override;
    void*    vk_physical_device() const override;
    void*    vk_device() const override;
    uint32_t queue_family_index() const override;
    void*    vk_queue() const override;
    void*    vk_get_instance_proc_addr() const override;
    const std::vector<std::string>& vk_enabled_device_extensions() const override;
    int    vk_decode_queue_family_index() const override;

    void vk_lock_queue() override;
    void vk_unlock_queue() override;

    core::mutable_frame import_gpu_images(const void*                              tag,
                                          const std::vector<core::gpu_image_desc>& planes,
                                          const core::pixel_format_desc&           desc,
                                          array<std::int32_t>                      audio_data,
                                          std::shared_ptr<void>                    lifetime_token) override;

  private:
    std::shared_ptr<device> vulkan_;
};

}}} // namespace caspar::accelerator::vulkan
