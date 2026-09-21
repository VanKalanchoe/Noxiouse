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
        
        // Ray Tracing Pipeline accessors
        const std::vector<uint8_t>& getShaderGroupHandles() const override { return m_shaderGroupHandles; }
        uint32_t getShaderGroupHandleSize() const override { return m_shaderGroupHandleSize; }
        uint32_t getShaderGroupBaseAlignment() const override { return m_shaderGroupBaseAlignment; }
        uint32_t getRayGenShaderGroupIndex() const override { return m_rayGenShaderGroupIndex; }
        const std::vector<uint32_t>& getMissShaderGroupIndices() const override { return m_missShaderGroupIndices; }
        const std::vector<uint32_t>& getHitShaderGroupIndices() const override { return m_hitShaderGroupIndices; }
        const std::vector<uint32_t>& getCallableShaderGroupIndices() const override { return m_callableShaderGroupIndices; }
        
    private:
        [[nodiscard]] vk::raii::ShaderModule createShaderModule(const std::vector<char>& code) const;
        vk::ShaderStageFlagBits translateShaderStage(ShaderStage stage);
        vk::ShaderStageFlagBits determineNextStage(ShaderStage stage);
        vk::Format translateImageFormat(ImageFormat format);

    private:
        DeviceVK& m_deviceVK;
        
        // Monolithic path
        vk::raii::Pipeline m_pipeline = nullptr;
        
        // Shader Object path
        std::vector<vk::raii::ShaderEXT> m_shaders;
        std::vector<vk::ShaderStageFlagBits> m_stages;
        std::vector<vk::ShaderEXT> m_rawShaders;

        // Ray Tracing Pipeline path
        std::vector<uint8_t> m_shaderGroupHandles;
        uint32_t m_shaderGroupHandleSize = 0;
        uint32_t m_shaderGroupBaseAlignment = 0;
        uint32_t m_rayGenShaderGroupIndex = 0;
        std::vector<uint32_t> m_missShaderGroupIndices;
        std::vector<uint32_t> m_hitShaderGroupIndices;
        std::vector<uint32_t> m_callableShaderGroupIndices;
    };
}