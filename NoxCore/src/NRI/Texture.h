#pragma once
#include <cstdint>

#include <imgui.h>

#include "NoxCore/Asset/Asset.h"

namespace NRI
{
    enum class ImageFormat
    {
        None = 0,
        Surface,
        RGB8,
        SRGB8,
        
        RGBA8,
        SRGBA8,
        
        R16G16,
        R32SINT,
        R32G32B32A32_SFLOAT,
        
        //tinyddsloader format
        BC7_UNorm,
        BC7_UNorm_SRGB
    };
    
    enum class TextureLayout : uint8_t
    {
        Undefined,
        ColorAttachment,
        DepthAttachment,
        ShaderResource,
        TransferSrc,
        TransferDst,
        Present
    };
    
    enum class TextureUsage : uint8_t
    {
        ColorAttachment,
        DepthStencilAttachment,
        ShaderResource, // For regular textures
        ColorResolveAttachment,
    };

    struct TextureDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 1;
        uint32_t sampleCount = 1; // For MSAA
        TextureUsage usage;
        ImageFormat format = ImageFormat::None;
        uint32_t directFormat = UINT32_MAX;
    };

    class Texture : public Nox::Asset
    {
    public:
        virtual ~Texture() = default;
        
        virtual uint32_t GetWidth() const = 0;
        virtual uint32_t GetHeight() const = 0;
        virtual uint32_t GetDescriptorIndexSlot() const = 0;
    };
    
    class Texture2D : public Texture
    {
    public:
        virtual void uploadFromBuffer(class CommandBuffer& cmdBuffer, class Buffer& stagingBuffer, uint32_t width, uint32_t height, uint32_t mipLevels, const std::vector<size_t>& mipOffsets) = 0;
        virtual void copyImageToBuffer(CommandBuffer& commandBuffer, Buffer& dstBuffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
        
        [[nodiscard]] virtual uint32_t getMipLevels() const = 0; // ??
        [[nodiscard]] virtual TextureUsage getUsage() const = 0; // ??
        
        virtual ImTextureID getImTextureID() = 0;
        
        static Nox::AssetType GetStaticType() { return Nox::AssetType::Texture2D; }
        virtual Nox::AssetType GetType() const { return GetStaticType(); }
    };
}

namespace Nox
{
    using Texture2D = NRI::Texture2D;
}
