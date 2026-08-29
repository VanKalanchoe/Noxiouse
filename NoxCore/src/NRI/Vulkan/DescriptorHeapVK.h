#pragma once
#include "../DescriptorHeap.h"
#include "VulkanCommon.h"
#include <memory>

#include "BufferVK.h"

namespace NRI
{
    class DeviceVK;
    class Buffer; // Forward declaration

    class DescriptorHeapVK final : public DescriptorHeap
    {
    public:
        DescriptorHeapVK(DeviceVK& device, const DescriptorHeapDesc& desc);
        ~DescriptorHeapVK() override = default;

        void registerTexture(Texture& texture, TextureUsage usageOverride) override;
        void unregisterTexture(uint32_t slot) override;
        uint64_t getBufferDeviceAddress(vk::raii::Buffer& buffer) const;
        uint32_t registerBuffer(Buffer& buffer, uint64_t size) override;
        void unregisterBuffer(uint32_t slot);

        uint32_t getImageHeapIndexOffset() const override { return m_imageHeapIndexOffset; }
        
        uint64_t getSize() const override
        {
            return m_size;
        }

        uint64_t getReservedRangeOffset() const override
        {
            return m_reservedRangeOffset;
        }

        uint64_t getReservedRangeSize() const override
        {
            return m_reservedRangeSize;
        }

        uint64_t getGPUAddress() const override
        {
            auto& vkBuffer = static_cast<BufferVK&>(*m_heapBuffer);

            return getBufferDeviceAddress(vkBuffer.getNativeBuffer());
        }

    private:
        uint64_t alignSize(uint64_t value, uint64_t alignment) const;

    private:
        DeviceVK& m_deviceVK;
        DescriptorHeapType m_type;
        vk::PhysicalDeviceDescriptorHeapPropertiesEXT m_heapProps;
        
        // 👈 Using your RHI interface object wrapper directly!
        std::unique_ptr<Buffer> m_heapBuffer = nullptr; 
        void* m_mappedPtr = nullptr;

        uint64_t m_bufferDescSize = 0;
        uint64_t m_imageDescSize = 0;
        uint64_t m_samplerDescSize = 0;
        uint64_t m_imageHeapOffset = 0;
        uint32_t m_imageHeapIndexOffset = 0;

        uint32_t m_allocatedBufferCount = 0;
        uint32_t m_allocatedImageCount = 0;
        std::vector<uint32_t> m_freeImageSlots;
        std::vector<uint32_t> m_freeBufferSlots;
        
        uint64_t m_size = 0;
        uint64_t m_reservedRangeOffset = 0;
        uint64_t m_reservedRangeSize = 0;
    };
}
