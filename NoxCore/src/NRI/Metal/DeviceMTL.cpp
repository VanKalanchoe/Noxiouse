#include "DeviceMTL.h"
#include <iostream>

#include "imgui_impl_metal4.h"

#include "SwapchainMTL.h"
#include "CommandAllocatorMTL.h"
#include "CommandBufferMTL.h"

namespace NRI
{
    // Factory
    std::unique_ptr<Swapchain>
    DeviceMTL::createSwapchain(const SwapchainDesc& desc)
    {
        return std::make_unique<SwapchainMTL>(*this, desc);
    }

    std::unique_ptr<Pipeline>
    DeviceMTL::createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        return nullptr;
    }

    std::unique_ptr<CommandAllocator>
    DeviceMTL::createCommandAllocator()
    {
        return std::make_unique<CommandAllocatorMTL>(*this);
    }

    Nox::Ref<Texture2D>
    DeviceMTL::createTexture(const TextureDesc&)
    {
        return nullptr;
    }

    std::unique_ptr<Buffer>
    DeviceMTL::createBuffer(const BufferDesc&)
    {
        return nullptr;
    }

    std::unique_ptr<DescriptorHeap>
    DeviceMTL::createDescriptorHeap(const DescriptorHeapDesc&)
    {
        return nullptr;
    }

    DeviceMTL::DeviceMTL(Nox::Window& window)
    {
        m_Device = MTL::CreateSystemDefaultDevice();
        if(!m_Device)
        {
            throw std::runtime_error("Failed to create Metal device");
        }
        
        m_msaaSamples = getMaxUsableSampleCount();
        
        buildMetalLayer(window);
        
        m_CommandQueue =
                m_Device->newMTL4CommandQueue();

            if (!m_CommandQueue)
            {
                m_Device->release();
                m_Device = nullptr;

                throw std::runtime_error("Failed to create Metal 4 command queue");
            }

        buildResidencySet();

        m_CommandQueue->addResidencySet(m_ResidencySet);
        
        buildSharedEvent();
    }

    DeviceMTL::~DeviceMTL()
    {
        if(m_SharedEvent)
        {
            m_SharedEvent->release();
            m_SharedEvent = nullptr;
        }
        
        if (m_ResidencySet)
        {
            m_ResidencySet->release();
            m_ResidencySet = nullptr;
        }

        if (m_CommandQueue)
        {
            m_CommandQueue->release();
            m_CommandQueue = nullptr;
        }
        
        if(m_metalView)
        {
            SDL_Metal_DestroyView(m_metalView);
            
            m_metalView = nullptr;
            m_layer = nullptr;
        }

        if (m_Device)
        {
            m_Device->release();
            m_Device = nullptr;
        }
    }

    void DeviceMTL::shutdown()
    {
    }

    uint32_t DeviceMTL::getMaxUsableSampleCount()
    {
        if (m_Device->supportsTextureSampleCount(64))
            return 64;
        
        if (m_Device->supportsTextureSampleCount(32))
            return 32;
        
        if (m_Device->supportsTextureSampleCount(16))
            return 16;
        
        if (m_Device->supportsTextureSampleCount(8))
            return 8;

        if (m_Device->supportsTextureSampleCount(4))
            return 4;

        if (m_Device->supportsTextureSampleCount(2))
            return 2;

        return 1;
    }

    void DeviceMTL::submitAndWait(
        CommandBuffer& cmdBuffer,
        uint32_t slotIndex)
    {
        auto* mtlCmd = static_cast<CommandBufferMTL*>(&cmdBuffer);

        MTL4::CommandBuffer* commandBuffer =
            mtlCmd->getNativeBuffer(slotIndex);

        const uint64_t signalValue = m_NextSignalValue++;

        m_CommandQueue->commit(&commandBuffer, 1);

        m_CommandQueue->signalEvent(
            m_SharedEvent,
            signalValue
        );

        m_SharedEvent->waitUntilSignaledValue(
            signalValue,
            UINT64_MAX
        );

        m_LastSubmittedValue = signalValue;
    }

    void DeviceMTL::submitCommandBuffer
    (
        CommandBuffer& cmdBuffer,
        Swapchain& swapchain,
        uint32_t frameIndex,
        uint32_t imageIndex)
    {
        auto* mtlCmd = static_cast<CommandBufferMTL*>(&cmdBuffer);
        auto* mtlSwap = static_cast<SwapchainMTL*>(&swapchain);
        
        MTL4::CommandBuffer* commandBuffer = mtlCmd->getNativeBuffer(frameIndex);
        CA::MetalDrawable* drawable = mtlSwap->getNativeDrawable();
        
        m_CommandQueue->wait(drawable);
        
        const uint64_t signalValue = m_NextSignalValue++;
        
        m_CommandQueue->commit(&commandBuffer, 1);
        
        m_CommandQueue->signalEvent(m_SharedEvent, signalValue);

        m_LastSubmittedValue = signalValue;

        m_CommandQueue->signalDrawable(drawable);

        drawable->present();
    }

    void DeviceMTL::waitIdle()
    {
        if (!m_SharedEvent)
                return;

        if (m_LastSubmittedValue == 0)
            return;

        if (!m_SharedEvent->waitUntilSignaledValue(
                m_LastSubmittedValue,
                10000))
        {
            throw std::runtime_error(
                "Metal waitIdle timed out"
            );
        }
    }

    void DeviceMTL::initImGui(Nox::Window& window)
    {
        ImGui_ImplMetal4_Init(m_Device, m_CommandQueue, 2);
    }

    void DeviceMTL::shutdownImGui()
    {
        ImGui_ImplMetal4_Shutdown();
    }

    void DeviceMTL::beginImGui()
    {
        ImGui_ImplMetal4_NewFrame();
    }

    void DeviceMTL::endImGui()
    {
        // Update and Render additional Platform Windows
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

    void DeviceMTL::buildMetalLayer(Nox::Window& window)
    {
        m_metalView = SDL_Metal_CreateView(window.getHandle());
        if(!m_metalView)
        {
            m_Device->release();
            m_Device = nullptr;
            
            throw std::runtime_error(SDL_GetError());
        }
        
        m_layer = static_cast<CA::MetalLayer*>(SDL_Metal_GetLayer(m_metalView));
        if(!m_layer)
        {
            SDL_Metal_DestroyView(m_metalView);
            m_metalView = nullptr;
            
            m_Device->release();
            m_Device = nullptr;
            
            throw std::runtime_error("Failed to obtain CAMetalLayer");
        }
        
        m_layer->setDevice(m_Device);
        
        m_layer->setPixelFormat(MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB);
    }

    void DeviceMTL::buildResidencySet()
    {
        NS::Error* error = nullptr;

        MTL::ResidencySetDescriptor*
            descriptor =
                MTL::ResidencySetDescriptor
                    ::alloc()
                    ->init();

        m_ResidencySet =
            m_Device->newResidencySet(
                descriptor,
                &error
            );

        descriptor->release();

        if (!m_ResidencySet)
        {
            const char* message =
                error
                    ? error->localizedDescription()->utf8String()
                    : "Unknown error";

            if (error)
                error->release();

            throw std::runtime_error(message);
        }
    }

    void DeviceMTL::buildSharedEvent()
    {
        m_SharedEvent = m_Device->newSharedEvent();
        
        if(!m_SharedEvent)
        {
            m_CommandQueue->release();
            m_CommandQueue = nullptr;
            
            m_Device->release();
            m_Device = nullptr;
            
            throw std::runtime_error("Failed to create Metal shared event");
            
            m_SharedEvent->setSignaledValue(0);
            m_NextSignalValue = 0;
            m_LastSubmittedValue = 0;
        }
    }
}
