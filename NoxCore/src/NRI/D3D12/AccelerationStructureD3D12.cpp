#include "AccelerationStructureD3D12.h"
#include "BufferD3D12.h"
#include "DeviceD3D12.h"

namespace NRI
{
    AccelerationStructureD3D12::AccelerationStructureD3D12(DeviceD3D12& device, const AccelerationStructureDesc& desc)
        : m_deviceD3D12(device), m_type(desc.type), m_storageBuffer(desc.storageBuffer), m_size(desc.size)
    {
        if (m_storageBuffer)
        {
            m_deviceAddress = m_storageBuffer->getDeviceAddress() + desc.bufferOffset;
        }
    }
}
