#include "Mesh.h"
#include "NoxCore/Asset/AssetManager.h"
#include <algorithm>

#include "Renderer.h"

namespace Nox
{
    StaticMesh::~StaticMesh()
    {
        // When the Mesh object dies, release all its submesh allocations from the GPU allocators
        for (const MeshHandle& handle : m_SubMeshes)
        {
            if (handle.IsValid())
            {
                Renderer::UnloadMesh(handle); // Static helper calling s_Instance->UnloadMeshGeometry(handle)
            }
        }
    }
    
    void StaticMesh::ReleaseGeometry()
    {
        for (const MeshHandle& handle : m_SubMeshes)
        {
            if (handle.IsValid())
                Renderer::UnloadMesh(handle);
        }
        m_SubMeshes.clear();
        m_SubmeshNames.clear();
    }

    Mesh::~Mesh()
    {
        // When the Mesh object dies, release all its submesh allocations from the GPU allocators
        for (const MeshHandle& handle : m_SubMeshes)
        {
            if (handle.IsValid())
            {
                Renderer::UnloadMesh(handle); // Static helper calling s_Instance->UnloadMeshGeometry(handle)
            }
        }
    }

    std::vector<std::pair<std::string, float>> GetMeshMorphTargets(AssetHandle mesh)
    {
        std::vector<std::pair<std::string, float>> targets;
        auto collect = [&](const auto* asset)
        {
            if (!asset)
                return;
            for (const MeshHandle& submesh : asset->GetSubMeshes())
            {
                if (!submesh.morphs)
                    continue;
                for (size_t target = 0; target < submesh.morphs->Names.size(); ++target)
                {
                    const std::string& name = submesh.morphs->Names[target];
                    if (std::none_of(targets.begin(), targets.end(), [&](const auto& known) { return known.first == name; }))
                        targets.emplace_back(name, submesh.morphs->DefaultWeights[target]);
                }
            }
        };
        const AssetType type = AssetManager::GetAssetType(mesh);
        if (type == AssetType::Mesh)
            collect(AssetManager::FindLoadedAsset<Mesh>(mesh));
        else if (type == AssetType::StaticMesh)
            collect(AssetManager::FindLoadedAsset<StaticMesh>(mesh));
        return targets;
    }
}
