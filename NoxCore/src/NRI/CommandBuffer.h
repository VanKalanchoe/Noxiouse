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
