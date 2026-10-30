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
            usageFlags = vk::BufferUsageFlagBits2::eVertexBuffer | vk::BufferUsageFlagBits2::eTransferDst |
                vk::BufferUsageFlagBits2::eShaderDeviceAddress | vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            break;
        case BufferUsage::Index:
            // GPU-only local index allocations (populated via staging copy commands); TransferSrc for stream growth.
            usageFlags = vk::BufferUsageFlagBits2::eIndexBuffer | vk::BufferUsageFlagBits2::eTransferDst |
                vk::BufferUsageFlagBits2::eTransferSrc |
                vk::BufferUsageFlagBits2::eShaderDeviceAddress | vk::BufferUsageFlagBits2::eStorageBuffer |
                vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            break;
        case BufferUsage::Uniform:
            // UBO: CPU streaming writes, GPU reads via BDA/Descriptor rules
            usageFlags = vk::BufferUsageFlagBits2::eUniformBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            allocFlags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped;
            break;
        case BufferUsage::Storage:
            usageFlags = vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst |
                vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            allocFlags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped;
            break;
        case BufferUsage::StorageStatic:
            // TransferSrc: a geometry stream that grows copies itself into the larger buffer (GeometryArena).
            usageFlags = vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eTransferDst |
                vk::BufferUsageFlagBits2::eTransferSrc |
                vk::BufferUsageFlagBits2::eShaderDeviceAddress | vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            break;
        case BufferUsage::Indirect:
            // TransferSrc: GPU-written draw arguments/counts are read back for the stats.
            usageFlags = vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eTransferDst | vk::BufferUsageFlagBits2::eTransferSrc |
                vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            allocFlags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped;
            break;
        case BufferUsage::IndirectStatic:
            usageFlags = vk::BufferUsageFlagBits2::eIndirectBuffer | vk::BufferUsageFlagBits2::eTransferDst |
                vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            break;
        case BufferUsage::AccelerationStructure:
            // Storage buffer backing an actual VkAccelerationStructureKHR
            usageFlags = vk::BufferUsageFlagBits2::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits2::eShaderDeviceAddress |
                vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            break;
        case BufferUsage::AccelerationStructureScratch:
            // Scratch memory needed during AS build/update operations
            usageFlags = vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress;
            memoryUsage = vma::MemoryUsage::eAutoPreferDevice;
            break;
        case BufferUsage::AccelerationStructureInstance:
            // Array of VkAccelerationStructureInstanceKHR for TLAS building, CPU-updated per frame
            usageFlags = vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR | vk::BufferUsageFlagBits2::eShaderDeviceAddress |
                vk::BufferUsageFlagBits2::eTransferDst;
            memoryUsage = vma::MemoryUsage::eAuto;
            allocFlags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped;
            break;
        case BufferUsage::ShaderBindingTable:
            // Shader Binding Table (SBT) buffer: host-written, device-addressed for vkCmdTraceRaysKHR
            usageFlags = vk::BufferUsageFlagBits2::eShaderBindingTableKHR | vk::BufferUsageFlagBits2::eShaderDeviceAddress |
                vk::BufferUsageFlagBits2::eTransferDst;
            memoryUsage = vma::MemoryUsage::eAuto;
            allocFlags = vma::AllocationCreateFlagBits::eHostAccessSequentialWrite | vma::AllocationCreateFlagBits::eMapped;
            break;
        }

        const std::span<const uint32_t> queueFamilies =
            (desc.sharedAcrossQueues || m_deviceVK.hasAsyncComputeQueue()) ? m_deviceVK.getSharedQueueFamilies() : std::span<const uint32_t>{};
        if (desc.sparse && m_deviceVK.supportsSparseBuffers())
        {
            // Address space only: memory is bound page by page (prepareSparseBinds). Sparse residency lets the buffer be used while
            // only some pages are bound; only bound pages are ever accessed.
            const vk::BufferUsageFlags2CreateInfo usage2{ .usage = usageFlags };
            const vk::BufferCreateInfo bufferInfo{
                .pNext = &usage2,
                .flags = vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
                .size = desc.size,
                .sharingMode = queueFamilies.size() > 1 ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
                .queueFamilyIndexCount = queueFamilies.size() > 1 ? static_cast<uint32_t>(queueFamilies.size()) : 0u,
                .pQueueFamilyIndices = queueFamilies.size() > 1 ? queueFamilies.data() : nullptr
            };
            m_sparseBuffer = vk::raii::Buffer(m_deviceVK.getDevice(), bufferInfo);
            m_sparseRequirements = m_sparseBuffer.getMemoryRequirements();
            m_sparsePageSize = m_sparseRequirements.alignment;
            const uint64_t pages = (m_sparseRequirements.size + m_sparsePageSize - 1) / m_sparsePageSize;
            m_sparsePages.reserve(pages);
            for (uint64_t page = 0; page < pages; ++page)
                m_sparsePages.emplace_back(nullptr);
            m_allocatedBuffer.size = desc.size;
            if (usageFlags & vk::BufferUsageFlagBits2::eShaderDeviceAddress)
                m_allocatedBuffer.address = m_deviceVK.getDevice().getBufferAddress(vk::BufferDeviceAddressInfo{ .buffer = *m_sparseBuffer });
            return;
        }

        m_allocatedBuffer = m_deviceVK.getAllocator().createBuffer(desc.size, usageFlags, memoryUsage, allocFlags, {}, queueFamilies);
    }

    bool BufferVK::prepareSparseBinds(std::span<const SparsePageRange> commits, std::span<const SparsePageRange> releases,
                                      std::vector<vk::SparseMemoryBind>& outBinds, std::vector<vma::raii::Allocation>& outRetired)
    {
        if (m_sparsePageSize == 0)
            return false;
        const size_t firstBind = outBinds.size();
        std::vector<uint64_t> allocated;
        const vk::MemoryRequirements page{ .size = m_sparsePageSize, .alignment = m_sparsePageSize,
                                           .memoryTypeBits = m_sparseRequirements.memoryTypeBits };
        for (const SparsePageRange& range : commits)
        {
            for (uint64_t index = range.firstPage; index < range.firstPage + range.pageCount && index < m_sparsePages.size(); ++index)
            {
                if (*m_sparsePages[index])
                    continue;
                try
                {
                    m_sparsePages[index] = m_deviceVK.getAllocator().allocateSparsePage(page);
                }
                catch (const std::exception&)
                {
                    // Out of device memory: nothing of this call is bound.
                    for (uint64_t undo : allocated)
                        m_sparsePages[undo].clear();
                    outBinds.resize(firstBind);
                    return false;
                }
                allocated.push_back(index);
                const vma::AllocationInfo info = m_sparsePages[index].getInfo();
                outBinds.push_back(vk::SparseMemoryBind{ .resourceOffset = index * m_sparsePageSize, .size = m_sparsePageSize,
                                                         .memory = info.deviceMemory, .memoryOffset = info.offset });
            }
        }
        for (const SparsePageRange& range : releases)
        {
            for (uint64_t index = range.firstPage; index < range.firstPage + range.pageCount && index < m_sparsePages.size(); ++index)
            {
                if (!*m_sparsePages[index])
                    continue;
                outBinds.push_back(vk::SparseMemoryBind{ .resourceOffset = index * m_sparsePageSize, .size = m_sparsePageSize });
                outRetired.push_back(std::move(m_sparsePages[index]));
                m_sparsePages[index] = vma::raii::Allocation(nullptr);
            }
        }
        return true;
    }

    void* BufferVK::map(uint64_t offset, uint64_t size)
    {
        if (m_sparsePageSize != 0)
            return nullptr; // device local, never mappable
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
        if (m_sparsePageSize != 0)
            return;
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
