#include "Asset.h"

namespace Nox
{
    std::string_view AssetTypeToString(AssetType type)
    {
        switch (type)
        {
        case AssetType::None: return "AssetType::None";
        case AssetType::Scene: return "AssetType::Scene";
        case AssetType::Texture2D: return "AssetType::Texture2D";
        case AssetType::MeshSource: return "AssetType::MeshSource";
        case AssetType::Mesh: return "AssetType::Mesh";
        case AssetType::StaticMesh: return "AssetType::StaticMesh";
            
        case AssetType::Skeleton: return "AssetType::Skeleton";
        case AssetType::AnimationSequence: return "AssetType::AnimationSequence";
        case AssetType::SkeletalMesh: return "AssetType::SkeletalMesh";
        }

        return "AssetType::<Invalid>";
    }

    AssetType AssetTypeFromString(std::string_view assetType)
    {
        if (assetType == "AssetType::None") return AssetType::None;
        if (assetType == "AssetType::Scene") return AssetType::Scene;
        if (assetType == "AssetType::Texture2D") return AssetType::Texture2D;
        if (assetType == "AssetType::MeshSource") return AssetType::MeshSource;
        if (assetType == "AssetType::Mesh") return AssetType::Mesh;
        if (assetType == "AssetType::StaticMesh") return AssetType::StaticMesh;
        
        if (assetType == "AssetType::Skeleton") return AssetType::Skeleton;
        if (assetType == "AssetType::AnimationSequence") return AssetType::AnimationSequence;
        if (assetType == "AssetType::SkeletalMesh") return AssetType::SkeletalMesh;
        
        return AssetType::None;
    }
}
