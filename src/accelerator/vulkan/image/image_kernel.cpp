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

#include "image_kernel.h"

#include "../util/device.h"
#include "../util/pipeline.h"
#include "../util/renderpass.h"
#include "../util/texture.h"

#include <common/assert.h>

#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>

#include <boost/algorithm/cxx11/all_of.hpp>
#include <boost/range/adaptor/transformed.hpp>

#include <array>
#include <cmath>

namespace caspar::accelerator::vulkan {

float get_precision_factor(common::bit_depth depth)
{
    switch (depth) {
        case common::bit_depth::bit8:
            return 1.0f;
        case common::bit_depth::bit10:
            return 64.0f;
        case common::bit_depth::bit12:
            return 16.0f;
        case common::bit_depth::bit16:
        default:
            return 1.0f;
    }
}

bool is_above_screen(double y) { return y < 0.0; }

bool is_below_screen(double y) { return y > 1.0; }

bool is_left_of_screen(double x) { return x < 0.0; }

bool is_right_of_screen(double x) { return x > 1.0; }

bool is_outside_screen(const std::vector<core::frame_geometry::coord>& coords)
{
    auto x_coords =
        coords | boost::adaptors::transformed([](const core::frame_geometry::coord& c) { return c.vertex_x; });
    auto y_coords =
        coords | boost::adaptors::transformed([](const core::frame_geometry::coord& c) { return c.vertex_y; });

    return boost::algorithm::all_of(x_coords, &is_left_of_screen) ||
           boost::algorithm::all_of(x_coords, &is_right_of_screen) ||
           boost::algorithm::all_of(y_coords, &is_above_screen) || boost::algorithm::all_of(y_coords, &is_below_screen);
}

static const double epsilon = 0.001;

static const uint32_t frame_buffer_size = 3;

struct image_kernel::impl
{
    spl::shared_ptr<device> vulkan_;
    common::bit_depth       depth_;

    struct frame_data : public frame_context
    {
        image_kernel::impl* parent = nullptr;

        vk::Buffer       buffer = nullptr;
        void*            data   = nullptr;
        vk::DeviceMemory memory = nullptr;
        size_t           size   = 0;

        vk::CommandBuffer cmd_buffer = nullptr;
        vk::Fence         fence      = nullptr;

        // Externally-imported images consumed by the current frame, and the
        // lifetime tokens that pin those images alive until the GPU is done.
        std::vector<external_use>          externals;
        std::vector<std::shared_ptr<void>> lifetime_tokens;
        // Track unique image handles so we don't issue duplicate barriers when
        // multiple planes alias the same multiplanar VkImage.
        std::vector<vk::Image> seen_external_images;

        explicit frame_data(image_kernel::impl* parent)
            : parent(parent)
        {
        }

        virtual vk::Buffer upload_vertex_data(const std::vector<float>& data)
        {
            return parent->upload_vertex_buffer(*this, (void*)data.data(), data.size() * sizeof(float));
        }
        virtual draw_data create_draw_data(const draw_params& params) { return parent->draw(params); }
        virtual std::shared_ptr<class pipeline> get_pipeline() { return parent->vulkan_->get_pipeline(parent->depth_); }
        virtual vk::CommandBuffer               get_command_buffer() { return cmd_buffer; }
        virtual void                            submit()
        {
            // Build wait/signal arrays from registered externals. Empty arrays
            // are legal and reduce to today's behavior (plain submit).
            std::vector<vk::Semaphore>          waits;
            std::vector<vk::Semaphore>          signals;
            std::vector<vk::PipelineStageFlags> wait_stages;
            std::vector<uint64_t>               wait_vals;
            std::vector<uint64_t>               signal_vals;

            for (auto& e : externals) {
                if (e.sync.wait_semaphore) {
                    waits.push_back(e.sync.wait_semaphore);
                    wait_vals.push_back(e.sync.wait_value);
                    wait_stages.push_back(vk::PipelineStageFlagBits::eFragmentShader);
                }
                if (e.sync.signal_semaphore) {
                    signals.push_back(e.sync.signal_semaphore);
                    signal_vals.push_back(e.sync.signal_value);
                }
            }

            vk::TimelineSemaphoreSubmitInfo tsi{};
            tsi.setWaitSemaphoreValues(wait_vals);
            tsi.setSignalSemaphoreValues(signal_vals);

            vk::SubmitInfo submitInfo{};
            if (!wait_vals.empty() || !signal_vals.empty()) {
                submitInfo.pNext = &tsi;
            }
            submitInfo.setCommandBuffers(cmd_buffer);
            submitInfo.setWaitSemaphores(waits);
            submitInfo.setWaitDstStageMask(wait_stages);
            submitInfo.setSignalSemaphores(signals);

            fence = parent->vulkan_->getVkDevice().createFence({});
            parent->vulkan_->submit(submitInfo, fence);

            // Synchronous writebacks: producer's next frame must see the new
            // state (e.g. AVVkFrame::sem_value bumped) as soon as we've recorded
            // the submit. The release fence inside the closure provides the
            // happens-before for any other thread reading the producer's state.
            const auto graphics_qfi = parent->vulkan_->getGraphicsQueueFamilyIndex();
            for (auto& e : externals) {
                if (e.sync.writeback) {
                    e.sync.writeback(static_cast<uint32_t>(e.sync.target_layout),
                                     graphics_qfi,
                                     e.sync.signal_value,
                                     static_cast<uint64_t>(vk::AccessFlagBits2::eShaderSampledRead));
                }
            }
        }
        virtual std::shared_ptr<class texture>
        create_attachment(uint32_t width, uint32_t height, uint32_t components_count)
        {
            return parent->vulkan_->create_attachment(width, height, parent->depth_, components_count);
        }

        virtual void register_external(const external_use& use, std::shared_ptr<void> lifetime_token)
        {
            // Skip duplicate VkImage handles (multiplanar single-image NV12 etc).
            for (auto& img : seen_external_images) {
                if (img == use.image)
                    return;
            }
            seen_external_images.push_back(use.image);
            externals.push_back(use);
            if (lifetime_token) {
                lifetime_tokens.push_back(std::move(lifetime_token));
            }
        }

        virtual const std::vector<external_use>& registered_externals() const { return externals; }
        virtual uint32_t graphics_queue_family() const { return parent->vulkan_->getGraphicsQueueFamilyIndex(); }
    };

    frame_data frames_[frame_buffer_size];
    uint32_t   current_frame_index_ = 0;

    explicit impl(const spl::shared_ptr<device>& vulkan, common::bit_depth depth)
        : vulkan_(vulkan)
        , depth_(depth)
        , frames_{frame_data{this}, frame_data{this}, frame_data{this}}
    {
        auto cmd_buffers = vulkan_->allocateCommandBuffers(frame_buffer_size);
        for (uint32_t i = 0; i < frame_buffer_size; ++i) {
            frames_[i].cmd_buffer = cmd_buffers[i];
        }
    }

    ~impl()
    {
        auto vk_device = vulkan_->getVkDevice();

        for (auto& frame : frames_) {
            if (frame.buffer) {
                vk_device.unmapMemory(frame.memory);
                vk_device.destroyBuffer(frame.buffer);
                vk_device.freeMemory(frame.memory);
                if (frame.fence) {
                    vk_device.destroyFence(frame.fence);
                }
            }
        }
    }

    spl::shared_ptr<renderpass> create_renderpass(uint32_t width, uint32_t height)
    {
        auto  device = vulkan_->getVkDevice();
        auto& ctx    = frames_[(++current_frame_index_) % frame_buffer_size];
        if (ctx.fence) {
            auto result = device.waitForFences(ctx.fence, true, 1000000000); // wait up to one second
            if (result == vk::Result::eTimeout) {
                throw std::runtime_error("[Vulkan image_kernel] Timeout waiting for fence");
            }
            device.destroyFence(ctx.fence);
            ctx.fence = nullptr;
        }

        // The fence is signaled, so the GPU is finished with anything the
        // previous occupant of this slot referenced. It is now safe to drop
        // its lifetime tokens (releasing any external producer's frame back
        // into its pool) and clear the per-frame externals list.
        ctx.lifetime_tokens.clear();
        ctx.externals.clear();
        ctx.seen_external_images.clear();

        ctx.cmd_buffer.reset({});
        return spl::make_shared<renderpass>(&ctx, width, height);
    }

    uint32_t findDedicatedMemoryType(uint32_t typeMask, vk::MemoryPropertyFlags properties)
    {
        auto memProperties = vulkan_->getMemoryProperties();
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
            if ((typeMask & (1 << i)) && ((memProperties.memoryTypes[i].propertyFlags & properties) == properties)) {
                return i;
            }
        }
        throw std::runtime_error("[Vulkan image_kernel] Failed to find suitable memory type");
    }

    vk::Buffer upload_vertex_buffer(frame_data& vb, void* data, size_t size)
    {
        if (vb.size < size) {
            auto vk_device = vulkan_->getVkDevice();

            if (vb.buffer) {
                vk_device.unmapMemory(vb.memory);
                vk_device.destroyBuffer(vb.buffer);
                vk_device.freeMemory(vb.memory);
            }

            // staging buffer
            vk::BufferCreateInfo stagingInfo{};
            stagingInfo.size        = size;
            stagingInfo.usage       = vk::BufferUsageFlagBits::eVertexBuffer;
            stagingInfo.sharingMode = vk::SharingMode::eExclusive;

            vb.buffer = vk_device.createBuffer(stagingInfo);

            auto stagingMemReq = vk_device.getBufferMemoryRequirements(vb.buffer);

            vk::MemoryAllocateInfo stagingAlloc{};
            stagingAlloc.allocationSize  = stagingMemReq.size;
            stagingAlloc.memoryTypeIndex = findDedicatedMemoryType(stagingMemReq.memoryTypeBits,
                                                                   vk::MemoryPropertyFlagBits::eHostVisible |
                                                                       vk::MemoryPropertyFlagBits::eHostCoherent);

            vb.memory = vk_device.allocateMemory(stagingAlloc);
            vk_device.bindBufferMemory(vb.buffer, vb.memory, 0);

            vb.data = vk_device.mapMemory(vb.memory, 0, size);
            vb.size = size;
        }
        memcpy(vb.data, data, size);

        return vb.buffer;
    }

    std::pair<std::vector<core::frame_geometry::coord>, uniform_block> draw(const draw_params& params)
    {
        CASPAR_ASSERT(params.pix_desc.planes.size() == params.textures.size());

        if (params.textures.empty() || !params.background) {
            return {};
        }

        if (params.transforms.image_transform.opacity < epsilon) {
            return {};
        }

        if (params.geometry.data().empty()) {
            return {};
        }

        auto coords     = params.geometry.data();
        auto transforms = params.transforms;

        auto const first_plane = params.pix_desc.planes.at(0);
        if (params.geometry.mode() != core::frame_geometry::scale_mode::stretch && first_plane.width > 0 &&
            first_plane.height > 0) {
            auto width_scale  = static_cast<double>(params.target_width) / static_cast<double>(first_plane.width);
            auto height_scale = static_cast<double>(params.target_height) / static_cast<double>(first_plane.height);

            core::image_transform transform;
            double                target_scale;
            switch (params.geometry.mode()) {
                case core::frame_geometry::scale_mode::fit:
                    target_scale = std::min(width_scale, height_scale);

                    transform.fill_scale[0] *= target_scale / width_scale;
                    transform.fill_scale[1] *= target_scale / height_scale;
                    break;

                case core::frame_geometry::scale_mode::fill:
                    target_scale = std::max(width_scale, height_scale);
                    transform.fill_scale[0] *= target_scale / width_scale;
                    transform.fill_scale[1] *= target_scale / height_scale;
                    break;

                case core::frame_geometry::scale_mode::original:
                    transform.fill_scale[0] /= width_scale;
                    transform.fill_scale[1] /= height_scale;
                    break;

                case core::frame_geometry::scale_mode::hfill:
                    transform.fill_scale[1] *= width_scale / height_scale;
                    break;

                case core::frame_geometry::scale_mode::vfill:
                    transform.fill_scale[0] *= height_scale / width_scale;
                    break;

                default:;
            }

            transforms = transforms.combine_transform(transform, params.aspect_ratio);
        }

        coords = transforms.transform_coords(coords);

        // Skip drawing if all the coordinates will be outside the screen.
        if (coords.size() < 3 || is_outside_screen(coords)) {
            return {};
        }

        uniform_block uniforms;

        for (int n = 0; n < params.textures.size(); ++n) {
            uniforms.precision_factor[n] = get_precision_factor(params.textures[n]->depth());
        }

        const auto is_hd           = params.pix_desc.planes.at(0).height > 700;
        const auto color_space     = is_hd ? params.pix_desc.color_space : core::color_space::bt601;
        uniforms.color_space_index = static_cast<uint32_t>(color_space);

        if (params.pix_desc.is_straight_alpha) {
            uniforms.flags |= static_cast<uint32_t>(shader_flags::is_straight_alpha);
        }

        if (static_cast<bool>(params.local_key)) {
            uniforms.flags |= static_cast<uint32_t>(shader_flags::has_local_key);
        }
        if (static_cast<bool>(params.layer_key)) {
            uniforms.flags |= static_cast<uint32_t>(shader_flags::has_layer_key);
        }
        uniforms.pixel_format = static_cast<uint32_t>(params.pix_desc.format);

        uniforms.opacity =
            transforms.image_transform.is_key ? 1.0f : static_cast<float>(transforms.image_transform.opacity);

        if (transforms.image_transform.chroma.enable) {
            uniforms.flags |= static_cast<uint32_t>(shader_flags::chroma);

            if (transforms.image_transform.chroma.show_mask)
                uniforms.flags |= static_cast<uint32_t>(shader_flags::chroma_show_mask);

            uniforms.chroma_target_hue     = static_cast<float>(transforms.image_transform.chroma.target_hue) / 360.0f;
            uniforms.chroma_hue_width      = static_cast<float>(transforms.image_transform.chroma.hue_width);
            uniforms.chroma_min_saturation = static_cast<float>(transforms.image_transform.chroma.min_saturation);
            uniforms.chroma_min_brightness = static_cast<float>(transforms.image_transform.chroma.min_brightness);
            uniforms.chroma_softness       = 1.0f + static_cast<float>(transforms.image_transform.chroma.softness);
            uniforms.chroma_spill_suppress =
                static_cast<float>(transforms.image_transform.chroma.spill_suppress) / 360.0f;
            uniforms.chroma_spill_suppress_saturation =
                static_cast<float>(transforms.image_transform.chroma.spill_suppress_saturation);
        }

        // Setup blend_func
        auto blend_mode = params.blend_mode;
        if (transforms.image_transform.is_key) {
            blend_mode = core::blend_mode::normal;
        }

        uniforms.blend_mode = static_cast<uint32_t>(blend_mode);
        uniforms.keyer      = static_cast<uint32_t>(params.keyer);

        if (transforms.image_transform.invert) {
            uniforms.flags |= static_cast<uint32_t>(shader_flags::invert);
        }

        if (transforms.image_transform.levels.min_input > epsilon ||
            transforms.image_transform.levels.max_input < 1.0 - epsilon ||
            transforms.image_transform.levels.min_output > epsilon ||
            transforms.image_transform.levels.max_output < 1.0 - epsilon ||
            std::abs(transforms.image_transform.levels.gamma - 1.0) > epsilon) {
            uniforms.flags |= static_cast<uint32_t>(shader_flags::levels);
            uniforms.min_input  = static_cast<float>(transforms.image_transform.levels.min_input);
            uniforms.max_input  = static_cast<float>(transforms.image_transform.levels.max_input);
            uniforms.min_output = static_cast<float>(transforms.image_transform.levels.min_output);
            uniforms.max_output = static_cast<float>(transforms.image_transform.levels.max_output);
            uniforms.gamma      = static_cast<float>(transforms.image_transform.levels.gamma);
        }

        if (std::abs(transforms.image_transform.brightness - 1.0) > epsilon ||
            std::abs(transforms.image_transform.saturation - 1.0) > epsilon ||
            std::abs(transforms.image_transform.contrast - 1.0) > epsilon) {
            uniforms.flags |= static_cast<uint32_t>(shader_flags::csb);

            uniforms.brt = static_cast<float>(transforms.image_transform.brightness);
            uniforms.sat = static_cast<float>(transforms.image_transform.saturation);
            uniforms.con = static_cast<float>(transforms.image_transform.contrast);
        }

        return {std::move(coords), uniforms};
    }
};

image_kernel::image_kernel(const spl::shared_ptr<device>& device, common::bit_depth depth)
    : impl_(new impl(device, depth))
{
}
image_kernel::~image_kernel() {}

spl::shared_ptr<renderpass> image_kernel::create_renderpass(uint32_t width, uint32_t height)
{
    return impl_->create_renderpass(width, height);
}

} // namespace caspar::accelerator::vulkan
