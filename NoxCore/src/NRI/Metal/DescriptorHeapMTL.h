#pragma once

#include "../DescriptorHeap.h"
#include "MetalCommon.h"

namespace NRI
{
    class DeviceMTL;

    class DescriptorHeapMTL final : public DescriptorHeap
    {
    public:
        DescriptorHeapMTL(
            DeviceMTL& device,
            const DescriptorHeapDesc& desc);

        ~DescriptorHeapMTL() override;

        void registerTexture(Texture& texture) override;
        void unregisterTexture(uint32_t slot) override;

        uint32_t registerBuffer(
            Buffer& buffer,
            uint64_t size) override;

        uint32_t getImageHeapIndexOffset() const override
        {
            return 0;
        }

        uint64_t getSize() const override
        {
            return m_heap ? m_heap->size() : 0;
        }

        uint64_t getReservedRangeOffset() const override
        {
            return 0;
        }

        uint64_t getReservedRangeSize() const override
        {
            return 0;
        }

        uint64_t getGPUAddress() const override
        {
            return 0;
        }

        MTL::Heap* getNativeHeap() const
        {
            return m_heap;
        }

        MTL4::ArgumentTable* getArgumentTable() const
        {
            return m_argumentTable;
        }

    private:
        void ensureTextureSlotCapacity(
            uint32_t requiredSlot);

    private:
        DeviceMTL& m_deviceMTL;
        DescriptorHeapType m_type;

        // Actual Metal resource heap.
        MTL::Heap* m_heap = nullptr;

        // Metal 4 binding table.
        MTL4::ArgumentTable* m_argumentTable = nullptr;

        // uint64_t GPU resource IDs.
        MTL::Buffer* m_bindlessTextureBuffer = nullptr;

        uint32_t m_textureCapacity = 0;
        uint32_t m_allocatedImageCount = 0;

        std::vector<uint32_t> m_freeImageSlots;

        std::vector<Texture*> m_registeredTextures;
    };
}
