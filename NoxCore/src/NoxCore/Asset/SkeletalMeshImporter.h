#pragma once

#include <filesystem>
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Renderer/SkeletalMesh.h"

namespace Nox
{
    struct AssetMetadata;

    class SkeletalMeshImporter
    {
    public:
        // AssetMetadata FilePath is relative to project asset directory (.nmesh or .nskmesh)
        static Ref<SkeletalMesh> ImportSkeletalMesh(AssetHandle handle, const AssetMetadata& metadata);

        // Direct loader from absolute/relative disk path
        static Ref<SkeletalMesh> LoadSkeletalMesh(const std::filesystem::path& path);
    };
}