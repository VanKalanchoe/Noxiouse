#include "CommandAllocatorVK.h"

#include "DeviceVK.h"
#include "CommandBufferVK.h"

namespace NRI
{
    CommandAllocatorVK::CommandAllocatorVK(DeviceVK& device, CommandBufferReset resetMode) : m_deviceVK(device), m_resetMode(resetMode)
    {
        vk::CommandPoolCreateInfo poolInfo
        {
            // Individual resets need the flag; whole-pool resets are cheaper and do not.
            .flags = resetMode == CommandBufferReset::PerCommandBuffer ? vk::CommandPoolCreateFlagBits::eResetCommandBuffer : vk::CommandPoolCreateFlags{},
            .queueFamilyIndex = m_deviceVK.getQueueIndex()
        };
        m_commandPool = vk::raii::CommandPool(m_deviceVK.getDevice(), poolInfo);
    }

    std::unique_ptr<CommandBuffer> CommandAllocatorVK::allocateCommandBuffer(uint32_t cbCount)
    {
        return std::make_unique<CommandBufferVK>(m_deviceVK, *this, cbCount);
    }

    void CommandAllocatorVK::reset()
    {
        m_commandPool.reset();
    }
}
