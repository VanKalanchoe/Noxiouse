#pragma once
#include "Asset.h"
#include "AssetMetadata.h"
#include "NoxCore/Renderer/Mesh.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Animation/Skeleton.h"
#include "NoxCore/Animation/AnimationSequence.h"

namespace Nox
{
    class MeshImporter
    {
    public:
        // AssetMetadata filepath is relative to project asset directory
        static Ref<Mesh> ImportMesh(AssetHandle handle, const AssetMetadata& metadata);
        
        static Ref<StaticMesh> ImportStaticMesh(AssetHandle handle, const AssetMetadata& metadata);
        
        // Load from filepath
        static Ref<Mesh> LoadMesh(const std::filesystem::path& path);
        
    private:
        static std::vector<MeshData> ParseGltfToMeshData(const std::filesystem::path& path, std::vector<MaterialData>& outMaterials, Skeleton& outSkeleton, std::vector<Ref<AnimationSequence>>& outAnimations);
    };
}
