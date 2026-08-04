#include "SkeletonImporter.h"

#include "MeshSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    Ref<Skeleton> SkeletonImporter::ImportSkeleton(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::filesystem::path cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;

        NOX_CORE_INFO("SkeletonImporter::ImportSkeleton loading skeleton from {}", cookedPath.string());

        Ref<Skeleton> skeletonAsset = LoadSkeleton(cookedPath);
        if (skeletonAsset)
        {
            skeletonAsset->Handle = handle;
        }

        return skeletonAsset;
    }

    Ref<Skeleton> SkeletonImporter::LoadSkeleton(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("SkeletonImporter::LoadSkeleton - File does not exist: {}", path.string());
            return {};
        }

        Ref<Skeleton> skeleton = CreateRef<Skeleton>();
        bool success = SkeletonSerializer::Deserialize(path, *skeleton);
        if (!success)
        {
            NOX_CORE_ASSERT(false, "SkeletonImporter::LoadSkeleton - Failed to deserialize .nskel file: {}", path.string());
            return {};
        }

        return skeleton;
    }
}