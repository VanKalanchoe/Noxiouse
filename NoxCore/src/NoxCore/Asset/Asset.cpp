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
        
        return AssetType::None;
    }
}
