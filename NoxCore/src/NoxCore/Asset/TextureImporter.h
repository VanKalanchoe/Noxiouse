#pragma once
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <vector>

#include "Asset.h"
#include "AssetMetadata.h"
#include "NoxCore/Core/core.h"
#include "NRI/Texture.h"
#include "NoxCore/Core/Buffer.h"

namespace Nox
{
    struct TextureData
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t ArrayLayers = 1;
        bool IsCubeMap = false;
        uint32_t MipLevels = 0;
        NRI::TextureUsage Usage = NRI::TextureUsage::ShaderResource;
        Buffer Data; // Raw CPU buffer from stb
        NRI::ImageFormat Format = NRI::ImageFormat::SRGBA8;
        uint32_t DirectFormat = UINT32_MAX;
        std::vector<size_t> MipOffsets;
    };
    
    // How a virtual texture's cooked file (NTV1, §5.12.2) holds its texels. Every mip larger than a tile is cut into tiles of
    // TileSize² texels plus a Border on each side (Unreal's tile size 128, border 4: filtering and anisotropy inside a tile need
    // no neighbor), stored mip by mip, row by row, each its own TileBytes; the border wraps around the texture (repeat addressing),
    // a mip's last tiles past its edge wrap too. The mips no larger than a tile (the tail) are stored whole, as an .ntex stores them.
    struct VirtualTextureLayout
    {
        static constexpr uint32_t DefaultTileSize = 128;
        static constexpr uint32_t DefaultBorder = 4;

        struct Mip
        {
            uint32_t TilesX = 0;
            uint32_t TilesY = 0;
            uint32_t FirstTile = 0;
        };

        uint32_t TileSize = DefaultTileSize; // texels, without the borders
        uint32_t Border = DefaultBorder;
        uint32_t BlockSize = 1;              // texels per block side (4: BC)
        uint32_t BlockBytes = 4;
        uint32_t TiledMips = 0;              // mips [0, TiledMips) are tiled, [TiledMips, MipLevels) is the tail
        std::vector<Mip> Mips;               // per tiled mip
        uint32_t TileCount = 0;
        uint64_t TilesOffset = 0;            // of the first tile in the file
        uint64_t TailOffset = 0;             // of the tail's first mip in the file
        uint64_t TailSize = 0;

        // Blocks per tile side, borders included.
        uint32_t TileBlocks() const { return (TileSize + 2 * Border) / BlockSize; }
        uint64_t TileBytes() const { return uint64_t(TileBlocks()) * TileBlocks() * BlockBytes; }
        uint64_t TileFileOffset(uint32_t mip, uint32_t tileX, uint32_t tileY) const
        {
            return TilesOffset + (uint64_t(Mips[mip].FirstTile) + uint64_t(tileY) * Mips[mip].TilesX + tileX) * TileBytes();
        }
    };

    // A cooked texture as a background load reads it: the header first, the texels later straight into staging.
    struct CookedTextureHeader
    {
        std::filesystem::path Path;
        TextureData Texture;     // everything but the texels (Data stays empty); MipOffsets as an .ntex lays the mips out
        uint64_t DataOffset = 0; // of the texels in the file (.ntex)
        uint64_t DataSize = 0;   // of all mips laid out as MipOffsets says
        // A virtual texture's tiles (NTV1); ReadCookedTextureMips still returns its whole mips.
        std::shared_ptr<const VirtualTextureLayout> Virtual;
    };

    class Renderer;

    class TextureImporter
    {
    public:
        // Any thread. The header of the asset's cooked .ntex when it is current; empty when the asset has to be cooked
        // first or is not cooked at all (those load synchronously).
        static std::optional<CookedTextureHeader> ReadCookedTextureHeader(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata);
        // Any thread. The texels of mips [firstMip, firstMip + mipCount), laid out as the header's MipOffsets say (a virtual
        // texture's tiled mips are put together from their tiles).
        static bool ReadCookedTextureMips(const CookedTextureHeader& header, uint32_t firstMip, uint32_t mipCount, uint8_t* destination);
        // Any thread. One tile of a virtual texture's tiled mip into `destination` (VirtualTextureLayout::TileBytes).
        static bool ReadVirtualTextureTile(const CookedTextureHeader& header, uint32_t mip, uint32_t tileX, uint32_t tileY, uint8_t* destination);
        // Whether a texture with this header is cooked as a virtual texture under `mode`: one 2D layer of a format with a fixed
        // block size, larger than a tile, with its mips down to a tile (Auto: from VirtualTextureAutoSize² texels).
        static bool ResolveVirtualTexture(VirtualTextureMode mode, const TextureData& texture);
        // Any thread. Cooks a PNG / JPG / DDS / KTX2 source into the asset's .ntex (and its .hash); false for other
        // sources or when it fails.
        static bool CookTexture(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata);
        // Any thread. Texels made by a tool -- `cpuData` one RGBA8 / SRGBA8 level, its Width x Height -- cooked into an .ntex at
        // `cookedPath` (absolute) with a mip chain: BC7, or BC1 with `fast` (several times smaller to encode, 4 bits a texel).
        // Registered with that .ntex as its FilePath and no source, the file is its own cook.
        static bool CookGeneratedTexture(TextureData& cpuData, const std::filesystem::path& cookedPath, bool fast);

        // AssetMetadata filepath is relative to project asset directory
        static Ref<Texture2D> ImportTexture2D(AssetHandle handle, const AssetMetadata& metadata);

        // Reads file directly from filesystem
        // (i.e. path has to be relative / absolute to working directory)
        static Ref<Texture2D> LoadTexture2D(const std::filesystem::path& path, const TextureSpecification& spec = TextureSpecification(), Renderer* renderer = nullptr);
        static Ref<Texture2D> LoadTexture2DFromMemory(TextureData& cpuData, const TextureSpecification& spec  = TextureSpecification(), Renderer* renderer = nullptr);
        
    private:
        static Ref<Texture2D> LoadWithSTB(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer);
        static Ref<Texture2D> LoadWithSTBHDR(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer);
        static Ref<Texture2D> LoadWithDDS(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer);
        // The file's texels (all mips) in memory cpuData owns.
        static bool DecodeSTB(const std::filesystem::path& path, const TextureSpecification& spec, TextureData& cpuData);
        static bool DecodeDDS(const std::filesystem::path& path, const TextureSpecification& spec, TextureData& cpuData);
        static bool DecodeKTX(const std::filesystem::path& path, const TextureSpecification& spec, TextureData& cpuData);
        static Ref<Texture2D> LoadWithKTX(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer);
        static Ref<Texture2D> LoadWithNTEX(const std::filesystem::path& path, Renderer* renderer);
        static bool SaveNTEX(const std::filesystem::path& path, const TextureData& cpuData);
        // As a virtual texture (NTV1): cpuData's mips cut into tiles.
        static bool SaveNTV(const std::filesystem::path& path, const TextureData& cpuData);
        static bool ReadNTEX(const std::filesystem::path& path, TextureData& outData);
        // Everything up to the texels of an .ntex or a virtual texture (header.Path set).
        static bool ReadCookedHeader(std::istream& stream, CookedTextureHeader& header);
        // Whether the cooked texture is a virtual texture exactly when its settings say so (else it is cooked again).
        static bool CookedVirtualTextureMatches(const std::filesystem::path& cookedPath, const TextureSpecification& spec);
    };
}
