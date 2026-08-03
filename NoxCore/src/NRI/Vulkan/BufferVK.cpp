#include "BufferVK.h"
#include "DeviceVK.h"

namespace NRI
{
    BufferVK::BufferVK(DeviceVK& device, const BufferDesc& desc) : m_deviceVK(device)
    {
        vk::BufferUsageFlags2 usageFlags;
        vma::AllocationCreateFlags allocFlags{};
        vma::MemoryUsage memoryUsage = vma::MemoryUsage::eAuto;
        
        switch (desc.usage)
        {
        case BufferUsage::Staging:
            // CPU host-writes streaming, sequential transfers to GPU
            usageFlags = vk::BufferUsageFlagBits2::eTransferSrc | vk::BufferUsageFlagBits2::eTransferDst;
            memoryUsage = vma::MemoryUsage::eAuto;
            allocFlags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped;
            break;
        case BufferUsage::DescriptorHeap:
            // Direct Descriptor Buffer structures (VK_EXT_descriptor_buffer requires explicit BDA access)
            usageFlags = vk::BufferUsageFlagBits2::eDescriptorHeapEXT | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAuto;
            allocFlags = vma::AllocationCreateFlagBits::eHostAccessRandom | vma::AllocationCreateFlagBits::eMapped;
            break;
        case BufferUsage::Vertex:
            // GPU-only local vertex allocations (populated via staging copy commands)
            usageFlags = vk::BufferUsageFlagBits2::eVertexBuffer | vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            break;
        case BufferUsage::Index:
            // GPU-only local index allocations (populated via staging copy commands)
            usageFlags = vk::BufferUsageFlagBits2::eIndexBuffer | vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            break;
        case BufferUsage::Uniform:
            // UBO: CPU streaming writes, GPU reads via BDA/Descriptor rules
            usageFlags = vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            allocFlags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped;
            break;
        case BufferUsage::Storage:
            usageFlags = vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            allocFlags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped;
            break;
        case BufferUsage::StorageStatic:
            usageFlags = vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            break;
        case BufferUsage::Indirect:
             usageFlags = vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
             memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
             allocFlags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped;
             break;
        case BufferUsage::IndirectStatic:
            usageFlags = vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            break;
        }
        
        m_allocatedBuffer = m_deviceVK.getAllocator().createBuffer(desc.size, usageFlags, memoryUsage, allocFlags);
    }
    
    void* BufferVK::map(uint64_t offset, uint64_t size)
    {
        //  Check if VMA already persistently mapped this for us (due to eMapped)
        vma::AllocationInfo info = m_allocatedBuffer.buffer.getAllocation().getInfo();
        
        if (info.pMappedData)
        {
            return static_cast<uint8_t*>(info.pMappedData) + offset;
        }

        // Fallback fallback if eMapped wasn't provided for some reason
        return m_allocatedBuffer.buffer.getAllocation().map();
    }

    void BufferVK::unmap()
    {
        vma::AllocationInfo info = m_allocatedBuffer.buffer.getAllocation().getInfo();
        
        //  If VMA is managing a persistent mapping via flags, do NOT unmap it manually!
        // VMA will cleanly unmap it during the destruction of the underlying handle.
        if (info.pMappedData) 
        {
            return; 
        }

        m_allocatedBuffer.buffer.getAllocation().unmap();
    }
}
