#pragma once

#include "../Texture.h"
#include "MetalCommon.h"

namespace NRI
{
    class DeviceMTL;
    class DescriptorHeap;

    class TextureMTL final : public Texture2D
    {
    public:
        TextureMTL(
            DeviceMTL& device,
            const TextureDesc& desc);

        ~TextureMTL() override;

        uint32_t GetWidth() const override
        {
            return m_desc.width;
        }

        uint32_t GetHeight() const override
        {
            return m_desc.height;
        }

        uint32_t GetDescriptorIndexSlot() const override
        {
            return m_descriptorIndexSlot;
        }

        uint32_t getMipLevels() const override
        {
            return m_desc.mipLevels;
        }

        TextureUsage getUsage() const override
        {
            return m_desc.usage;
        }

        ImTextureID getImTextureID() override;

        MTL::Texture* getNativeTexture()
        {
            return m_texture;
        }
        
        void setNativeTexture(MTL::Texture* tex) { m_texture = tex; }

        MTL::PixelFormat getPixelFormat() const
        {
            return m_pixelFormat;
        }

        void setDescriptorIndexSlot(uint32_t index)
        {
            m_descriptorIndexSlot = index;
        }

        void setDescriptorHeap(DescriptorHeap* heap)
        {
            m_boundHeap = heap;
        }
        
        TextureDesc& getDesc() { return m_desc; }

        void uploadFromBuffer(
            CommandBuffer& cmdBuffer,
            Buffer& stagingBuffer,
            uint32_t width,
            uint32_t height,
            uint32_t mipLevels,
            const std::vector<size_t>& mipOffsets) override;

        void copyImageToBuffer(
            CommandBuffer& commandBuffer,
            Buffer& dstBuffer,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height) override;

    private:
        DeviceMTL& m_deviceMTL;
        TextureDesc m_desc;

        MTL::Texture* m_texture = nullptr;

        MTL::PixelFormat m_pixelFormat =
            MTL::PixelFormat::PixelFormatInvalid;

        uint32_t m_descriptorIndexSlot = UINT32_MAX;

        DescriptorHeap* m_boundHeap = nullptr;
    };
}
