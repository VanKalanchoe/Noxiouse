#include "SkeletalMeshImporter.h"

#include "MeshSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Renderer.h"

namespace Nox
{
    Ref<SkeletalMesh> SkeletalMeshImporter::ImportSkeletalMesh(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::filesystem::path cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;

        NOX_CORE_INFO("SkeletalMeshImporter::ImportSkeletalMesh loading skeletal mesh from {}", cookedPath.string());

        Ref<SkeletalMesh> skeletalMeshAsset = LoadSkeletalMesh(cookedPath);
        if (skeletalMeshAsset)
        {
            skeletalMeshAsset->Handle = handle;
        }

        return skeletalMeshAsset;
    }

    Ref<SkeletalMesh> SkeletalMeshImporter::LoadSkeletalMesh(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("SkeletalMeshImporter::LoadSkeletalMesh - File does not exist: {}", path.string());
            return {};
        }

        std::vector<MeshData> meshDataList;
        std::vector<MaterialData> materialDataList;

        bool success = MeshSerializer::DeserializeMesh(path, meshDataList, materialDataList);
        if (!success)
        {
            NOX_CORE_ASSERT(false, "SkeletalMeshImporter::LoadSkeletalMesh - Failed to deserialize skeletal mesh file: {}", path.string());
            return {};
        }

        Ref<SkeletalMesh> skeletalMeshAsset = CreateRef<SkeletalMesh>();

        for (const auto& data : meshDataList)
        {
            MeshHandle subMeshHandle = Renderer::UploadMesh(data);
            skeletalMeshAsset->m_SubMeshes.push_back(subMeshHandle);
            skeletalMeshAsset->m_SubmeshNames.push_back(data.Name);
        }
        skeletalMeshAsset->m_Materials = std::move(materialDataList);

        return skeletalMeshAsset;
    }
}