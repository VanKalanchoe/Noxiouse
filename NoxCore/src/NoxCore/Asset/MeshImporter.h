#pragma once
#include <atomic>
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

    // A submesh whose clusters are not built yet: its triangles, and whether a glTF skin deforms it.
    struct PendingGeometry
    {
        std::vector<uint32_t> Indices;
        bool Skinned = false;
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
        // What a glTF contains, read before importing so the content decides the asset types (Unreal: skinned meshes
        // become skeletal meshes, everything else static meshes). Reads the whole file; call once per import.
        struct GltfContent
        {
            bool HasSkinnedMeshes = false;   // a node with a mesh and a skin
            bool HasStaticMeshes = false;    // a node with a mesh and no skin (or a mesh no node uses)
        };
        static GltfContent InspectGltf(const std::filesystem::path& sourcePath);

        // Any thread. Cooks the asset's glTF source into its .nmesh / .nsmesh (with .hash), the skeleton and clips, and
        // one .nmat per material that does not exist yet; false when the source cannot be read.
        static bool CookMesh(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata);
        // Do Not Combine (Unreal): every static glTF mesh of the file cooked as an asset of its own (.nsmesh) in `directory`
        // (relative to the asset directory), sharing the materials named after `materialBase`. Any thread; one glTF parse for all.
        struct SplitMesh
        {
            std::string Name;
            std::filesystem::path FilePath; // relative to the asset directory
            int32_t MeshIndex = -1;
        };
        // `total` / `done` (optional) report progress in meshes: total is set once the file is parsed.
        static std::vector<SplitMesh> CookSplitMeshes(const std::filesystem::path& assetDirectory, const AssetMetadata& materialBase,
                                                      const std::filesystem::path& directory, std::atomic<uint32_t>* total = nullptr,
                                                      std::atomic<uint32_t>* done = nullptr);
        // Where a model's material asset lives (relative to the asset directory), from the model's cooked file path.
        static std::filesystem::path MaterialAssetPath(const std::filesystem::path& meshFilePath, const MaterialData& material, size_t index);

        // AssetMetadata filepath is relative to project asset directory
        static Ref<Mesh> ImportMesh(AssetHandle handle, const AssetMetadata& metadata);
        
        static Ref<StaticMesh> ImportStaticMesh(AssetHandle handle, const AssetMetadata& metadata);
        
        // Load from filepath
        static Ref<Mesh> LoadMesh(const std::filesystem::path& path);
        
    private:
        // Main thread: uploads every submesh now (synchronous loads).
        static Ref<Asset> uploadMeshAsset(AssetType type, CookedMesh& cooked);
        // With outPending the meshlets/clusters are NOT built: each submesh keeps its index buffer there (parallel to the
        // result), so the cook can merge submeshes before building them.
        static std::vector<MeshData> ParseGltfToMeshData(const std::filesystem::path& path, std::vector<MaterialData>& outMaterials, Skeleton& outSkeleton, std::vector<Ref<AnimationSequence>>& outAnimations, std::vector<LightNodeData>& outLights, std::vector<MeshNodeData>& outNodes, std::vector<CameraNodeData>& outCameras, std::vector<PendingGeometry>* outPending = nullptr);
    };
}
