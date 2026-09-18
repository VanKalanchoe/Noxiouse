#pragma once
#include <algorithm>
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
        
        R16_SFLOAT,
        R32_SFLOAT,
        R10G10B10A2_UNORM,
        R16G16,
        R32SINT,
        R32G32_UINT,
        R32G32_SFLOAT,
        
        R16G16_SFLOAT,
        R16G16B16A16_SFLOAT,
        R32G32B32A32_SFLOAT,
        
        // tinyddsloader formats
        BC1_UNorm,
        BC1_UNorm_SRGB,
        BC2_UNorm,
        BC2_UNorm_SRGB,
        BC3_UNorm,
        BC3_UNorm_SRGB,
        BC4_UNorm,
        BC4_SNorm,
        BC5_UNorm,
        BC5_SNorm,
        BC6H_UF16,
        BC6H_SF16,
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
        Present,
        General
    };
    
    enum class TextureUsage : uint8_t
    {
        Default = 0, // Sentinel value for descriptor heap registration
        ColorAttachment,
        DepthStencilAttachment,
        ShaderResource, // For regular textures
        ColorResolveAttachment,
        Storage
    };

    struct TextureDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t arrayLayers = 1;
        bool isCubeMap = false;
        uint32_t mipLevels = 1;
        uint32_t sampleCount = 1; // For MSAA
        TextureUsage usage;
        ImageFormat format = ImageFormat::None;
        uint32_t directFormat = UINT32_MAX;
    };

    // What a texture costs in device memory: the mip chain of every layer, without the driver's alignment (images are
    // their own allocations, so this is what the memory categories account for).
    inline uint64_t estimateTextureBytes(const TextureDesc& desc)
    {
        uint64_t blockBytes = 4;  // bytes per pixel, or per 4x4 block for the BC formats
        uint32_t blockSize = 1;
        switch (desc.format)
        {
        case ImageFormat::R16_SFLOAT: blockBytes = 2; break;
        case ImageFormat::R16G16:
        case ImageFormat::R16G16_SFLOAT: blockBytes = 4; break;
        case ImageFormat::R32G32_UINT:
        case ImageFormat::R32G32_SFLOAT:
        case ImageFormat::R16G16B16A16_SFLOAT: blockBytes = 8; break;
        case ImageFormat::R32G32B32A32_SFLOAT: blockBytes = 16; break;
        case ImageFormat::BC1_UNorm:
        case ImageFormat::BC1_UNorm_SRGB:
        case ImageFormat::BC4_UNorm:
        case ImageFormat::BC4_SNorm: blockBytes = 8; blockSize = 4; break;
        case ImageFormat::BC2_UNorm:
        case ImageFormat::BC2_UNorm_SRGB:
        case ImageFormat::BC3_UNorm:
        case ImageFormat::BC3_UNorm_SRGB:
        case ImageFormat::BC5_UNorm:
        case ImageFormat::BC5_SNorm:
        case ImageFormat::BC6H_UF16:
        case ImageFormat::BC6H_SF16:
        case ImageFormat::BC7_UNorm:
        case ImageFormat::BC7_UNorm_SRGB: blockBytes = 16; blockSize = 4; break;
        default: break;
        }

        uint64_t bytes = 0;
        uint64_t width = desc.width;
        uint64_t height = desc.height;
        for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
        {
            const uint64_t blocksX = (std::max<uint64_t>(width, 1) + blockSize - 1) / blockSize;
            const uint64_t blocksY = (std::max<uint64_t>(height, 1) + blockSize - 1) / blockSize;
            bytes += blocksX * blocksY * blockBytes;
            width /= 2;
            height /= 2;
        }

        const uint64_t layers = desc.isCubeMap ? 6u * desc.arrayLayers : desc.arrayLayers;
        return bytes * std::max<uint64_t>(layers, 1);
    }

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
        // Nearest-filtered resize copy into dst (dst may be a different resolution than this texture).
        // Used to propagate render-resolution G-buffer data (entity IDs, depth) up to display
        // resolution for passes/picking that need to match the final image size when DLSS is scaling.
        // Nearest is mandatory anyway - depth and integer ID formats can't use linear filtering in Vulkan.
        virtual void blitTo(CommandBuffer& commandBuffer, Texture2D& dst) = 0;
        virtual void generateMipmaps(CommandBuffer& commandBuffer) = 0;
        
        [[nodiscard]] virtual uint32_t getMipLevels() const = 0; // ??
        [[nodiscard]] virtual uint32_t getArrayLayers() const = 0; // ??
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
