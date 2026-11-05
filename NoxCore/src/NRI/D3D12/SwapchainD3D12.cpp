#include "SwapchainD3D12.h"
#include "DeviceD3D12.h"

namespace NRI
{
    SwapchainD3D12::SwapchainD3D12(DeviceD3D12& device, const SwapchainDesc& desc)
        : m_deviceD3D12(device), m_desc(desc), m_extent{ desc.Width, desc.Height }
    {
        createSwapChain();
    }

    void SwapchainD3D12::createSwapChain()
    {
        HWND hwnd = m_deviceD3D12.getWindowHandle();
        if (!hwnd)
        {
            throw std::runtime_error("SwapchainD3D12: HWND is null on DeviceD3D12");
        }

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
        swapChainDesc.Width = m_extent.width;
        swapChainDesc.Height = m_extent.height;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.Stereo = FALSE;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = m_bufferCount;
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        ComPtr<IDXGISwapChain1> swapChain1;
        ThrowIfFailed(m_deviceD3D12.getNativeFactory()->CreateSwapChainForHwnd(
            m_deviceD3D12.getGraphicsQueue(),
            hwnd,
            &swapChainDesc,
            nullptr,
            nullptr,
            &swapChain1
        ), "CreateSwapChainForHwnd");

        ThrowIfFailed(swapChain1.As(&m_swapChain), "Query IDXGISwapChain4");

        // Disable Alt+Enter fullscreen handling by DXGI (SDL handles it)
        m_deviceD3D12.getNativeFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        // Create Texture2D wrappers for each backbuffer
        m_backBuffers.resize(m_bufferCount);
        for (uint32_t i = 0; i < m_bufferCount; ++i)
        {
            ComPtr<ID3D12Resource> backBuffer;
            ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)), "Swapchain GetBuffer");

            TextureDesc texDesc{};
            texDesc.width = m_extent.width;
            texDesc.height = m_extent.height;
            texDesc.format = ImageFormat::RGBA8;
            texDesc.usage = TextureUsage::ColorAttachment;
            texDesc.mipLevels = 1;
            texDesc.arrayLayers = 1;

            m_backBuffers[i] = std::make_unique<TextureD3D12>(m_deviceD3D12, std::move(backBuffer), texDesc);
        }
    }

    FrameResult SwapchainD3D12::acquireNextImage(uint32_t frameIndex, uint32_t& outImageIndex)
    {
        outImageIndex = m_swapChain->GetCurrentBackBufferIndex();
        return FrameResult::Success;
    }

    FrameResult SwapchainD3D12::present(uint32_t frameIndex, uint32_t imageIndex)
    {
        UINT syncInterval = m_desc.VSync ? 1 : 0;
        UINT flags = (!m_desc.VSync) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        HRESULT hr = m_swapChain->Present(syncInterval, flags);

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            return FrameResult::ResizeRequired;
        }
        return FrameResult::Success;
    }
}
