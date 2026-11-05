#pragma once
#include "D3D12Common.h"
#include "../AccelerationStructure.h"

namespace NRI
{
    class DeviceD3D12;

    class AccelerationStructureD3D12 final : public AccelerationStructure
    {
    public:
        AccelerationStructureD3D12(DeviceD3D12& device, const AccelerationStructureDesc& desc);
        ~AccelerationStructureD3D12() override = default;

        uint64_t getDeviceAddress() const override { return m_deviceAddress; }
        uint64_t getSize() const override { return m_size; }
        AccelerationStructureType getType() const override { return m_type; }
        Buffer* getBuffer() const override { return m_storageBuffer; }
        DeviceD3D12& getDevice() const { return m_deviceD3D12; }

    private:
        DeviceD3D12& m_deviceD3D12;
        AccelerationStructureType m_type;
        Buffer* m_storageBuffer = nullptr;
        uint64_t m_size = 0;
        uint64_t m_deviceAddress = 0;
    };
}
