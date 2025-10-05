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
#include "shader.h"
#include "vulkan_image_fragment.h"
#include "vulkan_image_vertex.h"

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

    auto vertShaderModule = createShaderModule(vertex_shader, sizeof(vertex_shader));
    auto fragShaderModule = createShaderModule(fragment_shader, sizeof(fragment_shader));

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

struct shader::impl
{
    uint32_t program_;
    // std::unordered_map<std::string, GLint> uniform_locations_;
    // std::unordered_map<std::string, GLint> attrib_locations_;

    impl(const impl&)            = delete;
    impl& operator=(const impl&) = delete;

  public:
    impl(const std::string& vertex_source_str, const std::string& fragment_source_str)
        : program_(0)
    {
    }

    ~impl() { /*glDeleteProgram(program_);*/ }

    GLint get_uniform_location(const char* name)
    {
        // auto it = uniform_locations_.find(name);
        // if (it == uniform_locations_.end())
        //     it = uniform_locations_.insert(std::make_pair(name, glGetUniformLocation(program_, name))).first;
        // return it->second;
        return 0;
    }

    GLint get_attrib_location(const char* name)
    {
        // auto it = attrib_locations_.find(name);
        // if (it == attrib_locations_.end())
        //     it = attrib_locations_.insert(std::make_pair(name, glGetAttribLocation(program_, name))).first;
        // return it->second;
        return 0;
    }

    void set(const std::string& name, bool value) { set(name, value ? 1 : 0); }

    void set(const std::string& name, int value) { /*GL(glUniform1i(get_uniform_location(name.c_str()), value));*/ }

    void set(const std::string& name, float value) { /*GL(glUniform1f(get_uniform_location(name.c_str()), value));*/ }

    void set(const std::string& name, double value0, double value1)
    {
        // GL(glUniform2f(get_uniform_location(name.c_str()), static_cast<float>(value0), static_cast<float>(value1)));
    }
    void set(const std::string& name, double value0, double value1, double value2)
    {
        // GL(glUniform3f(get_uniform_location(name.c_str()),
        //                static_cast<float>(value0),
        //                static_cast<float>(value1),
        //                static_cast<float>(value1)));
    }

    void set(const std::string& name, double value)
    {
        // GL(glUniform1f(get_uniform_location(name.c_str()), static_cast<float>(value)));
    }
    void set_matrix3(const std::string& name, const float* value)
    {
        // GL(glUniformMatrix3fv(get_uniform_location(name.c_str()), 1, GL_TRUE, value));
    }

    void use() { /*GL(glUseProgramObjectARB(program_));*/ }
};

shader::shader(const std::string& vertex_source_str, const std::string& fragment_source_str)
    : impl_(new impl(vertex_source_str, fragment_source_str))
{
}
shader::~shader() {}
void shader::set(const std::string& name, bool value) { impl_->set(name, value); }
void shader::set(const std::string& name, int value) { impl_->set(name, value); }
void shader::set(const std::string& name, float value) { impl_->set(name, value); }
void shader::set(const std::string& name, double value0, double value1) { impl_->set(name, value0, value1); }
void shader::set(const std::string& name, double value0, double value1, double value2)
{
    impl_->set(name, value0, value1, value2);
}
void  shader::set(const std::string& name, double value) { impl_->set(name, value); }
void  shader::set_matrix3(const std::string& name, const float* value) { impl_->set_matrix3(name, value); }
GLint shader::get_attrib_location(const char* name) { return impl_->get_attrib_location(name); }
int   shader::id() const { return impl_->program_; }
void  shader::use() const { impl_->use(); }

}}} // namespace caspar::accelerator::vulkan
