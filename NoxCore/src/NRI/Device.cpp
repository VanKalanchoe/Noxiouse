#include "Device.h"
#include <stdexcept>
// Include backend implementations explicitly here in the .cpp only!
#if defined(_WIN32)
    #include "Vulkan/DeviceVK.h"
#elif defined(__APPLE__)
    #include "Metal/DeviceMTL.h"
#endif

namespace NRI
{
    std::unique_ptr<Device> Device::create(GraphicsAPI api, Nox::Window& window)
    {
        switch (api)
        {
        case GraphicsAPI::Vulkan:
#if defined(_WIN32)
            return std::make_unique<DeviceVK>(window);
#else
            throw std::runtime_error("Vulkan backend unavailable on this platform.");
#endif
                
        case GraphicsAPI::Metal:
#if defined(__APPLE__)
            return std::make_unique<DeviceMTL>(window);
#else
            throw std::runtime_error("Metal backend unavailable on this platform.");
#endif
        case GraphicsAPI::None:
            // return std::make_unique<mtl::DeviceMTL>(windowHandle);
            throw std::runtime_error("None backend not implemented yet!");
        }
        return nullptr;
    }
}
