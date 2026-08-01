#pragma once
#include <filesystem>

#include "Asset.h"

namespace Nox
{
    struct AssetMetadata
    {
        AssetType Type = AssetType::None;
        std::filesystem::path FilePath;
        std::filesystem::path SourceFilePath;

        operator bool () const { return Type != AssetType::None; }
    };
}
