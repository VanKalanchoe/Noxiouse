#include "SwapchainMTL.h"
#include "DeviceMTL.h"

namespace NRI
{
    SwapchainMTL::SwapchainMTL(DeviceMTL& device, const SwapchainDesc& desc) : m_deviceMTL(device)
    {
        m_layer = m_deviceMTL.getNativeMetalLayer();

            if (!m_layer)
                throw std::runtime_error(
                    "DeviceMTL has no CAMetalLayer");
        
        m_layer->setDisplaySyncEnabled(desc.VSync);

            m_swapChainExtent =
            {
                desc.Width,
                desc.Height
            };

            m_layer->setDrawableSize(
                CGSizeMake(
                    static_cast<CGFloat>(desc.Width),
                    static_cast<CGFloat>(desc.Height)
                )
            );
    }

SwapchainMTL::~SwapchainMTL()
{
    m_drawable = nullptr;
}

FrameResult SwapchainMTL::acquireNextImage(uint32_t frameIndex, uint32_t& outImageIndex)
{
    m_drawable = m_layer->nextDrawable();
    
    if(!m_drawable)
        return FrameResult::ResizeRequired;
    
    outImageIndex = 0;
    
    return FrameResult::Success;
}

FrameResult SwapchainMTL::present(uint32_t frameIndex, uint32_t imageIndex)
{
    if(!m_drawable)
        return FrameResult::ResizeRequired;
    
    m_drawable->present();
    
    m_drawable = nullptr;
    
    return FrameResult::Success;
}
}
