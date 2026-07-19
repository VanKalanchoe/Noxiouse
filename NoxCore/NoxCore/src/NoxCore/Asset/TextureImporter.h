#pragma once

#include <filesystem>

#include "Asset.h"
#include "AssetMetadata.h"
#include "NoxCore/Core/core.h"
#include "NRI/Texture.h"

namespace Nox
{
    struct TextureSpecification
    {
        bool generateMips = true;
    };
    
    struct TextureData
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t MipLevels = 0;
        uint64_t ImageSize = 0;
        uint8_t* Pixels = nullptr; // Raw CPU buffer from stb
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
        
    private:
        static Ref<Texture2D> LoadWithSTB(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer);
        static Ref<Texture2D> LoadWithDDS(const std::filesystem::path& path, const TextureSpecification& spec);
        static Ref<Texture2D> LoadWithKTX(const std::filesystem::path& path, const TextureSpecification& spec);
    };
}
