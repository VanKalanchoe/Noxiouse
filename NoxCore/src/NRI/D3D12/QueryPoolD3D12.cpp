#include "QueryPoolD3D12.h"
#include "DeviceD3D12.h"

namespace NRI
{
    QueryPoolD3D12::QueryPoolD3D12(DeviceD3D12& device, const QueryPoolDesc& desc)
        : m_deviceD3D12(device), m_capacity(desc.capacity), m_type(desc.type)
    {
        D3D12_QUERY_HEAP_DESC heapDesc{};
        heapDesc.Count = m_capacity;
        heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; // Default to timestamp/readback
        heapDesc.NodeMask = 0;

        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap)), "CreateQueryHeap");

        // Readback buffer
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Width = m_capacity * sizeof(uint64_t);
        resDesc.Height = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels = 1;
        resDesc.SampleDesc.Count = 1;
        resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_readbackBuffer)), "QueryPool Readback Buffer");
    }

    bool QueryPoolD3D12::getResults(uint32_t first, std::span<uint64_t> out) const
    {
        if (first + out.size() > m_capacity) return false;

        D3D12_RANGE readRange{ first * sizeof(uint64_t), (first + out.size()) * sizeof(uint64_t) };
        void* mappedData = nullptr;
        if (FAILED(m_readbackBuffer->Map(0, &readRange, &mappedData)))
            return false;

        const auto* src = static_cast<const uint64_t*>(mappedData) + first;
        std::copy_n(src, out.size(), out.data());

        D3D12_RANGE writeRange{ 0, 0 };
        m_readbackBuffer->Unmap(0, &writeRange);
        return true;
    }
}
