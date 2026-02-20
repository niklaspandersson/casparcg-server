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
 * Author: CasparCG Team
 *
 * Phase 15: Graphics-based blend pipeline for MoltenVK compatibility.
 * Uses fragment shaders instead of compute shaders for image compositing.
 */

#include "../StdAfx.h"

#include "pipeline.h"
#include "texture.h"
#include "vk_check.h"

#include <common/log.h>

#include <vulkan/vulkan.h>

// Include the compiled SPIR-V shaders (graphics pipeline)
#include <vk_blend_vert_shader.h>
#include <vk_blend_frag_shader.h>

#include <atomic>
#include <unordered_map>

namespace caspar { namespace accelerator { namespace vk {

struct blend_pipeline::impl
{
    VkDevice              device_              = VK_NULL_HANDLE;
    VkPhysicalDevice      physical_device_     = VK_NULL_HANDLE;
    VkCommandPool         command_pool_        = VK_NULL_HANDLE;
    VkQueue               queue_               = VK_NULL_HANDLE;

    // Graphics pipeline resources
    VkShaderModule        vert_shader_module_  = VK_NULL_HANDLE;
    VkShaderModule        frag_shader_module_  = VK_NULL_HANDLE;
    VkRenderPass          render_pass_         = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_   = VK_NULL_HANDLE;
    VkPipelineLayout      pipeline_layout_     = VK_NULL_HANDLE;
    VkPipeline            pipeline_            = VK_NULL_HANDLE;
    VkDescriptorPool      descriptor_pool_     = VK_NULL_HANDLE;
    VkSampler             sampler_             = VK_NULL_HANDLE;

    // Framebuffer cache (keyed by image view pointer)
    std::unordered_map<VkImageView, VkFramebuffer> framebuffer_cache_;
    int last_fb_width_  = 0;
    int last_fb_height_ = 0;

    // Resource tracking for diagnostics
    std::atomic<uint64_t> total_renders_{0};
    std::atomic<uint64_t> descriptor_allocs_{0};
    std::atomic<uint64_t> descriptor_frees_{0};
    std::atomic<uint64_t> framebuffer_creates_{0};

    impl(void* device, void* physical_device, void* command_pool, void* queue)
        : device_(static_cast<VkDevice>(device))
        , physical_device_(static_cast<VkPhysicalDevice>(physical_device))
        , command_pool_(static_cast<VkCommandPool>(command_pool))
        , queue_(static_cast<VkQueue>(queue))
    {
        create_shader_modules();
        create_render_pass();
        create_sampler();
        create_descriptor_layout();
        create_pipeline_layout();
        create_pipeline();
        create_descriptor_pool();

        CASPAR_LOG(info) << L"[vk::blend_pipeline] Vulkan blend graphics pipeline initialized (Phase 15 - MoltenVK compatible)";
    }

    ~impl()
    {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);

            // Destroy cached framebuffers
            for (auto& [view, fb] : framebuffer_cache_) {
                vkDestroyFramebuffer(device_, fb, nullptr);
            }
            framebuffer_cache_.clear();

            if (descriptor_pool_ != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
            }
            if (sampler_ != VK_NULL_HANDLE) {
                vkDestroySampler(device_, sampler_, nullptr);
            }
            if (pipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, pipeline_, nullptr);
            }
            if (pipeline_layout_ != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            }
            if (descriptor_layout_ != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
            }
            if (render_pass_ != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device_, render_pass_, nullptr);
            }
            if (vert_shader_module_ != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_, vert_shader_module_, nullptr);
            }
            if (frag_shader_module_ != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_, frag_shader_module_, nullptr);
            }
        }
    }

    void create_shader_modules()
    {
        VkShaderModuleCreateInfo vertCreateInfo{};
        vertCreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vertCreateInfo.codeSize = blend_vert_shader_spv_size * sizeof(uint32_t);
        vertCreateInfo.pCode    = blend_vert_shader_spv;
        VK(vkCreateShaderModule(device_, &vertCreateInfo, nullptr, &vert_shader_module_));

        VkShaderModuleCreateInfo fragCreateInfo{};
        fragCreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        fragCreateInfo.codeSize = blend_frag_shader_spv_size * sizeof(uint32_t);
        fragCreateInfo.pCode    = blend_frag_shader_spv;
        VK(vkCreateShaderModule(device_, &fragCreateInfo, nullptr, &frag_shader_module_));
    }

    void create_render_pass()
    {
        // Render pass for rendering to RGBA8 texture
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format         = VK_FORMAT_R8G8B8A8_UNORM;
        colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;  // Keep existing content for blending
        colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorAttachmentRef;

        // Dependency to ensure previous renders are complete
        VkSubpassDependency dependency{};
        dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass    = 0;
        dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments    = &colorAttachment;
        renderPassInfo.subpassCount    = 1;
        renderPassInfo.pSubpasses      = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies   = &dependency;

        VK(vkCreateRenderPass(device_, &renderPassInfo, nullptr, &render_pass_));
    }

    void create_sampler()
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter               = VK_FILTER_LINEAR;
        samplerInfo.minFilter               = VK_FILTER_LINEAR;
        samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable        = VK_FALSE;
        samplerInfo.maxAnisotropy           = 1.0f;
        samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable           = VK_FALSE;
        samplerInfo.compareOp               = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.minLod                  = 0.0f;
        samplerInfo.maxLod                  = 0.0f;

        VK(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_));
    }

    void create_descriptor_layout()
    {
        // 5 combined image samplers: 4 source planes + 1 destination (for reading in blend modes)
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};

        for (int i = 0; i < 5; ++i) {
            bindings[i].binding            = i;
            bindings[i].descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount    = 1;
            bindings[i].stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[i].pImmutableSamplers = nullptr;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings    = bindings.data();

        VK(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptor_layout_));
    }

    void create_pipeline_layout()
    {
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset     = 0;
        pushConstantRange.size       = sizeof(blend_push_constants);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount         = 1;
        pipelineLayoutInfo.pSetLayouts            = &descriptor_layout_;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges    = &pushConstantRange;

        VK(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipeline_layout_));
    }

    void create_pipeline()
    {
        // Shader stages
        VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
        vertShaderStageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = vert_shader_module_;
        vertShaderStageInfo.pName  = "main";

        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        fragShaderStageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = frag_shader_module_;
        fragShaderStageInfo.pName  = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

        // Vertex input (no vertex buffers - full-screen triangle generated in shader)
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        // Input assembly
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // Viewport and scissor (dynamic)
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount  = 1;

        // Rasterizer
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode    = VK_CULL_MODE_NONE;
        rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth   = 1.0f;

        // Multisampling
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Color blending - use premultiplied alpha blending (standard "over" composite)
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.blendEnable         = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;  // Premultiplied alpha
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
        colorBlendAttachment.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments    = &colorBlendAttachment;

        // Dynamic state
        std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates    = dynamicStates.data();

        // Create graphics pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount          = 2;
        pipelineInfo.pStages             = shaderStages;
        pipelineInfo.pVertexInputState   = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState      = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState   = &multisampling;
        pipelineInfo.pColorBlendState    = &colorBlending;
        pipelineInfo.pDynamicState       = &dynamicState;
        pipelineInfo.layout              = pipeline_layout_;
        pipelineInfo.renderPass          = render_pass_;
        pipelineInfo.subpass             = 0;

        VK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_));
    }

    void create_descriptor_pool()
    {
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 10;  // 5 samplers * 2 descriptor sets

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        poolInfo.maxSets       = 2;
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        VK(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptor_pool_));
    }

    VkFramebuffer get_or_create_framebuffer(VkImageView imageView, int width, int height)
    {
        // Check if we need to clear cache due to size change
        if (width != last_fb_width_ || height != last_fb_height_) {
            for (auto& [view, fb] : framebuffer_cache_) {
                vkDestroyFramebuffer(device_, fb, nullptr);
            }
            framebuffer_cache_.clear();
            last_fb_width_  = width;
            last_fb_height_ = height;
        }

        auto it = framebuffer_cache_.find(imageView);
        if (it != framebuffer_cache_.end()) {
            return it->second;
        }

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass      = render_pass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments    = &imageView;
        framebufferInfo.width           = width;
        framebufferInfo.height          = height;
        framebufferInfo.layers          = 1;

        VkFramebuffer framebuffer;
        VK(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffer));
        framebuffer_creates_++;

        framebuffer_cache_[imageView] = framebuffer;
        return framebuffer;
    }

    VkCommandBuffer begin_command_buffer()
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool        = command_pool_;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer cmdBuffer;
        VK(vkAllocateCommandBuffers(device_, &allocInfo, &cmdBuffer));

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        VK(vkBeginCommandBuffer(cmdBuffer, &beginInfo));

        return cmdBuffer;
    }

    void end_command_buffer(VkCommandBuffer cmdBuffer)
    {
        VK(vkEndCommandBuffer(cmdBuffer));

        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &cmdBuffer;

        VK(vkQueueSubmit(queue_, 1, &submitInfo, VK_NULL_HANDLE));
        VK(vkQueueWaitIdle(queue_));

        vkFreeCommandBuffers(device_, command_pool_, 1, &cmdBuffer);
    }

    void transition_for_render(VkCommandBuffer cmdBuffer, VkImage image, VkImageLayout oldLayout)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = oldLayout;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = image;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        barrier.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask                   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        vkCmdPipelineBarrier(cmdBuffer,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);
    }

    void transition_after_render(VkCommandBuffer cmdBuffer, VkImage image)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = image;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        barrier.srcAccessMask                   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(cmdBuffer,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);
    }

    void execute(texture& src, texture& dst, const blend_push_constants& params)
    {
        // Single source texture bound to all planes
        execute_internal(&src, nullptr, nullptr, nullptr, dst, params);
    }

    void execute(const std::vector<std::shared_ptr<texture>>& planes, texture& dst, const blend_push_constants& params)
    {
        texture* plane_ptrs[4] = {nullptr, nullptr, nullptr, nullptr};
        for (size_t i = 0; i < planes.size() && i < 4; ++i) {
            if (planes[i]) {
                plane_ptrs[i] = planes[i].get();
            }
        }
        execute_internal(plane_ptrs[0], plane_ptrs[1], plane_ptrs[2], plane_ptrs[3], dst, params);
    }

    void execute_internal(texture* plane0, texture* plane1, texture* plane2, texture* plane3,
                          texture& dst, const blend_push_constants& params)
    {
        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = descriptor_pool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &descriptor_layout_;

        VkDescriptorSet descriptorSet;
        VK(vkAllocateDescriptorSets(device_, &allocInfo, &descriptorSet));
        descriptor_allocs_++;

        // Use plane0 as fallback for unused planes
        VkImageView fallback_view = plane0 ? static_cast<VkImageView>(plane0->image_view()) : VK_NULL_HANDLE;

        // Set up image info for all 5 samplers (4 planes + 1 dst for reading)
        std::array<VkDescriptorImageInfo, 5> imageInfos{};

        imageInfos[0].sampler     = sampler_;
        imageInfos[0].imageView   = plane0 ? static_cast<VkImageView>(plane0->image_view()) : fallback_view;
        imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        imageInfos[1].sampler     = sampler_;
        imageInfos[1].imageView   = plane1 ? static_cast<VkImageView>(plane1->image_view()) : fallback_view;
        imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        imageInfos[2].sampler     = sampler_;
        imageInfos[2].imageView   = plane2 ? static_cast<VkImageView>(plane2->image_view()) : fallback_view;
        imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        imageInfos[3].sampler     = sampler_;
        imageInfos[3].imageView   = plane3 ? static_cast<VkImageView>(plane3->image_view()) : fallback_view;
        imageInfos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Binding 4: destination for reading (for complex blend modes)
        // For now, use plane0 as a placeholder - complex blend modes will need special handling
        imageInfos[4].sampler     = sampler_;
        imageInfos[4].imageView   = fallback_view;
        imageInfos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 5> descriptorWrites{};
        for (int i = 0; i < 5; ++i) {
            descriptorWrites[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[i].dstSet          = descriptorSet;
            descriptorWrites[i].dstBinding      = i;
            descriptorWrites[i].dstArrayElement = 0;
            descriptorWrites[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[i].descriptorCount = 1;
            descriptorWrites[i].pImageInfo      = &imageInfos[i];
        }

        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);

        // Get or create framebuffer for destination
        VkImageView dstImageView = static_cast<VkImageView>(dst.image_view());
        VkFramebuffer framebuffer = get_or_create_framebuffer(dstImageView, params.dst_width, params.dst_height);

        // Record command buffer
        auto cmdBuffer = begin_command_buffer();

        // Transition destination to color attachment
        transition_for_render(cmdBuffer, static_cast<VkImage>(dst.image()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Begin render pass
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass        = render_pass_;
        renderPassInfo.framebuffer       = framebuffer;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {static_cast<uint32_t>(params.dst_width), static_cast<uint32_t>(params.dst_height)};

        vkCmdBeginRenderPass(cmdBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Set viewport and scissor
        VkViewport viewport{};
        viewport.x        = 0.0f;
        viewport.y        = 0.0f;
        viewport.width    = static_cast<float>(params.dst_width);
        viewport.height   = static_cast<float>(params.dst_height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {static_cast<uint32_t>(params.dst_width), static_cast<uint32_t>(params.dst_height)};
        vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

        // Bind pipeline and descriptors
        vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &descriptorSet, 0, nullptr);

        // Push constants
        vkCmdPushConstants(cmdBuffer, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(blend_push_constants), &params);

        // Draw full-screen triangle (3 vertices, generated in vertex shader)
        vkCmdDraw(cmdBuffer, 3, 1, 0, 0);

        // End render pass
        vkCmdEndRenderPass(cmdBuffer);

        // Transition destination back to shader read
        transition_after_render(cmdBuffer, static_cast<VkImage>(dst.image()));

        end_command_buffer(cmdBuffer);

        // Sync the texture's internal layout tracking with what we just transitioned to
        // This is critical for copy_to to use the correct source layout
        dst.set_layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Free descriptor set
        vkFreeDescriptorSets(device_, descriptor_pool_, 1, &descriptorSet);
        descriptor_frees_++;
        total_renders_++;
    }
};

blend_pipeline::blend_pipeline(void* device, void* physical_device, void* command_pool, void* queue)
    : impl_(std::make_unique<impl>(device, physical_device, command_pool, queue))
{
}

blend_pipeline::~blend_pipeline() = default;

void blend_pipeline::execute(texture& src, texture& dst, const blend_push_constants& params)
{
    impl_->execute(src, dst, params);
}

void blend_pipeline::execute(const std::vector<std::shared_ptr<texture>>& planes, texture& dst, const blend_push_constants& params)
{
    impl_->execute(planes, dst, params);
}

}}} // namespace caspar::accelerator::vk
