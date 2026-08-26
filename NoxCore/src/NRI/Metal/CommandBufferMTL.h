#pragma once

#include "MetalCommon.h"
#include "../CommandBuffer.h"

namespace NRI
{
    class DeviceMTL;
    class CommandAllocatorMTL;
    class DescriptorHeap;

    class CommandBufferMTL final : public CommandBuffer
    {
    public:
        CommandBufferMTL(DeviceMTL& device, CommandAllocatorMTL& allocator, uint32_t cbCount);
        ~CommandBufferMTL() override = default;
        
        void begin(uint32_t index = 0, bool oneTimeSubmit = false) override; // We can update this later to accept an index: begin(uint32_t frameIndex)
        void end(uint32_t index = 0) override;
        void beginRendering(RenderDesc& desc) override;
        
        MTL4::CommandBuffer* getNativeBuffer(uint32_t index) { return m_commandBuffers[index]; }
        MTL4::CommandBuffer* getActiveNativeBuffer() { return m_commandBuffers[m_currentFrameIndex]; }
        
    private:
        DeviceMTL& m_deviceMTL;
        CommandAllocatorMTL& m_allocatorMTL;
        
        std::vector<MTL4::CommandBuffer*> m_commandBuffers;
        // Caches the index passed into the begin method
        uint32_t m_currentFrameIndex = 0;
        
        MTL4::RenderCommandEncoder* m_renderEncoder = nullptr;
        MTL4::ComputeCommandEncoder* m_computeEncoder = nullptr;
    };
}
