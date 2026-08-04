#include "AnimationImporter.h"

#include "MeshSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    Ref<AnimationSequence> AnimationImporter::ImportAnimation(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::filesystem::path cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;

        NOX_CORE_INFO("AnimationImporter::ImportAnimation loading animation from {}", cookedPath.string());

        Ref<AnimationSequence> animAsset = LoadAnimation(cookedPath);
        if (animAsset)
        {
            animAsset->Handle = handle;
        }

        return animAsset;
    }

    Ref<AnimationSequence> AnimationImporter::LoadAnimation(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("AnimationImporter::LoadAnimation - File does not exist: {}", path.string());
            return {};
        }

        Ref<AnimationSequence> anim = CreateRef<AnimationSequence>();
        bool success = AnimationSerializer::Deserialize(path, *anim);
        if (!success)
        {
            NOX_CORE_ASSERT(false, "AnimationImporter::LoadAnimation - Failed to deserialize .nanim file: {}", path.string());
            return {};
        }

        return anim;
    }
}