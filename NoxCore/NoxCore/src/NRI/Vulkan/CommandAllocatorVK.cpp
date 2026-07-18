#include "CommandAllocatorVK.h"

#include "DeviceVK.h"
#include "CommandBufferVK.h"

namespace NRI
{
    CommandAllocatorVK::CommandAllocatorVK(DeviceVK& device) : m_deviceVK(device)
    {
        vk::CommandPoolCreateInfo poolInfo
        {
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
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
