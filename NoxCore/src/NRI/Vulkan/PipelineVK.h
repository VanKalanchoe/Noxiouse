#pragma once

#include "VulkanCommon.h"
#include "../Pipeline.h"

namespace NRI
{
    class DeviceVK; // Forward declaration
    
    class PipelineVK final : public Pipeline
    {
    public:
        PipelineVK(DeviceVK& device, const PipelineDesc& desc);
        ~PipelineVK() override = default;
        
        const vk::raii::Pipeline& getNativePipeline() const { return m_pipeline; }
        
    private:
        [[nodiscard]] vk::raii::ShaderModule createShaderModule(const std::vector<char>& code) const;
        vk::ShaderStageFlagBits translateShaderStage(ShaderStage stage);

    private:
        DeviceVK& m_deviceVK;
        vk::raii::Pipeline m_pipeline = nullptr;
    };
}