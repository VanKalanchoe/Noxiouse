#pragma once
#include "../Buffer.h"
#include "MemoryAllocatorVK.h"

#include <vector>

namespace NRI
{
    class DeviceVK;
    
    class BufferVK final : public Buffer
    {
    public:
        BufferVK(DeviceVK& device, const BufferDesc& desc);
        ~BufferVK() override = default;
        
        void* map(uint64_t offset, uint64_t size) override;
        void unmap() override;
        
        uint64_t getDeviceAddress() const override { return m_allocatedBuffer.address; };
        uint64_t getSize() const override { return m_allocatedBuffer.size; };
        uint64_t getSparsePageSize() const override { return m_sparsePageSize; }
        
        vk::raii::Buffer& getNativeBuffer()
        {
            if (m_sparsePageSize != 0)
                return m_sparseBuffer;
            return m_allocatedBuffer.buffer;
        }

        // A sparse buffer's binds for DeviceVK::updateSparseBuffer: memory allocated for the committed pages that have none, the
        // released pages' memory moved to outRetired (freed once the unbind completed). False when memory ran out: the pages this
        // call allocated are given back and nothing is to be bound.
        bool prepareSparseBinds(std::span<const SparsePageRange> commits, std::span<const SparsePageRange> releases,
                                std::vector<vk::SparseMemoryBind>& outBinds, std::vector<vma::raii::Allocation>& outRetired);
        
    private:
        DeviceVK& m_deviceVK;
        AllocatedBuffer m_allocatedBuffer;
        // Sparse (BufferDesc::sparse): the buffer without memory of its own, and per page the memory bound to it (null: none).
        vk::raii::Buffer m_sparseBuffer = nullptr;
        vk::MemoryRequirements m_sparseRequirements{};
        uint64_t m_sparsePageSize = 0;
        std::vector<vma::raii::Allocation> m_sparsePages;
    };   
}
