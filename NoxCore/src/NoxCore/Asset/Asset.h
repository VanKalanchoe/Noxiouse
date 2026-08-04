#pragma once

#include <string_view>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/Core/UUID.h"

namespace Nox
{
    using AssetHandle = UUID;

    enum class AssetType : uint16_t
    {
        None = 0,
        Scene,
        Texture2D,
        MeshSource,
        Mesh,
        StaticMesh,
        
        Skeleton,
        AnimationSequence,
        SkeletalMesh
    };

    std::string_view AssetTypeToString(AssetType type);
    AssetType AssetTypeFromString(std::string_view assetType);

    class Asset : public RefCounted
    {
    public:
        AssetHandle Handle; // generate handle

        virtual AssetType GetType() const = 0;
    };
}