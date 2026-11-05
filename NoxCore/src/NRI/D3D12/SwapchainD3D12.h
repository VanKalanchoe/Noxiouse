#pragma once
#include <vector>
#include "D3D12Common.h"
#include "../Swapchain.h"
#include "TextureD3D12.h"

namespace NRI
{
    class DeviceD3D12;

    class SwapchainD3D12 final : public Swapchain
    {
    public:
        SwapchainD3D12(DeviceD3D12& device, const SwapchainDesc& desc);
        ~SwapchainD3D12() override = default;

        Extent2D getExtent() const override { return m_extent; }
        FrameResult acquireNextImage(uint32_t frameIndex, uint32_t& outImageIndex) override;
        FrameResult present(uint32_t frameIndex, uint32_t imageIndex) override;

        IDXGISwapChain4* getNativeSwapchain() const { return m_swapChain.Get(); }
        TextureD3D12* getBackBuffer(uint32_t index) const { return m_backBuffers[index].get(); }

    private:
        void createSwapChain();

    private:
        DeviceD3D12& m_deviceD3D12;
        SwapchainDesc m_desc;
        Extent2D m_extent{};
        ComPtr<IDXGISwapChain4> m_swapChain;
        std::vector<std::unique_ptr<TextureD3D12>> m_backBuffers;
        uint32_t m_bufferCount = 3;
    };
}
