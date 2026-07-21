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
        void setViewportWithCount(const ViewportBounds& bounds, float minDepth, float maxDepth) override;
        void setScissorWithCount(const Extent2D& swapChainExtent) override;
        void setVertexInput() override;
        void setPrimitiveTopology(const PrimitiveTopology& topology) override;
        void setPrimitiveRestartEnable(bool enable) override;
        void setRasterizerDiscardEnable(bool enable) override;
        void setPolygonMode(const PolygonMode& polygon)override;
        void setCullMode(const CullMode& cullMode) override;
        void setFrontFace(const FrontFace& frontFace) override;
        void setDepthBiasEnable(bool enable) override;
        void setDepthClampEnable(bool enable) override;
        void setRasterizationSamples(uint32_t sampleCount) override;
        void setSampleMask(uint32_t sampleCount, uint32_t sampleMask) override;
        void setAlphaToCoverageEnable(bool enable) override;
        void setAlphaToOneEnableEXT(bool enable) override;
        void setDepthTestEnable(bool enable) override;
        void setDepthWriteEnable(bool enable) override;
        void setDepthCompareOp(const CompareOp& compareOp) override;
        void setDepthBoundsTestEnable(bool enable) override;
        void setStencilTestEnable(bool enable) override;
        void setColorBlendEnable(uint32_t firstAttachment, bool enable) override;
        void setColorBlendEquation(uint32_t firstAttachment, const ColorBlendEquation& blendEquation) override;
        void setColorWriteMask(uint32_t firstAttachment, uint32_t colorWriteMask) override;
        void setLogicOpEnable(bool enable) override;
        void bindDescriptorHeaps(DescriptorHeap* resourceHeap, DescriptorHeap* samplerHeap) override;
        void pushData(const void* data, uint32_t size) override;
        void bindVertexBuffers(uint32_t firstBinding, Buffer& buffer, uint64_t offset) override;
        void bindIndexBuffer(Buffer& buffer, uint64_t offet) override;
        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
        void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
        void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;

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
