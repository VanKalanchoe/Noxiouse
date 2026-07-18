#pragma once

#include "NRITypes.h"

namespace NRI
{
    class Pipeline; // Forward declaration
    class Texture;
    class Swapchain;
    class DescriptorHeap;
    class Buffer;
    
    enum class TextureLayout : uint8_t;

    enum class LoadOP{ load, clear, dontCare, none };
    enum class StoreOP{ store, dontCare, none };
    struct ClearColor{ float r, g, b, a; };
    struct ClearDepth{ float depth; uint32_t stencil; };
    
    struct RenderAttachDesc
    {
        Swapchain* attachmentSwapchain;
        Texture* attachment;
        Texture* resolve;
        Swapchain* resolveSwapchain;
        uint32_t   resolveImageIndex = 0;
        LoadOP loadOP;
        StoreOP storeOP;
        ClearColor clearColor;
        ClearDepth clearDepth;
    };
    
    struct RenderDesc
    {
        Extent2D renderArea;
        std::vector<RenderAttachDesc> colorAttachments;
        RenderAttachDesc depthAttachment;
        bool isSwapchainPass = false;
    };
    
    enum class PipelineBindPoint
    {
        Graphics,
        Compute
    };
    
    enum class PrimitiveTopology
    {
        TriangleList,
    };
    
    enum class PolygonMode
    {
        Fill
    };
    
    enum class CullMode
    {
        None,
        Back
    };

    enum class FrontFace
    {
        CounterClockWise
    };
    
    enum class CompareOp
    {
        Less
    };
    
    enum class BlendFactor
    {
        SrcAlpha,
        OneMinusSrcAlpha
    };
    
    enum class BlendOp
    {
        Add
    };
    
    struct  ColorBlendEquation
    {
        BlendFactor srcColorBlendFactor;
        BlendFactor dstColorBlendFactor;
        BlendOp     colorBlendOp;
        BlendFactor srcAlphaBlendFactor;
        BlendFactor dstAlphaBlendFactor;
        BlendOp     alphaBlendOp;
    };
    
    namespace ColorComponent 
    {
        constexpr uint32_t R = 0x00000001;
        constexpr uint32_t G = 0x00000002;
        constexpr uint32_t B = 0x00000004;
        constexpr uint32_t A = 0x00000008;
        constexpr uint32_t All = R | G | B | A;
    }
    
    class CommandBuffer
    {
    public:
        virtual ~CommandBuffer() = default;
        
        virtual void begin(uint32_t index = 0, bool oneTimeSubmit = false) = 0;
        virtual void end(uint32_t index = 0) = 0;
        virtual void beginRendering(RenderDesc& desc) = 0;
        virtual void endRendering() = 0;
        virtual void renderImGui() = 0;
        virtual void bindPipeline(PipelineBindPoint bindPoint, Pipeline& pipeline) = 0;
        
        virtual void setViewport(Extent2D swapChainExtent) = 0;
        virtual void setScissor(Extent2D swapChainExtent) = 0;
        virtual void setViewportWithCount(const Extent2D& swapChainExtent) = 0;
        virtual void setScissorWithCount(const Extent2D& swapChainExtent) = 0;
        virtual void setVertexInput() = 0;
        virtual void setPrimitiveTopology(const PrimitiveTopology& topology) = 0;
        virtual void setPrimitiveRestartEnable(bool enable) = 0;
        virtual void setRasterizerDiscardEnable(bool enable) = 0;
        virtual void setPolygonMode(const PolygonMode& polygon) = 0;
        virtual void setCullMode(const CullMode& cullMode) = 0;
        virtual void setFrontFace(const FrontFace& frontFace) = 0;
        virtual void setDepthBiasEnable(bool enable) = 0;
        virtual void setDepthClampEnable(bool enable) = 0;
        virtual void setRasterizationSamples(uint32_t sampleCount) = 0;
        virtual void setSampleMask(uint32_t sampleCount, uint32_t sampleMask) = 0;
        virtual void setAlphaToCoverageEnable(bool enable) = 0;
        virtual void setAlphaToOneEnableEXT(bool enable) = 0;
        virtual void setDepthTestEnable(bool enable) = 0;
        virtual void setDepthWriteEnable(bool enable) = 0;
        virtual void setDepthCompareOp(const CompareOp& compareOp) = 0;
        virtual void setDepthBoundsTestEnable(bool enable) = 0;
        virtual void setStencilTestEnable(bool enable) = 0;
        virtual void setColorBlendEnable(uint32_t firstAttachment, bool enable) = 0;
        virtual void setColorBlendEquation(uint32_t firstAttachment, const ColorBlendEquation& blendEquation) = 0;
        virtual void setColorWriteMask(uint32_t firstAttachment, uint32_t colorWriteMask) = 0;
        virtual void setLogicOpEnable(bool enable) = 0;
        
        virtual void bindDescriptorHeaps(DescriptorHeap* resourceHeap, DescriptorHeap* samplerHeap) = 0;
        virtual void pushData(const void* data, uint32_t size) = 0;
        virtual void bindVertexBuffers(uint32_t firstBinding, Buffer& buffer, uint64_t offset) = 0;
        virtual void bindIndexBuffer(Buffer& buffer, uint64_t offet) = 0;
        virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) = 0;
        
        virtual void copyBuffer(class Buffer& srcBuffer, class Buffer& dstBuffer, uint64_t deviceSize) = 0;
        
        virtual void transitionTextureLayout(Texture& texture, TextureLayout oldLayout, TextureLayout newLayout) = 0;
        virtual void transitionSwapchainLayout(Swapchain& swapchain, uint32_t imageIndex, TextureLayout oldLayout, TextureLayout newLayout) = 0;
    };
}
