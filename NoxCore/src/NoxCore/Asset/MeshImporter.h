#pragma once
#include <optional>

#include "Asset.h"
#include "AssetMetadata.h"
#include "NoxCore/Renderer/Mesh.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Animation/Skeleton.h"
#include "NoxCore/Animation/AnimationSequence.h"

namespace Nox
{
    // A cooked mesh as a background load reads it.
    struct CookedMesh
    {
        std::vector<MeshData> Submeshes;
        std::vector<MaterialData> Materials;
        std::vector<LightNodeData> Lights;
        std::vector<MeshNodeData> Nodes;
        std::vector<CameraNodeData> Cameras;
    };

    class MeshImporter
    {
    public:
        // Any thread. The asset's cooked mesh (.nmesh, or .nsmesh for a StaticMesh) when it is current; empty when it
        // has to be cooked first (that loads synchronously).
        static std::optional<CookedMesh> ReadCookedMesh(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata);
        // Main thread. The Mesh / StaticMesh asset around cooked data (its CPU data moves in), without geometry: each
        // submesh is set once its upload is published.
        static Ref<Asset> CreateMeshAsset(AssetType type, CookedMesh& cooked);
        static void SetSubMesh(Asset& mesh, size_t index, const MeshHandle& handle);

        // AssetMetadata filepath is relative to project asset directory
        static Ref<Mesh> ImportMesh(AssetHandle handle, const AssetMetadata& metadata);
        
        static Ref<StaticMesh> ImportStaticMesh(AssetHandle handle, const AssetMetadata& metadata);
        
        // Load from filepath
        static Ref<Mesh> LoadMesh(const std::filesystem::path& path);
        
    private:
        static std::vector<MeshData> ParseGltfToMeshData(const std::filesystem::path& path, std::vector<MaterialData>& outMaterials, Skeleton& outSkeleton, std::vector<Ref<AnimationSequence>>& outAnimations, std::vector<LightNodeData>& outLights, std::vector<MeshNodeData>& outNodes, std::vector<CameraNodeData>& outCameras);
    };
}
