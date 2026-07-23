#include "Device.h"
#include <stdexcept>
// Include backend implementations explicitly here in the .cpp only!
#include "Vulkan/DeviceVK.h"

namespace NRI
{
    std::unique_ptr<Device> Device::create(GraphicsAPI api, Nox::Window& window)
    {
        switch (api)
        {
        case GraphicsAPI::Vulkan:
            return std::make_unique<DeviceVK>(window);
            
        case GraphicsAPI::Metal:
            // return std::make_unique<mtl::DeviceMTL>(windowHandle);
            throw std::runtime_error("Metal backend not implemented yet!");
        }
        return nullptr;
    }
}
