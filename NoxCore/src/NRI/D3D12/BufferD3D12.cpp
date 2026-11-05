#include "BufferD3D12.h"
#include "DeviceD3D12.h"

namespace NRI
{
    BufferD3D12::BufferD3D12(DeviceD3D12& device, const BufferDesc& desc)
        : m_deviceD3D12(device), m_size(desc.size)
    {
        uint64_t alignedSize = (desc.size + 255) & ~255ull; // 256-byte alignment
        D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;

        bool isUpload = (desc.usage == BufferUsage::Staging ||
                         desc.usage == BufferUsage::Uniform ||
                         desc.usage == BufferUsage::DescriptorHeap ||
                         desc.usage == BufferUsage::AccelerationStructureInstance ||
                         desc.usage == BufferUsage::ShaderBindingTable);

        if (isUpload)
        {
            heapType = D3D12_HEAP_TYPE_UPLOAD;
            m_initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
        else
        {
            heapType = D3D12_HEAP_TYPE_DEFAULT;
            m_initialState = D3D12_RESOURCE_STATE_COMMON;

            if (desc.usage == BufferUsage::Storage ||
                desc.usage == BufferUsage::StorageStatic ||
                desc.usage == BufferUsage::AccelerationStructureScratch ||
                desc.usage == BufferUsage::AccelerationStructure)
            {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }

            if (desc.usage == BufferUsage::AccelerationStructure)
            {
                m_initialState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
            }
        }

        m_buffer = m_deviceD3D12.getAllocator()->createBuffer(alignedSize, flags, m_initialState, heapType);
        m_gpuAddress = m_buffer.gpuAddress;
    }

    BufferD3D12::~BufferD3D12()
    {
        if (m_isMapped && m_buffer.resource)
        {
            m_buffer.resource->Unmap(0, nullptr);
            m_isMapped = false;
            m_mappedPtr = nullptr;
        }
    }

    void* BufferD3D12::map(uint64_t offset, uint64_t size)
    {
        if (!m_isMapped && m_buffer.resource)
        {
            D3D12_RANGE readRange{ 0, 0 };
            ThrowIfFailed(m_buffer.resource->Map(0, &readRange, &m_mappedPtr), "Buffer Map");
            m_isMapped = true;
        }
        return static_cast<uint8_t*>(m_mappedPtr) + offset;
    }

    void BufferD3D12::unmap()
    {
        if (m_isMapped && m_buffer.resource)
        {
            m_buffer.resource->Unmap(0, nullptr);
            m_isMapped = false;
            m_mappedPtr = nullptr;
        }
    }
}
