#pragma once
#include <filesystem>

#include "Asset.h"
#include "NRI/Texture.h"

namespace Nox
{
    struct TextureSpecification
    {
        bool flip = false;
        bool generateMips = true;
        NRI::ImageFormat format = NRI::ImageFormat::SRGBA8; // Default to sRGB
    };
    
    struct AssetMetadata
    {
        AssetType Type = AssetType::None;
        std::filesystem::path FilePath;
        std::filesystem::path SourceFilePath;
        TextureSpecification TextureSpec; // Import settings for Texture2D assets

        operator bool () const { return Type != AssetType::None; }
    };
}
