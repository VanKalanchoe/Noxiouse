#pragma once
#include <memory>

#include "Swapchain.h"
#include "Pipeline.h"
#include "CommandAllocator.h"
#include "CommandBuffer.h"
#include "Texture.h"
#include "Buffer.h"
#include "DescriptorHeap.h"
#include "ShaderCompiler.h"
#include "NoxCore/Core/Window.h"

namespace NRI
{
    enum class GraphicsAPI : uint8_t
    {
        Vulkan,
        Metal
    };
    
    class Device
    {
    public:
        // Factory function: The ONLY place that knows about specific backends
        static std::unique_ptr<Device> create(GraphicsAPI api, Nox::Window& window);
        
        virtual std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) = 0;
        virtual std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler) = 0;
        virtual std::unique_ptr<CommandAllocator> createCommandAllocator() = 0;
        virtual Nox::Ref<Texture2D> createTexture(const TextureDesc& desc) = 0;
        virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) = 0;
        virtual std::unique_ptr<DescriptorHeap> createDescriptorHeap(const DescriptorHeapDesc& desc) = 0;
        
        virtual void shutdown() = 0;
        virtual uint32_t getMSAASampleCount() const = 0;
        virtual void submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex = 0) = 0;
        virtual void submitCommandBuffer(CommandBuffer& cmdBuffer, Swapchain& swapchain, uint32_t frameIndex, uint32_t imageIndex) = 0;
        virtual void waitIdle() = 0;
        virtual void initImGui(Nox::Window& window) = 0;
        virtual void shutdownImGui() = 0;
        virtual void beginImGui() = 0;
        virtual void endImGui() = 0;
        
        virtual ~Device() = default;
    };
}
