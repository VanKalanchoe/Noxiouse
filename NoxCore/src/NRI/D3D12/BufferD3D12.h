#pragma once
#include "D3D12Common.h"
#include "../Buffer.h"
#include "MemoryAllocatorD3D12.h"

namespace NRI
{
    class DeviceD3D12;

    class BufferD3D12 final : public Buffer
    {
    public:
        BufferD3D12(DeviceD3D12& device, const BufferDesc& desc);
        ~BufferD3D12() override;

        void* map(uint64_t offset, uint64_t size) override;
        void unmap() override;

        uint64_t getDeviceAddress() const override { return m_gpuAddress; }
        uint64_t getSize() const override { return m_size; }
        uint64_t getSparsePageSize() const override { return 0; }

        ID3D12Resource* getNativeBuffer() const { return m_buffer.resource.Get(); }
        D3D12_RESOURCE_STATES getInitialState() const { return m_initialState; }

    private:
        DeviceD3D12& m_deviceD3D12;
        AllocatedBufferD3D12 m_buffer;
        uint64_t m_size = 0;
        uint64_t m_gpuAddress = 0;
        void* m_mappedPtr = nullptr;
        bool m_isMapped = false;
        D3D12_RESOURCE_STATES m_initialState = D3D12_RESOURCE_STATE_COMMON;
    };
}
