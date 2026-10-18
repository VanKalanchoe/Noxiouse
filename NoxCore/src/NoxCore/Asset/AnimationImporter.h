#pragma once

#include <filesystem>
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Animation/AnimationSequence.h"

namespace Nox
{
    struct AssetMetadata;

    class AnimationImporter
    {
    public:
        // AssetMetadata FilePath is relative to project asset directory (.nanim)
        static Ref<AnimationSequence> ImportAnimation(AssetHandle handle, const AssetMetadata& metadata);

        // Direct loader from absolute/relative disk path
        static Ref<AnimationSequence> LoadAnimation(const std::filesystem::path& path);

        // A clip's notifies live beside it (<clip>.nanim.notifies): read when the clip loads, written by the clip editor.
        static std::filesystem::path NotifiesPath(const std::filesystem::path& clipPath);
        static std::vector<AnimationNotify> LoadNotifies(const std::filesystem::path& clipPath);
        static bool SaveNotifies(const std::filesystem::path& clipPath, const std::vector<AnimationNotify>& notifies);
    };
}