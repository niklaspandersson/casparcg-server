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
 */

#include "../StdAfx.h"

#include "render_pipeline.h"
#include "swapchain.h"
#include "texture.h"
#include "vk_check.h"

#include <common/env.h>
#include <common/log.h>
#include <common/utf.h>

#include <vulkan/vulkan.h>

#include <filesystem>
#include <fstream>

namespace caspar { namespace accelerator { namespace vk {

namespace {

std::vector<uint32_t> load_spirv(const std::string& filename)
{
    // Search paths for SPIR-V shader files
    namespace fs = std::filesystem;

    std::vector<fs::path> search_paths;

    // 1. Relative to initial folder (working directory / install location)
    auto initial = u8(env::initial_folder());
    search_paths.push_back(fs::path(initial) / "shaders" / filename);
    search_paths.push_back(fs::path(initial) / filename);

#ifdef __APPLE__
    // 2. macOS app bundle: ../Resources/shaders/ relative to executable
    search_paths.push_back(fs::path(initial) / ".." / "Resources" / "shaders" / filename);
#endif

    for (const auto& path : search_paths) {
        if (fs::exists(path)) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (file.is_open()) {
                auto size = file.tellg();
                file.seekg(0);

                std::vector<uint32_t> buffer(static_cast<size_t>(size) / sizeof(uint32_t));
                file.read(reinterpret_cast<char*>(buffer.data()), size);

                CASPAR_LOG(info) << L"[vk::render_pipeline] Loaded shader: " << path.wstring();
                return buffer;
            }
        }
    }

    // Log all searched paths for debugging
    std::wstringstream ss;
    ss << L"Failed to find shader file '" << u16(filename) << L"'. Searched:";
    for (const auto& p : search_paths) {
        ss << L"\n  " << p.wstring();
    }
    CASPAR_THROW_EXCEPTION(caspar::vk::vk_exception() << msg_info(u8(ss.str())));
}

} // anonymous namespace

struct render_pipeline::impl
{
    VkDevice         device_         = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkCommandPool    command_pool_   = VK_NULL_HANDLE;
    VkQueue          queue_          = VK_NULL_HANDLE;
    swapchain*       swapchain_      = nullptr;

    VkShaderModule        vert_shader_module_ = VK_NULL_HANDLE;
    VkShaderModule        frag_shader_module_ = VK_NULL_HANDLE;
    VkRenderPass          render_pass_        = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_  = VK_NULL_HANDLE;
    VkPipelineLayout      pipeline_layout_    = VK_NULL_HANDLE;
    VkPipeline            pipeline_           = VK_NULL_HANDLE;
    VkDescriptorPool      descriptor_pool_    = VK_NULL_HANDLE;
    VkSampler             sampler_            = VK_NULL_HANDLE;

    std::vector<VkFramebuffer>   framebuffers_;
    std::vector<VkCommandBuffer> command_buffers_;

    impl(void* device, void* physical_device, void* command_pool, void* queue, swapchain& swap)
        : device_(static_cast<VkDevice>(device))
        , physical_device_(static_cast<VkPhysicalDevice>(physical_device))
        , command_pool_(static_cast<VkCommandPool>(command_pool))
        , queue_(static_cast<VkQueue>(queue))
        , swapchain_(&swap)
    {
        create_shader_modules();
        create_render_pass();
        create_sampler();
        create_descriptor_layout();
        create_pipeline_layout();
        create_pipeline();
        create_descriptor_pool();
        create_framebuffers();
        create_command_buffers();

        CASPAR_LOG(info) << L"[vk::render_pipeline] Vulkan render pipeline initialized (Phase 9)";
    }

    ~impl()
    {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);

            cleanup_framebuffers();

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
        auto vert_spirv = load_spirv("screen_vert.spv");
        VkShaderModuleCreateInfo vertCreateInfo{};
        vertCreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vertCreateInfo.codeSize = vert_spirv.size() * sizeof(uint32_t);
        vertCreateInfo.pCode    = vert_spirv.data();
        VK(vkCreateShaderModule(device_, &vertCreateInfo, nullptr, &vert_shader_module_));

        auto frag_spirv = load_spirv("screen_frag.spv");
        VkShaderModuleCreateInfo fragCreateInfo{};
        fragCreateInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        fragCreateInfo.codeSize = frag_spirv.size() * sizeof(uint32_t);
        fragCreateInfo.pCode    = frag_spirv.data();
        VK(vkCreateShaderModule(device_, &fragCreateInfo, nullptr, &frag_shader_module_));
    }

    void create_render_pass()
    {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format         = static_cast<VkFormat>(swapchain_->format());
        colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorAttachmentRef;

        VkSubpassDependency dependency{};
        dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass    = 0;
        dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

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
        samplerInfo.borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable           = VK_FALSE;
        samplerInfo.compareOp               = VK_COMPARE_OP_ALWAYS;
        // Use NEAREST mipmap mode since textures have only 1 mip level
        samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.minLod                  = 0.0f;
        samplerInfo.maxLod                  = 0.0f;

        VK(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_));
    }

    void create_descriptor_layout()
    {
        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding            = 0;
        samplerBinding.descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount    = 1;
        samplerBinding.stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;
        samplerBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &samplerBinding;

        VK(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptor_layout_));
    }

    void create_pipeline_layout()
    {
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset     = 0;
        pushConstantRange.size       = sizeof(screen_push_constants);

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

        // No vertex input - we generate vertices in the shader
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // Dynamic viewport and scissor
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable        = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth               = 1.0f;
        rasterizer.cullMode                = VK_CULL_MODE_NONE;
        rasterizer.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable         = VK_FALSE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable  = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable   = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments    = &colorBlendAttachment;

        std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates    = dynamicStates.data();

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
        poolSize.descriptorCount = swapchain_->image_count();

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        poolInfo.maxSets       = swapchain_->image_count();
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        VK(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptor_pool_));
    }

    void create_framebuffers()
    {
        uint32_t width, height;
        swapchain_->get_extent(width, height);

        framebuffers_.resize(swapchain_->image_count());

        for (uint32_t i = 0; i < swapchain_->image_count(); ++i) {
            VkImageView attachments[] = {static_cast<VkImageView>(swapchain_->get_image_view(i))};

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass      = render_pass_;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments    = attachments;
            framebufferInfo.width           = width;
            framebufferInfo.height          = height;
            framebufferInfo.layers          = 1;

            VK(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffers_[i]));
        }
    }

    void create_command_buffers()
    {
        command_buffers_.resize(swapchain_->image_count());

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool        = command_pool_;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());

        VK(vkAllocateCommandBuffers(device_, &allocInfo, command_buffers_.data()));
    }

    void cleanup_framebuffers()
    {
        if (!command_buffers_.empty()) {
            vkFreeCommandBuffers(
                device_, command_pool_, static_cast<uint32_t>(command_buffers_.size()), command_buffers_.data());
            command_buffers_.clear();
        }

        for (auto framebuffer : framebuffers_) {
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        }
        framebuffers_.clear();
    }

    void recreate_framebuffers()
    {
        vkDeviceWaitIdle(device_);
        cleanup_framebuffers();
        create_framebuffers();
        create_command_buffers();
    }

    void render(texture&                     src,
                uint32_t                     image_index,
                const screen_push_constants& params,
                void*                        wait_semaphore,
                void*                        signal_semaphore,
                void*                        fence)
    {
        // Transition source texture to shader read optimal
        src.transition_to_shader_read();

        auto cmdBuffer = command_buffers_[image_index];

        // Reset and begin command buffer
        VK(vkResetCommandBuffer(cmdBuffer, 0));

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK(vkBeginCommandBuffer(cmdBuffer, &beginInfo));

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = descriptor_pool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &descriptor_layout_;

        VkDescriptorSet descriptorSet;
        VK(vkAllocateDescriptorSets(device_, &allocInfo, &descriptorSet));

        // Update descriptor set with source texture
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler     = sampler_;
        imageInfo.imageView   = static_cast<VkImageView>(src.image_view());
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet          = descriptorSet;
        descriptorWrite.dstBinding      = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo      = &imageInfo;

        vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);

        // Begin render pass
        uint32_t width, height;
        swapchain_->get_extent(width, height);

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass        = render_pass_;
        renderPassInfo.framebuffer       = framebuffers_[image_index];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {width, height};

        VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues    = &clearColor;

        vkCmdBeginRenderPass(cmdBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Bind pipeline
        vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

        // Set viewport and scissor
        VkViewport viewport{};
        viewport.x        = 0.0f;
        viewport.y        = 0.0f;
        viewport.width    = static_cast<float>(width);
        viewport.height   = static_cast<float>(height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {width, height};
        vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

        // Bind descriptor set
        vkCmdBindDescriptorSets(
            cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &descriptorSet, 0, nullptr);

        // Push constants
        vkCmdPushConstants(cmdBuffer,
                           pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0,
                           sizeof(screen_push_constants),
                           &params);

        // Draw fullscreen quad (6 vertices, no vertex buffer)
        vkCmdDraw(cmdBuffer, 6, 1, 0, 0);

        vkCmdEndRenderPass(cmdBuffer);

        VK(vkEndCommandBuffer(cmdBuffer));

        // Submit command buffer
        VkSemaphore          waitSemaphores[] = {static_cast<VkSemaphore>(wait_semaphore)};
        VkPipelineStageFlags waitStages[]     = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSemaphore          signalSemaphores[] = {static_cast<VkSemaphore>(signal_semaphore)};

        VkSubmitInfo submitInfo{};
        submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount   = 1;
        submitInfo.pWaitSemaphores      = waitSemaphores;
        submitInfo.pWaitDstStageMask    = waitStages;
        submitInfo.commandBufferCount   = 1;
        submitInfo.pCommandBuffers      = &cmdBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores    = signalSemaphores;

        VK(vkQueueSubmit(queue_, 1, &submitInfo, static_cast<VkFence>(fence)));

        // Free descriptor set
        vkFreeDescriptorSets(device_, descriptor_pool_, 1, &descriptorSet);
    }
};

render_pipeline::render_pipeline(void*      device,
                                 void*      physical_device,
                                 void*      command_pool,
                                 void*      queue,
                                 swapchain& swap)
    : impl_(std::make_unique<impl>(device, physical_device, command_pool, queue, swap))
{
}

render_pipeline::~render_pipeline() = default;

void render_pipeline::render(texture&                     src,
                             uint32_t                     image_index,
                             const screen_push_constants& params,
                             void*                        wait_semaphore,
                             void*                        signal_semaphore,
                             void*                        fence)
{
    impl_->render(src, image_index, params, wait_semaphore, signal_semaphore, fence);
}

void render_pipeline::recreate_framebuffers() { impl_->recreate_framebuffers(); }

}}} // namespace caspar::accelerator::vk
