#pragma once
#include <filesystem>
#include <iosfwd>
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
    
    // A cooked texture as a background load reads it: the header first, the texels later straight into staging.
    struct CookedTextureHeader
    {
        std::filesystem::path Path;
        TextureData Texture;     // everything but the texels (Data stays empty)
        uint64_t DataOffset = 0; // of the texels in the file
        uint64_t DataSize = 0;
    };

    class Renderer;

    class TextureImporter
    {
    public:
        // Any thread. The header of the asset's cooked .ntex when it is current; empty when the asset has to be cooked
        // first or is not cooked at all (those load synchronously).
        static std::optional<CookedTextureHeader> ReadCookedTextureHeader(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata);
        // Any thread. The texels of mips [firstMip, firstMip + mipCount), as they lie in the file.
        static bool ReadCookedTextureMips(const CookedTextureHeader& header, uint32_t firstMip, uint32_t mipCount, uint8_t* destination);
        // Any thread. Cooks a PNG / JPG / DDS / KTX2 source into the asset's .ntex (and its .hash); false for other
        // sources or when it fails.
        static bool CookTexture(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata);

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
        static bool DecodeDDS(const std::filesystem::path& path, TextureData& cpuData);
        static bool DecodeKTX(const std::filesystem::path& path, const TextureSpecification& spec, TextureData& cpuData);
        static Ref<Texture2D> LoadWithKTX(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer);
        static Ref<Texture2D> LoadWithNTEX(const std::filesystem::path& path, Renderer* renderer);
        static bool SaveNTEX(const std::filesystem::path& path, const TextureData& cpuData);
        static bool ReadNTEX(const std::filesystem::path& path, TextureData& outData);
        // Everything up to the texels; the stream is left at the first texel.
        static bool ReadNTEXHeader(std::istream& stream, TextureData& outData, uint64_t& outDataSize);
    };
}
