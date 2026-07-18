#pragma once

#include "VulkanCommon.h"
#include "../Pipeline.h"

namespace NRI
{
    class ShaderCompiler;
    class DeviceVK; // Forward declaration
    
    class PipelineVK final : public Pipeline
    {
    public:
        PipelineVK(DeviceVK& device, const PipelineDesc& desc, ShaderCompiler& compiler);
        ~PipelineVK() override = default;
        
        bool isShaderObject() const { return !m_shaders.empty(); }
        const vk::raii::Pipeline& getNativePipeline() const { return m_pipeline; }
        
        const std::vector<vk::raii::ShaderEXT>& getShaders() const { return m_shaders; }
        const std::vector<vk::ShaderStageFlagBits>& getStages() const { return m_stages; }
        const std::vector<vk::ShaderEXT>& getRawShaders() const { return m_rawShaders; }
        
    private:
        [[nodiscard]] vk::raii::ShaderModule createShaderModule(const std::vector<char>& code) const;
        vk::ShaderStageFlagBits translateShaderStage(ShaderStage stage);
        vk::ShaderStageFlagBits determineNextStage(ShaderStage stage);

    private:
        DeviceVK& m_deviceVK;
        
        // Monolithic path
        vk::raii::Pipeline m_pipeline = nullptr;
        
        // Shader Object path
        std::vector<vk::raii::ShaderEXT> m_shaders;
        std::vector<vk::ShaderEXT> m_rawShaders;
        std::vector<vk::ShaderStageFlagBits> m_stages;
    };
}