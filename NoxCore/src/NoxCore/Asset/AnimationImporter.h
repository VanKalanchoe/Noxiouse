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
    };
}