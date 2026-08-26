#pragma once

#include "MetalCommon.h"
#include "../Swapchain.h"
#include "../NRITypes.h"

namespace NRI
{
    class DeviceMTL; // Forward declaration

    class SwapchainMTL final : public Swapchain
    {
    public:
        SwapchainMTL(DeviceMTL& device, const SwapchainDesc& desc);
        ~SwapchainMTL() override;
        
        Extent2D getExtent() const override { return m_swapChainExtent; }
        FrameResult acquireNextImage(uint32_t frameIndex, uint32_t& imageIndex) override;
        FrameResult present(uint32_t frameIndex, uint32_t imageIndex) override;
        CA::MetalDrawable* getNativeDrawable() const { return m_drawable; }
        MTL::Texture* getNativeTexture() const { return m_drawable ? m_drawable->texture() : nullptr; }
        
    private:
        DeviceMTL& m_deviceMTL;
        CA::MetalLayer* m_layer = nullptr;
        CA::MetalDrawable* m_drawable = nullptr;
        Extent2D m_swapChainExtent;
        
        const size_t MAX_FRAMES_IN_FLIGHT = 2; // Match your configuration
    };
}
