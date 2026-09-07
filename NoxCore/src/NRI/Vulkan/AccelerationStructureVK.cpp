#include "AccelerationStructureVK.h"
#include "BufferVK.h"
#include "DeviceVK.h"

namespace NRI
{
    AccelerationStructureVK::AccelerationStructureVK(DeviceVK& device, const AccelerationStructureDesc& desc)
        : m_deviceVK(device),
          m_type(desc.type),
          m_storageBuffer(desc.storageBuffer),
          m_bufferOffset(desc.bufferOffset),
          m_size(desc.size),
          m_handle(nullptr)
    {
        auto* bufferVK = static_cast<BufferVK*>(desc.storageBuffer);

        vk::AccelerationStructureCreateInfoKHR createInfo{
            .buffer = *bufferVK->getNativeBuffer(),
            .offset = desc.bufferOffset,
            .size   = desc.size,
            .type   = (desc.type == AccelerationStructureType::BottomLevel)
                ? vk::AccelerationStructureTypeKHR::eBottomLevel
                : vk::AccelerationStructureTypeKHR::eTopLevel
        };

        m_handle = m_deviceVK.getDevice().createAccelerationStructureKHR(createInfo);

        vk::AccelerationStructureDeviceAddressInfoKHR addressInfo{
            .accelerationStructure = *m_handle
        };
        m_deviceAddress = m_deviceVK.getDevice().getAccelerationStructureAddressKHR(addressInfo);
    }
}