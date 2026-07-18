#pragma once

#include "VulkanCommon.h"
#include "../Swapchain.h"
#include "../NRITypes.h"
#include "TextureVK.h"

namespace NRI
{
    class DeviceVK; // Forward declaration
    
    class SwapchainVK final : public Swapchain
    {
    public:
        SwapchainVK(DeviceVK& device, const SwapchainDesc& desc);
        ~SwapchainVK() = default;
        
        Extent2D getExtent() const override { return { m_swapChainExtent.width, m_swapChainExtent.height }; };
        vk::Image getNativeImage(uint32_t index) const { return m_swapChainImages[index]; }
        vk::ImageView getNativeView(uint32_t index) const { return m_swapChainImageViews[index]; }
        FrameResult acquireNextImage(uint32_t frameIndex, uint32_t& outImageIndex) override;
        FrameResult present(uint32_t frameIndex, uint32_t imageIndex) override;
        const vk::raii::Semaphore& getPresentCompleteSemaphore(uint32_t frameIndex) const { return m_presentCompleteSemaphores[frameIndex]; }
        const vk::raii::Semaphore& getRenderFinishedSemaphore(uint32_t imageIndex) const { return m_renderFinishedSemaphores[imageIndex]; }
        const vk::raii::Fence& getInFlightFence(uint32_t frameIndex) const { return m_inFlightFences[frameIndex]; }
        
    private:
        void createSwapChain(const SwapchainDesc& desc);
        void createImageViews();
        vk::PresentModeKHR chooseSwapPresentMode(std::vector<vk::PresentModeKHR> const& availablePresentModes, bool vSync);
        vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats);
        uint32_t chooseSwapMinImageCount(vk::SurfaceCapabilitiesKHR const& surfaceCapabilities);
        vk::Extent2D chooseSwapExtent(vk::SurfaceCapabilitiesKHR const& capabilities, uint32_t windowWidth, uint32_t windowHeight);
        void createSyncObjects();

    private:
        DeviceVK& m_deviceVK;
        vk::raii::SwapchainKHR m_swapChain = nullptr;
        std::vector<vk::Image> m_swapChainImages;
        vk::SurfaceFormatKHR m_swapChainSurfaceFormat;
        vk::Extent2D m_swapChainExtent;
        std::vector<vk::raii::ImageView> m_swapChainImageViews;
        
        std::vector<vk::raii::Semaphore> m_presentCompleteSemaphores;
        std::vector<vk::raii::Semaphore> m_renderFinishedSemaphores;
        std::vector<vk::raii::Fence> m_inFlightFences;
        
        const size_t MAX_FRAMES_IN_FLIGHT = 2; // Match your configuration
    };
}
