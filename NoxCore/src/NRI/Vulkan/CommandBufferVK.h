#pragma once
#include "VulkanCommon.h"
#include "../CommandBuffer.h"

namespace NRI
{
    class DeviceVK;
    class CommandAllocatorVK;
    class DescriptorHeap;
    
    class CommandBufferVK final : public CommandBuffer
    {
    public:
        CommandBufferVK(DeviceVK& device, CommandAllocatorVK& allocator, uint32_t cbCount);
        ~CommandBufferVK() override = default;
        
        void begin(uint32_t index = 0, bool oneTimeSubmit = false) override; // We can update this later to accept an index: begin(uint32_t frameIndex)
        void end(uint32_t index = 0) override;
        void beginRendering(RenderDesc& desc) override;
        void endRendering() override;
        void renderImGui() override;
        void bindPipeline(PipelineBindPoint bindPoint, Pipeline& pipeline) override;
        void setViewport(Extent2D swapChainExtent) override;
        void setScissor(Extent2D swapChainExtent) override;
        void setViewportWithCount(const Extent2D& swapChainExtent) override;
        void setScissorWithCount(const Extent2D& swapChainExtent) override;
        void bindDescriptorHeaps(DescriptorHeap* resourceHeap, DescriptorHeap* samplerHeap) override;
        void pushData(const void* data, uint32_t size) override;
        void bindVertexBuffers(uint32_t firstBinding, Buffer& buffer, uint64_t offset) override;
        void bindIndexBuffer(Buffer& buffer, uint64_t offet) override;
        void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;

        void copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, uint64_t deviceSize) override;
        void transitionTextureLayout(Texture& texture, TextureLayout oldLayout, TextureLayout newLayout) override;
        void transitionSwapchainLayout(Swapchain& swapchain, uint32_t imageIndex, TextureLayout oldLayout, TextureLayout newLayout) override;
        void submitImageBarrier(vk::Image image, TextureLayout oldLayout, TextureLayout newLayout, vk::ImageAspectFlags aspectFlags);
        void getSyncFlags(TextureLayout layout, bool isSource, vk::PipelineStageFlags2& stageMask, vk::AccessFlags2& accessMask) const;
        vk::ImageLayout translateLayoutToVk(TextureLayout layout) const;

        vk::raii::CommandBuffer& getNativeBuffer(uint32_t index) { return m_commandBuffers[index]; }
        vk::raii::CommandBuffer& getActiveNativeBuffer() { return m_commandBuffers[m_currentFrameIndex]; }
        
    private:
        DeviceVK& m_deviceVK;
        std::vector<vk::raii::CommandBuffer> m_commandBuffers;
        // Caches the index passed into the begin method
        uint32_t m_currentFrameIndex = 0;
    };
}
