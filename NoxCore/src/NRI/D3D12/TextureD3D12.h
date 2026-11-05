#pragma once
#include "D3D12Common.h"
#include "../Texture.h"
#include "MemoryAllocatorD3D12.h"

namespace NRI
{
    class DeviceD3D12;

    DXGI_FORMAT TranslateImageFormat(ImageFormat format);

    class TextureD3D12 final : public Texture2D
    {
    public:
        TextureD3D12(DeviceD3D12& device, const TextureDesc& desc);
        // Wrapping an existing swapchain backbuffer
        TextureD3D12(DeviceD3D12& device, ComPtr<ID3D12Resource> resource, const TextureDesc& desc);
        ~TextureD3D12() override;

        uint32_t GetWidth() const override { return m_desc.width; }
        uint32_t GetHeight() const override { return m_desc.height; }
        uint32_t GetDescriptorIndexSlot() const override { return m_descriptorSlot; }
        void setDescriptorIndexSlot(uint32_t slot) { m_descriptorSlot = slot; }

        uint32_t getMipLevels() const override { return m_desc.mipLevels; }
        uint32_t getArrayLayers() const override { return m_desc.arrayLayers; }
        TextureUsage getUsage() const override { return m_desc.usage; }
        const TextureDesc& getDesc() const { return m_desc; }

        ID3D12Resource* getNativeResource() const { return m_resource.Get(); }
        DXGI_FORMAT getDxgiFormat() const { return m_dxgiFormat; }

        void uploadFromBuffer(CommandBuffer& cmdBuffer, Buffer& stagingBuffer, uint32_t width, uint32_t height,
                              uint32_t mipLevels, const std::vector<size_t>& mipOffsets) override;
        void recordUpload(CommandBuffer& cmdBuffer, Buffer& stagingBuffer, uint64_t stagingOffset,
                          const std::vector<size_t>& mipOffsets) override;
        void recordInitialLayout(CommandBuffer& cmdBuffer) override;
        void recordRegionUploads(CommandBuffer& cmdBuffer, Buffer& stagingBuffer, std::span<const TextureRegionUpload> regions) override;
        void copyImageToBuffer(CommandBuffer& commandBuffer, Buffer& dstBuffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
        void blitTo(CommandBuffer& commandBuffer, Texture2D& dst) override;
        void generateMipmaps(CommandBuffer& commandBuffer) override;

        ImTextureID getImTextureID() override;

    private:
        void createResource();

    private:
        DeviceD3D12& m_deviceD3D12;
        TextureDesc m_desc;
        DXGI_FORMAT m_dxgiFormat = DXGI_FORMAT_UNKNOWN;
        AllocatedImageD3D12 m_image;
        ComPtr<ID3D12Resource> m_resource;
        uint32_t m_descriptorSlot = ~0u;
        bool m_ownsResource = true;
    };
}
