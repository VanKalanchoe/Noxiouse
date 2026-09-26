#pragma once

#include <filesystem>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Scene/Prefab.h"

namespace Nox
{
    struct AssetMetadata;

    class PrefabImporter
    {
    public:
        // AssetMetadata FilePath is relative to the project asset directory (.nprefab)
        static Ref<Prefab> ImportPrefab(AssetHandle handle, const AssetMetadata& metadata);

        // Writes the prefab's current content back to its file (a variant with its base and its changes).
        static bool SavePrefab(const Prefab& prefab, const std::filesystem::path& absolutePath);

        // A new variant of `base`: no changes and no entities of its own yet.
        static bool CreateVariantFile(const std::string& name, uint64_t baseRoot, AssetHandle base, const std::filesystem::path& absolutePath);
    };
}
