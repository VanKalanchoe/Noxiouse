#include "DescriptorHeapD3D12.h"
#include "DeviceD3D12.h"
#include "TextureD3D12.h"
#include "BufferD3D12.h"
#include "AccelerationStructureD3D12.h"

namespace NRI
{
    DescriptorHeapD3D12::DescriptorHeapD3D12(DeviceD3D12& device, const DescriptorHeapDesc& desc)
        : m_deviceD3D12(device), m_type(desc.type)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        if (m_type == DescriptorHeapType::Resource)
        {
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            m_numDescriptors = desc.maxImageDescriptors + desc.maxBufferDescriptors;
            if (m_numDescriptors == 0) m_numDescriptors = 65536;
        }
        else
        {
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            m_numDescriptors = std::max(desc.maxSamplerDescriptors, 2048u);
        }

        heapDesc.NumDescriptors = m_numDescriptors;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heapDesc.NodeMask = 0;

        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&m_heap)), "CreateDescriptorHeap");

        m_descriptorSize = m_deviceD3D12.getNativeDevice()->GetDescriptorHandleIncrementSize(heapDesc.Type);
        m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
        m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();

        // Create initial static samplers if Sampler heap
        if (m_type == DescriptorHeapType::Sampler && !desc.samplers.empty())
        {
            for (size_t i = 0; i < desc.samplers.size(); ++i)
            {
                const auto& s = desc.samplers[i];
                D3D12_SAMPLER_DESC sampDesc{};
                sampDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                sampDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                sampDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                sampDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                sampDesc.MinLOD = s.minLod;
                sampDesc.MaxLOD = s.maxLod;
                sampDesc.MaxAnisotropy = std::max(1u, static_cast<UINT>(s.maxAnisotropy));
                sampDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;

                D3D12_CPU_DESCRIPTOR_HANDLE handle = getCpuHandle(static_cast<uint32_t>(i));
                m_deviceD3D12.getNativeDevice()->CreateSampler(&sampDesc, handle);
            }
            m_nextFreeSlot = static_cast<uint32_t>(desc.samplers.size());
        }
    }

    uint32_t DescriptorHeapD3D12::allocateSlot()
    {
        std::scoped_lock lock(m_mutex);
        if (!m_freeSlots.empty())
        {
            uint32_t slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            return slot;
        }
        if (m_nextFreeSlot < m_numDescriptors)
        {
            return m_nextFreeSlot++;
        }
        throw std::runtime_error("Descriptor heap exhausted!");
    }

    void DescriptorHeapD3D12::freeSlot(uint32_t slot)
    {
        std::scoped_lock lock(m_mutex);
        m_freeSlots.push_back(slot);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapD3D12::getCpuHandle(uint32_t slot) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuStart;
        handle.ptr += static_cast<SIZE_T>(slot) * m_descriptorSize;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapD3D12::getGpuHandle(uint32_t slot) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = m_gpuStart;
        handle.ptr += static_cast<UINT64>(slot) * m_descriptorSize;
        return handle;
    }

    void DescriptorHeapD3D12::registerTexture(Texture& texture, TextureUsage usageOverride)
    {
        auto& texD3D12 = static_cast<TextureD3D12&>(texture);
        uint32_t slot = allocateSlot();

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = getCpuHandle(slot);
        TextureUsage effectiveUsage = (usageOverride != TextureUsage::Default) ? usageOverride : texD3D12.getUsage();

        if (effectiveUsage == TextureUsage::Storage)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = texD3D12.getDxgiFormat();
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = 0;
            m_deviceD3D12.getNativeDevice()->CreateUnorderedAccessView(texD3D12.getNativeResource(), nullptr, &uavDesc, cpuHandle);
        }
        else
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = texD3D12.getDxgiFormat();
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (texD3D12.getDesc().isCubeMap)
            {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = 0;
                srvDesc.TextureCube.MipLevels = texD3D12.getMipLevels();
            }
            else if (texD3D12.getArrayLayers() > 1)
            {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srvDesc.Texture2DArray.MostDetailedMip = 0;
                srvDesc.Texture2DArray.MipLevels = texD3D12.getMipLevels();
                srvDesc.Texture2DArray.FirstArraySlice = 0;
                srvDesc.Texture2DArray.ArraySize = texD3D12.getArrayLayers();
            }
            else
            {
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = texD3D12.getMipLevels();
            }
            m_deviceD3D12.getNativeDevice()->CreateShaderResourceView(texD3D12.getNativeResource(), &srvDesc, cpuHandle);
        }

        texD3D12.setDescriptorIndexSlot(slot);
    }

    uint32_t DescriptorHeapD3D12::registerStorageTextureMip(Texture& texture, uint32_t mipLevel)
    {
        auto& texD3D12 = static_cast<TextureD3D12&>(texture);
        uint32_t slot = allocateSlot();

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = getCpuHandle(slot);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = texD3D12.getDxgiFormat();
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = mipLevel;

        m_deviceD3D12.getNativeDevice()->CreateUnorderedAccessView(texD3D12.getNativeResource(), nullptr, &uavDesc, cpuHandle);
        return slot;
    }

    void DescriptorHeapD3D12::unregisterTexture(uint32_t slot)
    {
        if (slot != ~0u)
            freeSlot(slot);
    }

    uint32_t DescriptorHeapD3D12::registerBuffer(Buffer& buffer, uint64_t size)
    {
        auto& bufD3D12 = static_cast<BufferD3D12&>(buffer);
        uint32_t slot = allocateSlot();

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = getCpuHandle(slot);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = static_cast<UINT>(size / 4);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

        m_deviceD3D12.getNativeDevice()->CreateShaderResourceView(bufD3D12.getNativeBuffer(), &srvDesc, cpuHandle);
        return slot;
    }

    uint32_t DescriptorHeapD3D12::registerAccelerationStructure(AccelerationStructure& as, uint32_t slot)
    {
        auto& asD3D12 = static_cast<AccelerationStructureD3D12&>(as);
        uint32_t assignedSlot = (slot != ~0u) ? slot : allocateSlot();

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = getCpuHandle(assignedSlot);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.RaytracingAccelerationStructure.Location = asD3D12.getDeviceAddress();

        m_deviceD3D12.getNativeDevice()->CreateShaderResourceView(nullptr, &srvDesc, cpuHandle);
        return assignedSlot;
    }
}
