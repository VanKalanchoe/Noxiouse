#pragma once
#include <memory>

namespace NRI
{
    class CommandBuffer;
    
    class CommandAllocator
    {
    public:
        virtual ~CommandAllocator() = default;
        
        // Command buffers are always allocated out of a pool/allocator
        virtual std::unique_ptr<CommandBuffer> allocateCommandBuffer(uint32_t framesInFlight) = 0;
        
        virtual void reset() = 0;
    };
}
