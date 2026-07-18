#pragma once

#include "NRITypes.h"

namespace NRI
{
    class Texture; // Forward declaration
    
    struct SwapchainDesc
    {
        uint32_t Width;
        uint32_t Height;
        bool VSync = true;
    };
    
    enum class FrameResult { Success, ResizeRequired };
    
    class Swapchain
    {
    public:
        virtual ~Swapchain() = default;
        virtual Extent2D getExtent() const = 0;
        
        // Returns the image index to render into, or an indicator if resizing is needed
        virtual FrameResult acquireNextImage(uint32_t frameIndex, uint32_t& outImageIndex) = 0;
        
        // Pushes the finished image to the screen
        virtual FrameResult present(uint32_t frameIndex, uint32_t imageIndex) = 0;
    };
}
