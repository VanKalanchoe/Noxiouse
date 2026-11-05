#pragma once
#include <span>
#include "D3D12Common.h"
#include "../QueryPool.h"

namespace NRI
{
    class DeviceD3D12;

    class QueryPoolD3D12 final : public QueryPool
    {
    public:
        QueryPoolD3D12(DeviceD3D12& device, const QueryPoolDesc& desc);
        ~QueryPoolD3D12() override = default;

        uint32_t getCapacity() const override { return m_capacity; }
        bool getResults(uint32_t first, std::span<uint64_t> out) const override;

        ID3D12QueryHeap* getNativeHeap() const { return m_queryHeap.Get(); }
        ID3D12Resource* getReadbackBuffer() const { return m_readbackBuffer.Get(); }

    private:
        DeviceD3D12& m_deviceD3D12;
        uint32_t m_capacity = 0;
        QueryType m_type;
        ComPtr<ID3D12QueryHeap> m_queryHeap;
        ComPtr<ID3D12Resource> m_readbackBuffer;
    };
}
