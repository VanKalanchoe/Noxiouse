#include "Device.h"
#include <stdexcept>
// Include backend implementations explicitly here in the .cpp only!
#include "Vulkan/DeviceVK.h"
#ifdef _WIN32
#include "D3D12/DeviceD3D12.h"
#endif

namespace NRI
{
    std::unique_ptr<Device> Device::create(GraphicsAPI api, Nox::Window& window)
    {
        switch (api)
        {
        case GraphicsAPI::Vulkan:
            return std::make_unique<DeviceVK>(window);
#ifdef _WIN32
        case GraphicsAPI::D3D12:
            return std::make_unique<DeviceD3D12>(window);
#endif
            
        case GraphicsAPI::Metal:
            // return std::make_unique<mtl::DeviceMTL>(windowHandle);
            throw std::runtime_error("Metal backend not implemented yet!");
        }
        return nullptr;
    }
}
