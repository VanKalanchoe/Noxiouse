#include "CommandAllocatorMTL.h"

#include "DeviceMTL.h"
#include "CommandBufferMTL.h"

namespace NRI
{
    CommandAllocatorMTL::CommandAllocatorMTL(DeviceMTL& device) : m_deviceMTL(device)
    {
    }

    CommandAllocatorMTL::~CommandAllocatorMTL()
    {
        for(MTL4::CommandAllocator* allocator : m_commandAllocators)
        {
            if(allocator)
                allocator->release();
        }
        
        m_commandAllocators.clear();
    }

    std::unique_ptr<CommandBuffer> CommandAllocatorMTL::allocateCommandBuffer(uint32_t cbCount)
    {
        m_commandAllocators.resize(cbCount);
        for (uint32_t i = 0; i < cbCount; i++)
        {
            if(m_commandAllocators[i] == nullptr)
            {
                m_commandAllocators[i] = m_deviceMTL.getDevice()->newCommandAllocator();
                
                if(!m_commandAllocators[i])
                {
                    throw std::runtime_error("Failed to create Metal 4 command allocator");
                }
            }
        }
        
        return std::make_unique<CommandBufferMTL>(m_deviceMTL, *this, cbCount);
    }

    void CommandAllocatorMTL::reset()
    {
        for(MTL4::CommandAllocator* allocator : m_commandAllocators)
        {
            allocator->reset();
        }
    }
}
