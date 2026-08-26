#include "DescriptorHeapMTL.h"

#include "DeviceMTL.h"
#include "TextureMTL.h"

namespace NRI
{

DescriptorHeapMTL::DescriptorHeapMTL(
    DeviceMTL& device,
    const DescriptorHeapDesc& desc)
    : m_deviceMTL(device)
    , m_type(desc.type)
{
    if (m_type != DescriptorHeapType::Resource)
        return;

    // --------------------------------------------------
    // Metal resource heap
    // --------------------------------------------------

    constexpr uint64_t heapSize =
        64ull * 1024ull * 1024ull;

    MTL::HeapDescriptor* heapDesc =
        MTL::HeapDescriptor::alloc()->init();

    heapDesc->setType(
        MTL::HeapType::HeapTypeAutomatic);

    heapDesc->setStorageMode(
        MTL::StorageMode::StorageModePrivate);

    heapDesc->setCpuCacheMode(
        MTL::CPUCacheMode::CPUCacheModeDefaultCache);

    heapDesc->setSize(heapSize);

    m_heap =
        m_deviceMTL.getDevice()
            ->newHeap(heapDesc);

    heapDesc->release();

    if (!m_heap)
    {
        throw std::runtime_error(
            "Failed to create Metal resource heap");
    }

    // --------------------------------------------------
    // Metal 4 argument table
    // --------------------------------------------------

    MTL4::ArgumentTableDescriptor* tableDesc =
        MTL4::ArgumentTableDescriptor::alloc()->init();

    // buffer(0) = bindless texture IDs
    // buffer(1) = per-frame arguments
    tableDesc->setMaxBufferBindCount(2);

    // We are NOT binding one MTL texture per table slot.
    tableDesc->setMaxTextureBindCount(0);

    // sampler(0)
    tableDesc->setMaxSamplerStateBindCount(1);

    m_argumentTable =
        m_deviceMTL.getDevice()
            ->newArgumentTable(
                tableDesc,
                nullptr);

    tableDesc->release();

    if (!m_argumentTable)
    {
        throw std::runtime_error(
            "Failed to create Metal argument table");
    }

    // --------------------------------------------------
    // Initial bindless ID buffer
    // --------------------------------------------------

    m_textureCapacity =
        std::max(1u, desc.maxImageDescriptors);

    m_bindlessTextureBuffer =
        m_deviceMTL.getDevice()->newBuffer(
            sizeof(uint64_t) * m_textureCapacity,
            MTL::ResourceStorageModeShared);

    if (!m_bindlessTextureBuffer)
    {
        throw std::runtime_error(
            "Failed to create bindless texture buffer");
    }

    m_deviceMTL.getResidencySet()
        ->addAllocation(m_bindlessTextureBuffer);

    m_deviceMTL.getResidencySet()->commit();

    // buffer(0) is persistent.
    m_argumentTable->setAddress(
        m_bindlessTextureBuffer->gpuAddress(),
        0);
}

void DescriptorHeapMTL::registerTexture(Texture& texture)
{
    auto& metalTexture =
        static_cast<TextureMTL&>(texture);

    uint32_t slot;

    if (!m_freeImageSlots.empty())
    {
        slot = m_freeImageSlots.back();
        m_freeImageSlots.pop_back();
    }
    else
    {
        slot = m_allocatedImageCount++;
    }

    ensureTextureSlotCapacity(slot);

    // --------------------------------------------
    // Create the actual Metal texture from THIS
    // DescriptorHeapMTL's MTL::Heap.
    // --------------------------------------------

    MTL::TextureDescriptor* descriptor =
        MTL::TextureDescriptor::alloc()->init();

    descriptor->setTextureType(
        metalTexture.getDesc().sampleCount > 1
            ? MTL::TextureType::TextureType2DMultisample
            : MTL::TextureType::TextureType2D);

    descriptor->setPixelFormat(
        metalTexture.getPixelFormat());

    descriptor->setWidth(
        metalTexture.getDesc().width);

    descriptor->setHeight(
        metalTexture.getDesc().height);

    descriptor->setMipmapLevelCount(
        metalTexture.getDesc().sampleCount > 1
            ? 1
            : metalTexture.getDesc().mipLevels);

    descriptor->setSampleCount(
        metalTexture.getDesc().sampleCount);

    descriptor->setUsage(
        MTL::TextureUsageShaderRead);

    descriptor->setStorageMode(
        MTL::StorageMode::StorageModePrivate);

    MTL::Texture* nativeTexture =
        m_heap->newTexture(descriptor);

    descriptor->release();

    if (!nativeTexture)
        throw std::runtime_error(
            "Failed to create Metal heap texture");

    metalTexture.setNativeTexture(nativeTexture);

    m_deviceMTL.getResidencySet()
        ->addAllocation(nativeTexture);

    m_deviceMTL.getResidencySet()->commit();

    // --------------------------------------------
    // Bindless descriptor
    // --------------------------------------------

    auto* ids =
        static_cast<uint64_t*>(
            m_bindlessTextureBuffer->contents());

    ids[slot] =
        nativeTexture->gpuResourceID()._impl;

    metalTexture.setDescriptorIndexSlot(slot);
    metalTexture.setDescriptorHeap(this);

    if (slot >= m_registeredTextures.size())
        m_registeredTextures.resize(slot + 1);

    m_registeredTextures[slot] = &texture;
}

void DescriptorHeapMTL::unregisterTexture(
    uint32_t slot)
{
    if (slot == UINT32_MAX)
        return;

    if (slot >= m_allocatedImageCount)
        return;

    m_freeImageSlots.push_back(slot);

    if (slot < m_registeredTextures.size())
        m_registeredTextures[slot] = nullptr;
}

uint32_t DescriptorHeapMTL::registerBuffer(
    Buffer& /*buffer*/,
    uint64_t /*size*/)
{
    /*
     * Intentionally empty for now.
     *
     * Buffer GPU addresses are used directly by your
     * Metal 4 argument-table / bindless model.
     */
    return UINT32_MAX;
}

}
