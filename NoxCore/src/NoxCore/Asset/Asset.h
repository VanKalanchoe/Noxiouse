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
        Material,
        
        Skeleton,
        AnimationSequence,
        SkeletalMesh,

        AnimationGraph, // NodeGraph/NodeGraphAsset.h wrapping a NodeGraph whose Domain is "AnimationGraph" (.nanimgraph)

        Prefab // Scene/Prefab.h: a saved recipe of entities (.nprefab), instanced in scenes by PrefabInstanceComponent
    };

    // Where an asset is on its way into memory (§5.11.2). A request starts the load once; the asset is usable when Ready.
    enum class AssetState : uint8_t
    {
        Unloaded,
        Loading, // reading, decoding and uploading in the background
        Ready,
        Failed
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
