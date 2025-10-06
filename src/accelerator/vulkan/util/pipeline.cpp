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
 * Author: Robert Nagy, ronag89@gmail.com
 */

#include "pipeline.h"
#include "../image/image_kernel.h"
#include "texture.h"

#include "vulkan_image_fragment.h"
#include "vulkan_image_vertex.h"
#include <core/frame/geometry.h>

#include <vulkan/vulkan.hpp>

#include <unordered_map>

namespace caspar { namespace accelerator { namespace vulkan {

std::vector<vk::PipelineShaderStageCreateInfo> create_shader_program(vk::Device device)
{
    // Helper to create shader module
    auto createShaderModule = [&](const uint8_t* code, size_t size) {
        vk::ShaderModuleCreateInfo createInfo{};
        createInfo.codeSize = size;
        createInfo.pCode    = reinterpret_cast<const uint32_t*>(code);
        return device.createShaderModule(createInfo);
    };

    auto vertShaderModule = createShaderModule(vertex_shader, sizeof(vertex_shader) - 1);
    auto fragShaderModule = createShaderModule(fragment_shader, sizeof(fragment_shader) - 1);

    vk::PipelineShaderStageCreateInfo vertShaderStageInfo;
    vertShaderStageInfo.stage  = vk::ShaderStageFlagBits::eVertex;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName  = "main";

    vk::PipelineShaderStageCreateInfo fragShaderStageInfo;
    fragShaderStageInfo.stage  = vk::ShaderStageFlagBits::eFragment;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName  = "main";

    return {vertShaderStageInfo, fragShaderStageInfo};
}

std::array<vk::VertexInputAttributeDescription, 2> get_attribute_descriptions(uint32_t binding)
{
    std::array<vk::VertexInputAttributeDescription, 2> attributeDescriptions{
        {{0, binding, vk::Format::eR32G32Sfloat, offsetof(core::frame_geometry::coord, vertex_x) / 2},
         {1, binding, vk::Format::eR32G32B32A32Sfloat, offsetof(core::frame_geometry::coord, texture_x) / 2}}};

    return attributeDescriptions;
}

const int DescriptorPoolSize = 10;

struct uniform_block
{
    uint32_t color_space_index;
    float    precision_factor[4];
    int32_t  blend_mode;
    int32_t  keyer;
    int32_t  pixel_format;
    float    opacity;

    /* levels */
    float min_input;
    float max_input;
    float gamma;
    float min_output;
    float max_output;

    /* contrast, saturation & brightness */
    float brt;
    float sat;
    float con;

    /* Chroma */
    float chroma_target_hue;
    float chroma_hue_width;
    float chroma_min_saturation;
    float chroma_min_brightness;
    float chroma_softness;
    float chroma_spill_suppress;
    float chroma_spill_suppress_saturation;

    uint32_t flags;
} default_uniforms;

struct pipeline::impl
{
    vk::Device device_;

    vk::Sampler                    textureSampler_;
    vk::DescriptorSetLayout        descriptorSetLayout_;
    vk::DescriptorPool             descriptorPool_;
    std::vector<vk::DescriptorSet> descriptorSets_;

    vk::PipelineLayout pipelineLayout_;
    vk::Pipeline       pipeline_;

    size_t currentDescriptorSet_ = 0;

    impl(const impl&)            = delete;
    impl& operator=(const impl&) = delete;

    void setup_descriptors()
    {
        // Create descriptor set layout
        vk::DescriptorSetLayoutBinding planesLayoutBinding{};
        planesLayoutBinding.binding         = 0;
        planesLayoutBinding.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
        planesLayoutBinding.descriptorCount = 4;
        planesLayoutBinding.stageFlags      = vk::ShaderStageFlagBits::eFragment;

        vk::DescriptorSetLayoutBinding backgroundLayoutBinding{};
        backgroundLayoutBinding.binding         = 1;
        backgroundLayoutBinding.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
        backgroundLayoutBinding.descriptorCount = 1;
        backgroundLayoutBinding.stageFlags      = vk::ShaderStageFlagBits::eFragment;

        vk::DescriptorSetLayoutBinding localKeyLayoutBinding{};
        localKeyLayoutBinding.binding         = 2;
        localKeyLayoutBinding.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
        localKeyLayoutBinding.descriptorCount = 1;
        localKeyLayoutBinding.stageFlags      = vk::ShaderStageFlagBits::eFragment;

        vk::DescriptorSetLayoutBinding localMaxLayoutBinding{};
        localMaxLayoutBinding.binding         = 3;
        localMaxLayoutBinding.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
        localMaxLayoutBinding.descriptorCount = 1;
        localMaxLayoutBinding.stageFlags      = vk::ShaderStageFlagBits::eFragment;

        std::vector bindings = {
            planesLayoutBinding, backgroundLayoutBinding, localKeyLayoutBinding, localMaxLayoutBinding};

        vk::DescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.setBindings(bindings);

        descriptorSetLayout_ = device_.createDescriptorSetLayout(layoutInfo);

        // Create descriptor pool
        vk::DescriptorPoolSize samplerPoolSize(vk::DescriptorType::eCombinedImageSampler, 7 * DescriptorPoolSize);
        vk::DescriptorPoolSize poolSizes[]{samplerPoolSize};

        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.maxSets = DescriptorPoolSize;

        poolInfo.setPoolSizes(poolSizes);
        descriptorPool_ = device_.createDescriptorPool(poolInfo);

        // Allocate descriptor sets
        std::vector<vk::DescriptorSetLayout> layouts(DescriptorPoolSize, descriptorSetLayout_);
        vk::DescriptorSetAllocateInfo        allocInfo;
        allocInfo.descriptorPool = descriptorPool_;
        allocInfo.setSetLayouts(layouts);

        descriptorSets_ = device_.allocateDescriptorSets(allocInfo);
    }

    void setup_sampler()
    {
        vk::SamplerCreateInfo samplerInfo{};

        samplerInfo.magFilter               = vk::Filter::eLinear;
        samplerInfo.minFilter               = vk::Filter::eLinear;
        samplerInfo.mipmapMode              = vk::SamplerMipmapMode::eLinear;
        samplerInfo.addressModeU            = vk::SamplerAddressMode::eRepeat;
        samplerInfo.addressModeV            = vk::SamplerAddressMode::eRepeat;
        samplerInfo.addressModeW            = vk::SamplerAddressMode::eRepeat;
        samplerInfo.mipLodBias              = 0.0f;
        samplerInfo.anisotropyEnable        = VK_FALSE;
        samplerInfo.maxAnisotropy           = 2;
        samplerInfo.compareEnable           = VK_FALSE;
        samplerInfo.compareOp               = vk::CompareOp::eAlways;
        samplerInfo.minLod                  = 0.0f;
        samplerInfo.maxLod                  = 0.0f;
        samplerInfo.borderColor             = vk::BorderColor::eIntOpaqueBlack;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;

        textureSampler_ = device_.createSampler(samplerInfo);
    }

  public:
    impl(vk::Device device)
        : device_(device)
    {
        setup_descriptors();

        setup_sampler();

        // Vertex input
        auto attributeDescriptions = get_attribute_descriptions(0);

        auto vertexBindings =
            vk::VertexInputBindingDescription(0, sizeof(core::frame_geometry::coord), vk::VertexInputRate::eVertex);
        vk::PipelineVertexInputStateCreateInfo vertexInputInfo;
        vertexInputInfo.setVertexBindingDescriptions(vertexBindings);
        vertexInputInfo.setVertexAttributeDescriptions(attributeDescriptions);

        // Input assembly
        vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.topology               = vk::PrimitiveTopology::eTriangleStrip;
        inputAssembly.primitiveRestartEnable = VK_TRUE;

        vk::PipelineViewportStateCreateInfo viewportState{};
        viewportState.scissorCount  = 1;
        viewportState.viewportCount = 1;
        vk::DynamicState dynamicStates[]{vk::DynamicState::eViewport, vk::DynamicState::eScissor};

        // Rasterizer
        vk::PipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.depthClampEnable        = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode             = vk::PolygonMode::eFill;
        rasterizer.cullMode                = vk::CullModeFlagBits::eBack;
        rasterizer.frontFace               = vk::FrontFace::eClockwise;
        rasterizer.depthBiasEnable         = VK_FALSE;
        rasterizer.lineWidth               = 1.0f;

        // Multisampling
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
        multisampling.sampleShadingEnable  = VK_FALSE;

        // Color blending
        vk::PipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.blendEnable    = vk::False;
        colorBlendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

        vk::PipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.logicOpEnable = vk::False;
        colorBlending.logicOp       = vk::LogicOp::eCopy;
        colorBlending.setAttachments(colorBlendAttachment);

        vk::PushConstantRange range{};
        range.stageFlags = vk::ShaderStageFlagBits::eFragment;
        range.offset     = 0;
        range.size       = sizeof(uniform_block);

        // Pipeline layout
        vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.setSetLayouts(descriptorSetLayout_);
        pipelineLayoutInfo.setPushConstantRanges(range);

        pipelineLayout_ = device_.createPipelineLayout(pipelineLayoutInfo);

        vk::PipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.setDynamicStates(dynamicStates);

        // Graphics pipeline
        vk::GraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.pVertexInputState   = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState      = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pDynamicState       = &dynamicState;
        pipelineInfo.pMultisampleState   = &multisampling;
        pipelineInfo.pColorBlendState    = &colorBlending;
        pipelineInfo.layout              = pipelineLayout_;
        pipelineInfo.renderPass          = nullptr;
        pipelineInfo.subpass             = 0;

        auto shaderStages = std::move(create_shader_program(device_));
        pipelineInfo.setStages(shaderStages);

        vk::Format                      swapchain_image_format = vk::Format::eB8G8R8A8Unorm;
        vk::PipelineRenderingCreateInfo rendering_info{};
        rendering_info.colorAttachmentCount    = 1;
        rendering_info.pColorAttachmentFormats = &swapchain_image_format;

        pipelineInfo.pNext = &rendering_info;

        pipeline_ = device_.createGraphicsPipeline(nullptr, pipelineInfo).value;

        // Cleanup shader modules after pipeline creation
        for (auto& shaderStage : shaderStages) {
            device_.destroyShaderModule(shaderStage.module);
        }
    }

    vk::DescriptorSet acquire_descriptor_set(const std::array<vk::ImageView, 4>& textures)
    {
        auto descriptorSet    = descriptorSets_[currentDescriptorSet_];
        currentDescriptorSet_ = (currentDescriptorSet_ + 1) % DescriptorPoolSize;

        vk::DescriptorImageInfo imageInfo{};
        imageInfo.sampler     = textureSampler_;
        imageInfo.imageView   = nullptr;
        imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        std::vector<vk::DescriptorImageInfo> images(4, imageInfo);
        images[0].imageView   = textures[0];
        images[0].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        images[1].imageView   = textures[1];
        images[1].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        images[2].imageView   = textures[2];
        images[2].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        images[3].imageView   = textures[3];
        images[3].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        vk::WriteDescriptorSet imageDescriptorWrite{};
        imageDescriptorWrite.dstSet          = descriptorSet;
        imageDescriptorWrite.dstBinding      = 0;
        imageDescriptorWrite.dstArrayElement = 0;
        imageDescriptorWrite.descriptorType  = vk::DescriptorType::eCombinedImageSampler;
        imageDescriptorWrite.setImageInfo(images);

        vk::WriteDescriptorSet descriptorWrites[]{imageDescriptorWrite};
        device_.updateDescriptorSets(descriptorWrites, nullptr);

        return descriptorSet;
    }

    void draw(vk::CommandBuffer commandBuffer, vk::Buffer vertexBuffer, const std::array<vk::ImageView, 4>& textures)
    {
        auto descriptorSet = acquire_descriptor_set(textures);
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_);
        commandBuffer.bindVertexBuffers(0, vertexBuffer, {0});
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout_, 0, descriptorSet, nullptr);
        commandBuffer.pushConstants(
            pipelineLayout_, vk::ShaderStageFlagBits::eFragment, 0, sizeof(default_uniforms), &default_uniforms);
        commandBuffer.draw(4, 1, 0, 0);
    }

    ~impl()
    {
        device_.destroyDescriptorPool(descriptorPool_);
        device_.destroyDescriptorSetLayout(descriptorSetLayout_);
        device_.destroySampler(textureSampler_);

        device_.destroyPipeline(pipeline_);
        device_.destroyPipelineLayout(pipelineLayout_);
    }
};

pipeline::pipeline(vk::Device device)
    : impl_(new impl(device))
{
}
pipeline::~pipeline() {}

void pipeline::draw(vk::CommandBuffer                   commandBuffer,
                    vk::Buffer                          vertexBuffer,
                    const std::array<vk::ImageView, 4>& textures)
{
    impl_->draw(commandBuffer, vertexBuffer, textures);
}

vk::Pipeline pipeline::id() const { return impl_->pipeline_; }

}}} // namespace caspar::accelerator::vulkan
