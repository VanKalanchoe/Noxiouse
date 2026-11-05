#pragma once
#include <vector>
#include <span>
#include "D3D12Common.h"
#include "../CommandBuffer.h"
#include "../AccelerationStructure.h"

namespace NRI
{
    class DeviceD3D12;
    class CommandAllocatorD3D12;

    class CommandBufferD3D12 final : public CommandBuffer
    {
    public:
        CommandBufferD3D12(DeviceD3D12& device, CommandAllocatorD3D12& allocator);
        ~CommandBufferD3D12() override = default;

        ID3D12GraphicsCommandList6* getNativeCommandList() const { return m_cmdList.Get(); }

        void begin(uint32_t index = 0, bool oneTimeSubmit = false) override;
        void end(uint32_t index = 0) override;
        void beginRendering(RenderDesc& desc) override;
        void endRendering() override;
        void renderImGui() override;
        void bindPipeline(PipelineBindPoint bindPoint, Pipeline& pipeline) override;

        void setViewport(Extent2D swapChainExtent) override;
        void setScissor(Extent2D swapChainExtent) override;
        void setViewportWithCount(const ViewportBounds& bounds, float minDepth, float maxDepth) override;
        void setScissorWithCount(const Extent2D& swapChainExtent) override;
        void setVertexInput() override {}
        void setPrimitiveTopology(const PrimitiveTopology& topology) override;
        void setPrimitiveRestartEnable(bool enable) override {}
        void setRasterizerDiscardEnable(bool enable) override {}
        void setPolygonMode(const PolygonMode& polygon) override {}
        void setCullMode(const CullMode& cullMode) override {}
        void setFrontFace(const FrontFace& frontFace) override {}
        void setDepthBiasEnable(bool enable) override {}
        // D3D12 keeps depth bias in the pipeline's rasterizer state (ID3D12GraphicsCommandList9::RSSetDepthBias is the dynamic form);
        // like setDepthBiasEnable above, not wired yet: the shadow map passes draw without the raster slope bias here.
        void setDepthBias(float constantFactor, float clamp, float slopeFactor) override {}
        void setDepthClampEnable(bool enable) override {}
        void setLineWidth(float lineWidth) override {}
        void setRasterizationSamples(uint32_t sampleCount) override {}
        void setSampleMask(uint32_t sampleCount, uint32_t sampleMask) override {}
        void setAlphaToCoverageEnable(bool enable) override {}
        void setAlphaToOneEnableEXT(bool enable) override {}
        void setDepthTestEnable(bool enable) override {}
        void setDepthWriteEnable(bool enable) override {}
        void setDepthCompareOp(const CompareOp& compareOp) override {}
        void setDepthBoundsTestEnable(bool enable) override {}
        void setStencilTestEnable(bool enable) override {}
        void setColorBlendEnable(uint32_t firstAttachment, bool enable) override {}
        void setColorBlendEquation(uint32_t firstAttachment, const ColorBlendEquation& blendEquation) override {}
        void setColorWriteMask(uint32_t firstAttachment, uint32_t colorWriteMask) override {}
        void setLogicOpEnable(bool enable) override {}

        void bindDescriptorHeaps(DescriptorHeap* resourceHeap, DescriptorHeap* samplerHeap) override;
        void pushData(const void* data, uint32_t size) override;
        void bindVertexBuffers(uint32_t firstBinding, Buffer& buffer, uint64_t offset) override;
        void bindIndexBuffer(Buffer& buffer, uint64_t offet) override;
        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
        void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
        void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
        void drawMeshTasksIndirect(Buffer& indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) override;
        void drawMeshTasksIndirect(uint64_t indirectBufferDeviceAddress, uint64_t offset, uint32_t drawCount, uint32_t stride) override;
        void drawMeshTasksIndirectCount(Buffer& indirectBuffer, uint64_t offset, Buffer& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride) override;
        void dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
        void traceRays(const StridedDeviceAddressRegion& rayGen, const StridedDeviceAddressRegion& miss,
                       const StridedDeviceAddressRegion& hitGroup, const StridedDeviceAddressRegion& callable,
                       uint32_t width, uint32_t height, uint32_t depth = 1) override;

        void copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, const BufferCopyRegion& region = {}) override;
        void fillBuffer(Buffer& buffer, uint64_t offset, uint64_t size, uint32_t value) override;
        void copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, std::span<const BufferCopyRegion> regions) override;

        void transitionTextureLayout(Texture& texture, TextureLayout oldLayout, TextureLayout newLayout) override;
        void transitionSwapchainLayout(Swapchain& swapchain, uint32_t imageIndex, TextureLayout oldLayout, TextureLayout newLayout) override;
        void resolveImage(Texture& srcTexture, Texture& dstTexture, uint32_t width, uint32_t height) override;
        void copyTexture(Texture& srcTexture, Texture& dstTexture, uint32_t width, uint32_t height) override;
        void copyTextureMips(Texture& srcTexture, uint32_t srcFirstMip, Texture& dstTexture, uint32_t dstFirstMip, uint32_t mipCount) override;

        void buildAccelerationStructure(const AccelerationStructureBuildDesc& buildDesc, uint64_t scratchAddress, AccelerationStructure& dstAS) override;
        void updateAccelerationStructure(const AccelerationStructureBuildDesc& buildDesc, uint64_t scratchAddress, AccelerationStructure& srcAS, AccelerationStructure& dstAS) override;
        void accelerationStructureBarrier(AccelerationStructureBarrierType barrierType = AccelerationStructureBarrierType::BuildToShaderRead) override;

        void resetQueries(QueryPool& pool, uint32_t first, uint32_t count) override;
        void writeCompactedSizes(std::span<AccelerationStructure* const> structures, QueryPool& pool, uint32_t firstQuery) override;
        void copyAccelerationStructure(AccelerationStructure& src, AccelerationStructure& dst, bool compact) override;

        void executionBarrier() override;
        void transferBarrier() override;
        void resourceBarriers(std::span<const TextureBarrierDesc> textures, std::span<const BufferBarrierDesc> buffers) override;

        void beginDebugLabel(const char* label) override;
        void endDebugLabel() override;

    private:
        DeviceD3D12& m_deviceD3D12;
        CommandAllocatorD3D12& m_allocator;
        ComPtr<ID3D12GraphicsCommandList6> m_cmdList;
        bool m_isRecording = false;

        // Temporary RTV/DSV descriptor storage for dynamic rendering
        ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
        uint32_t m_rtvDescriptorSize = 0;
        uint32_t m_dsvDescriptorSize = 0;
    };
}
