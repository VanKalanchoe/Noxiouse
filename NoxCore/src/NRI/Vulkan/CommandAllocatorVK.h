#pragma once
#include "VulkanCommon.h"
#include "../CommandAllocator.h"

namespace NRI
{
    class DeviceVK;

    class CommandAllocatorVK final : public CommandAllocator
    {
    public:
        CommandAllocatorVK(DeviceVK& device, CommandBufferReset resetMode);
        ~CommandAllocatorVK() override = default;

        std::unique_ptr<CommandBuffer> allocateCommandBuffer(uint32_t cbCount) override;
        void reset() override;
        vk::raii::CommandPool& getNativePool() { return m_commandPool; }
        CommandBufferReset getResetMode() const { return m_resetMode; }

    private:
        DeviceVK& m_deviceVK;
        vk::raii::CommandPool m_commandPool = nullptr;
        CommandBufferReset m_resetMode;
    };
};
