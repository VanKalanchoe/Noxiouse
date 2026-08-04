#include "AssetImporter.h"

#include "TextureImporter.h"
#include "SceneImporter.h"
#include "MeshImporter.h"

#include <map>

#include "AnimationImporter.h"
#include "SkeletalMeshImporter.h"
#include "SkeletonImporter.h"
#include "NoxCore/Core/Log.h"

namespace Nox
{
    using AssetImportFunction = std::function<Ref<Asset>(AssetHandle, const AssetMetadata&)>;
    /*static std::map<AssetType, AssetImportFunction> s_AssetImportFunctions = 
    {
        { AssetType::Texture2D, TextureImporter::ImportTexture2D },
        { AssetType::Scene, SceneImporter::ImportScene }
    };*/
    
    static std::map<AssetType, AssetImportFunction> s_AssetImportFunctions = 
    {
        { AssetType::Texture2D, [](AssetHandle h, const AssetMetadata& meta) -> Ref<Asset> {
            return Ref<Asset>(TextureImporter::ImportTexture2D(h, meta));
        }},
        { AssetType::Scene, [](AssetHandle h, const AssetMetadata& meta) -> Ref<Asset> {
            return Ref<Asset>(SceneImporter::ImportScene(h, meta));
        }},
        { AssetType::MeshSource, [](AssetHandle h, const AssetMetadata& meta) -> Ref<Asset>
        {
            return Ref<Asset>(MeshImporter::ImportMesh(h, meta));
        }},
        { AssetType::Mesh, [](AssetHandle h, const AssetMetadata& meta) -> Ref<Asset>
        {
            return Ref<Asset>(MeshImporter::ImportMesh(h, meta));
        }},
        { AssetType::StaticMesh, [](AssetHandle h, const AssetMetadata& meta) -> Ref<Asset>
        {
            return Ref<Asset>(MeshImporter::ImportStaticMesh(h, meta));
        }},
        { AssetType::Skeleton, [](AssetHandle h, const AssetMetadata& meta) -> Ref<Asset> {
            return Ref<Asset>(SkeletonImporter::ImportSkeleton(h, meta));
        }},
        { AssetType::AnimationSequence, [](AssetHandle h, const AssetMetadata& meta) -> Ref<Asset> {
            return Ref<Asset>(AnimationImporter::ImportAnimation(h, meta));
        }},
        { AssetType::SkeletalMesh, [](AssetHandle h, const AssetMetadata& meta) -> Ref<Asset> {
            return Ref<Asset>(SkeletalMeshImporter::ImportSkeletalMesh(h, meta));
        }}
    };
    
    
    Ref<Asset> AssetImporter::ImportAsset(AssetHandle handle, const AssetMetadata& metadata)
    {
        if (s_AssetImportFunctions.find(metadata.Type) == s_AssetImportFunctions.end())
        {
            NOX_CORE_ERROR("No importer available for asset type: {}", (uint16_t)metadata.Type);
            return {};
        }
        
        return s_AssetImportFunctions.at(metadata.Type)(handle, metadata);
    }
}
