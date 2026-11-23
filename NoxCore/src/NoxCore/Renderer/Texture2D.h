#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <imgui.h>
#include <NRIHPP/NRI.hpp>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    // Engine Asset wrapper around a NoxRHI texture (nri::Texture).
    class Texture2D : public Asset
    {
    public:
        Texture2D() = default;
        explicit Texture2D(nri::Texture texture) : m_Texture(std::move(texture)) {}
        ~Texture2D() override = default;

        Texture2D(const Texture2D&) = delete;
        Texture2D& operator=(const Texture2D&) = delete;

        nri::Texture& GetRHI() { return m_Texture; }
        const nri::Texture& GetRHI() const { return m_Texture; }
        NriTexture GetHandle() const { return m_Texture.handle(); }
        operator const nri::Texture&() const { return m_Texture; }
        explicit operator bool() const { return static_cast<bool>(m_Texture); }

        uint32_t GetWidth() const { return m_Texture ? m_Texture.getWidth() : 0; }
        uint32_t GetHeight() const { return m_Texture ? m_Texture.getHeight() : 0; }
        uint32_t GetDescriptorIndexSlot() const { return m_Texture ? m_Texture.getDescriptorSlot() : ~0u; }
        uint32_t getMipLevels() const { return m_Texture ? m_Texture.getMipLevels() : 0; }
        uint32_t getArrayLayers() const { return m_Texture ? m_Texture.getArrayLayers() : 0; }
        nri::TextureUsage getUsage() const { return m_Texture ? m_Texture.getUsage() : NRI_TEXTURE_USAGE_DEFAULT; }

        void uploadFromBuffer(nri::CommandBuffer& cmdBuffer, nri::Buffer& stagingBuffer, uint32_t width, uint32_t height,
                              uint32_t mipLevels, const std::vector<size_t>& mipOffsets)
        {
            m_Texture.uploadFromBuffer(cmdBuffer.handle(), stagingBuffer.handle(), width, height, mipLevels, mipOffsets);
        }

        void recordUpload(nri::CommandBuffer& cmdBuffer, nri::Buffer& stagingBuffer, uint64_t stagingOffset,
                          const std::vector<size_t>& mipOffsets)
        {
            m_Texture.recordUpload(cmdBuffer.handle(), stagingBuffer.handle(), stagingOffset, mipOffsets);
        }

        void recordInitialLayout(nri::CommandBuffer& cmdBuffer)
        {
            m_Texture.recordInitialLayout(cmdBuffer.handle());
        }

        void recordRegionUploads(nri::CommandBuffer& cmdBuffer, nri::Buffer& stagingBuffer,
                                 std::span<const nri::TextureRegionUpload> regions)
        {
            m_Texture.recordRegionUploads(cmdBuffer.handle(), stagingBuffer.handle(), regions);
        }

        void copyImageToBuffer(nri::CommandBuffer& cmdBuffer, nri::Buffer& dstBuffer,
                               uint32_t x, uint32_t y, uint32_t width, uint32_t height)
        {
            m_Texture.copyToBuffer(cmdBuffer.handle(), dstBuffer.handle(), x, y, width, height);
        }

        void blitTo(nri::CommandBuffer& cmdBuffer, Texture2D& dst)
        {
            m_Texture.blitTo(cmdBuffer.handle(), dst.GetHandle());
        }

        void generateMipmaps(nri::CommandBuffer& cmdBuffer)
        {
            m_Texture.generateMipmaps(cmdBuffer.handle());
        }

        ImTextureID getImTextureID() const
        {
            return m_Texture ? static_cast<ImTextureID>(m_Texture.getDescriptorSlot()) : static_cast<ImTextureID>(0);
        }

        static AssetType GetStaticType() { return AssetType::Texture2D; }
        AssetType GetType() const override { return GetStaticType(); }

    private:
        nri::Texture m_Texture;
    };
}
