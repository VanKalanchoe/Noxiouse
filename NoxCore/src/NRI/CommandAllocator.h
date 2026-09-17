#pragma once
#include <memory>

namespace NRI
{
    class CommandBuffer;

    // How command buffers of an allocator are recycled.
    enum class CommandBufferReset : uint8_t
    {
        PerCommandBuffer, // CommandBuffer::begin resets the buffer it records
        WithAllocator     // CommandAllocator::reset recycles every buffer at once (cheaper); begin does not reset
    };

    // Not thread-safe: an allocator and its command buffers are used by one thread at a time.
    class CommandAllocator
    {
    public:
        virtual ~CommandAllocator() = default;
        
        // Command buffers are always allocated out of a pool/allocator
        virtual std::unique_ptr<CommandBuffer> allocateCommandBuffer(uint32_t framesInFlight) = 0;
        
        // Every command buffer of the allocator must be done executing.
        virtual void reset() = 0;
    };
}
