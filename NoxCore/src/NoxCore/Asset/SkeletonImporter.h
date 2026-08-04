#pragma once

#include <filesystem>
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Animation/Skeleton.h"

namespace Nox
{
    struct AssetMetadata;

    class SkeletonImporter
    {
    public:
        // AssetMetadata FilePath is relative to project asset directory (.nskel)
        static Ref<Skeleton> ImportSkeleton(AssetHandle handle, const AssetMetadata& metadata);

        // Direct loader from absolute/relative disk path
        static Ref<Skeleton> LoadSkeleton(const std::filesystem::path& path);
    };
}