#pragma once

#include "MetalCommon.h"
#include "../CommandAllocator.h"

namespace NRI
{
    class DeviceMTL;

    class CommandAllocatorMTL final : public CommandAllocator
    {
    public:
        CommandAllocatorMTL(DeviceMTL& device);
        ~CommandAllocatorMTL() override;
        
        std::unique_ptr<CommandBuffer> allocateCommandBuffer(uint32_t cbCount) override;
        void reset() override;
        MTL4::CommandAllocator* getNativeAllocator(uint32_t index) { return m_commandAllocators[index]; };
    
    private:
        DeviceMTL& m_deviceMTL;
        std::vector<MTL4::CommandAllocator*> m_commandAllocators;
    };
}
