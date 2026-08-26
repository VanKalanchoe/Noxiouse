#pragma once
#include "MetalCommon.h"

#include <SDL3/SDL_metal.h>
#include "../Device.h"

namespace NRI
{

    class ShaderCompiler;
    class DeviceMTL final : public Device
    {
    public:
        DeviceMTL(Nox::Window& window);
        ~DeviceMTL() override;
        void shutdown() override;
        
        uint32_t getMSAASampleCount() { return m_msaaSamples; }

        void submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex = 0) override;

        void submitCommandBuffer(
            CommandBuffer& cmdBuffer,
            Swapchain& swapchain,
            uint32_t frameIndex,
            uint32_t imageIndex) override;

        void waitIdle() override;

        void initImGui(Nox::Window& window) override;
        void shutdownImGui() override;
        void beginImGui() override;
        void endImGui() override;
        
        MTL::Device* getDevice() { return m_Device; }
        SDL_MetalView getNativeMetalView() const { return m_metalView; }
        CA::MetalLayer* getNativeMetalLayer() const { return m_layer; }
        MTL::ResidencySet* getResidencySet() { return m_ResidencySet; }
        
        // Factory
        std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) override;
        std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler) override;
        std::unique_ptr<CommandAllocator> createCommandAllocator() override;
        Nox::Ref<Texture2D> createTexture(const TextureDesc& desc) override;
        std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override;
        std::unique_ptr<DescriptorHeap> createDescriptorHeap(const DescriptorHeapDesc& desc) override;
        
    private:
        uint32_t getMaxUsableSampleCount();
        void buildMetalLayer(Nox::Window& window);
        void buildResidencySet();
        void buildSharedEvent();
        
    private:
        MTL::Device* m_Device = nullptr;
        uint32_t m_msaaSamples = 1;
        MTL4::CommandQueue* m_CommandQueue = nullptr;
        MTL::ResidencySet* m_ResidencySet = nullptr;
        MTL::SharedEvent* m_SharedEvent = nullptr;
        
        SDL_MetalView m_metalView = nullptr;
        CA::MetalLayer* m_layer = nullptr;

        uint64_t m_NextSignalValue = 0;
        uint64_t m_LastSubmittedValue = 0;
    };
}
