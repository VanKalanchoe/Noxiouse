#pragma once
#include <filesystem>

#include "Asset.h"
#include "AssetMetadata.h"
#include "NoxCore/Core/core.h"
#include "NRI/Texture.h"
#include "NoxCore/Core/Buffer.h"

namespace Nox
{
    struct TextureSpecification
    {
        bool flip = false;
        bool generateMips = true;
    };
    
    struct TextureData
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t MipLevels = 0;
        Buffer Data; // Raw CPU buffer from stb
        NRI::ImageFormat Format = NRI::ImageFormat::SRGBA8;
        uint32_t DirectFormat;
        std::vector<size_t> MipOffsets;
    };
    
    class Renderer;
    
    class TextureImporter
    {
    public:
        // AssetMetadata filepath is relative to project asset directory
        static Ref<Texture2D> ImportTexture2D(AssetHandle handle, const AssetMetadata& metadata);

        // Reads file directly from filesystem
        // (i.e. path has to be relative / absolute to working directory)
        static Ref<Texture2D> LoadTexture2D(const std::filesystem::path& path, const TextureSpecification& spec = TextureSpecification(), Renderer* renderer = nullptr);
        static Ref<Texture2D> LoadTexture2DFromMemory(TextureData& cpuData, const TextureSpecification& spec  = TextureSpecification(), Renderer* renderer = nullptr);
        
    private:
        static Ref<Texture2D> LoadWithSTB(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer);
        static Ref<Texture2D> LoadWithDDS(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer);
        static Ref<Texture2D> LoadWithKTX(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer);
    };
}
