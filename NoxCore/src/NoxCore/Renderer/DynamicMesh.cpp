#include "DynamicMesh.h"

#include <unordered_map>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/MeshImporter.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Mesh.h"
#include "NoxCore/Renderer/Renderer.h"

namespace Nox
{
    namespace
    {
        struct DynamicMesh
        {
            Ref<StaticMesh> Mesh;
            DynamicMeshes::Triangles Triangles;
        };
        // Never destroyed: a mesh still alive when the process exits must not free GPU geometry after the renderer is gone.
        std::unordered_map<AssetHandle, DynamicMesh>& s_Meshes = *new std::unordered_map<AssetHandle, DynamicMesh>();
        std::vector<AssetHandle> s_Changed;

        DynamicMesh* Find(AssetHandle handle, const char* call)
        {
            auto found = s_Meshes.find(handle);
            if (found != s_Meshes.end())
                return &found->second;
            NOX_CORE_ERROR("DynamicMeshes::{}: no mesh {} (destroyed, or made before Play started)", call, static_cast<uint64_t>(handle));
            return nullptr;
        }

        // Any unit tangent perpendicular to the normal (Duff et al. 2017, "Building an Orthonormal Basis, Revisited"): geometry
        // without UVs still needs one, the shading builds its tangent frame from it.
        glm::vec4 TangentFor(const glm::vec3& normal)
        {
            const float sign = normal.z >= 0.0f ? 1.0f : -1.0f;
            const float a = -1.0f / (sign + normal.z);
            return glm::vec4(1.0f + sign * normal.x * normal.x * a, sign * normal.x * normal.y * a, -sign * normal.x, 1.0f);
        }
    }

    AssetHandle DynamicMeshes::Create()
    {
        if (!Project::GetActive())
            return 0;

        Ref<StaticMesh> mesh = CreateRef<StaticMesh>();
        MaterialData material;
        material.Name = "Dynamic Mesh";
        mesh->m_Materials = { material };
        mesh->m_MaterialAssets = { AssetHandle(0) };

        const AssetHandle handle;
        Project::GetActiveAssetManager().AddMemoryAsset(handle, Ref<Asset>(mesh));
        s_Meshes.emplace(handle, DynamicMesh{ mesh, {} });
        return handle;
    }

    bool DynamicMeshes::SetData(AssetHandle handle, std::span<const glm::vec3> positions, std::span<const uint32_t> indices,
                                std::span<const glm::vec3> normals, std::span<const glm::vec2> uvs)
    {
        DynamicMesh* entry = Find(handle, "SetData");
        if (!entry)
            return false;
        if (indices.size() % 3 != 0)
        {
            NOX_CORE_ERROR("DynamicMeshes::SetData: {} indices is not whole triangles (3 per triangle)", indices.size());
            return false;
        }
        if ((!normals.empty() && normals.size() != positions.size()) || (!uvs.empty() && uvs.size() != positions.size()))
        {
            NOX_CORE_ERROR("DynamicMeshes::SetData: {} positions but {} normals and {} UVs (one per position, or none)", positions.size(),
                           normals.size(), uvs.size());
            return false;
        }
        for (uint32_t index : indices)
        {
            if (index >= positions.size())
            {
                NOX_CORE_ERROR("DynamicMeshes::SetData: index {} is past the {} positions", index, positions.size());
                return false;
            }
        }
        if (positions.empty() || indices.empty())
        {
            Clear(handle);
            return true;
        }

        MeshData data;
        data.Name = "Dynamic Mesh";
        data.Vertices.resize(positions.size());
        for (size_t index = 0; index < positions.size(); ++index)
        {
            shaderio::Vertex& vertex = data.Vertices[index];
            vertex.pos = positions[index];
            if (!normals.empty())
                vertex.normal = normals[index];
            if (!uvs.empty())
                vertex.uv0 = uvs[index];
        }
        if (normals.empty())
        {
            // Smooth: each vertex takes the area-weighted sum of its triangles' normals (the cross product's length is twice
            // the area). A hard edge needs its own vertices on each side, as in any mesh.
            for (size_t corner = 0; corner < indices.size(); corner += 3)
            {
                shaderio::Vertex& a = data.Vertices[indices[corner]];
                shaderio::Vertex& b = data.Vertices[indices[corner + 1]];
                shaderio::Vertex& c = data.Vertices[indices[corner + 2]];
                const glm::vec3 face = glm::cross(glm::vec3(b.pos) - glm::vec3(a.pos), glm::vec3(c.pos) - glm::vec3(a.pos));
                a.normal += face;
                b.normal += face;
                c.normal += face;
            }
        }
        for (shaderio::Vertex& vertex : data.Vertices)
        {
            const float length = glm::length(glm::vec3(vertex.normal));
            vertex.normal = length > 1e-12f ? glm::vec3(vertex.normal) / length : glm::vec3(0.0f, 1.0f, 0.0f);
            if (uvs.empty())
                vertex.tangent = TangentFor(vertex.normal);
        }
        MeshImporter::BuildRuntimeGeometry(data, std::vector<uint32_t>(indices.begin(), indices.end()), !uvs.empty());
        if (data.Draws.empty())
        {
            Clear(handle);
            return true;
        }

        // The ray tracing structure is built opaque unless the material may let light through (alpha mask / blend).
        StaticMesh& mesh = *entry->Mesh;
        bool opaque = mesh.m_Materials[0].Mode == AlphaMode::Opaque;
        if (const AssetHandle material = mesh.m_MaterialAssets[0]; material != 0)
        {
            if (const Material* loaded = AssetManager::FindLoadedAsset<Material>(material))
                opaque = loaded->GetData().Mode == AlphaMode::Opaque;
        }
        const MeshHandle uploaded = Renderer::UploadMesh(data, opaque);

        // The new geometry in, the old freed: its instances stop drawing now and their entities register again this frame.
        std::vector<MeshHandle> previous = std::move(mesh.m_SubMeshes);
        mesh.m_SubMeshes = { uploaded };
        mesh.m_SubmeshNames = { data.Name };
        for (const MeshHandle& old : previous)
        {
            if (old.IsValid())
                Renderer::UnloadMesh(old);
        }

        entry->Triangles.Positions.assign(positions.begin(), positions.end());
        entry->Triangles.Indices.assign(indices.begin(), indices.end());
        s_Changed.push_back(handle);
        return true;
    }

    void DynamicMeshes::Clear(AssetHandle handle)
    {
        DynamicMesh* entry = Find(handle, "Clear");
        if (!entry)
            return;
        entry->Mesh->ReleaseGeometry();
        entry->Triangles = {};
        s_Changed.push_back(handle);
    }

    void DynamicMeshes::SetColor(AssetHandle handle, const glm::vec4& color, float metallic, float roughness, const glm::vec3& emissive)
    {
        DynamicMesh* entry = Find(handle, "SetColor");
        if (!entry)
            return;
        MaterialData& material = entry->Mesh->m_Materials[0];
        const AlphaMode previousMode = material.Mode;
        material.BaseColorFactor = color;
        material.MetallicFactor = glm::clamp(metallic, 0.0f, 1.0f);
        material.RoughnessFactor = glm::clamp(roughness, 0.0f, 1.0f);
        material.EmissiveFactor = emissive;
        material.Mode = color.a < 1.0f ? AlphaMode::Blend : AlphaMode::Opaque;
        Renderer::MarkMeshMaterialsChanged();
        // Opaque and see-through instances are drawn by different passes: those register again.
        if (material.Mode != previousMode)
            s_Changed.push_back(handle);
    }

    void DynamicMeshes::SetMaterial(AssetHandle handle, AssetHandle material)
    {
        DynamicMesh* entry = Find(handle, "SetMaterial");
        if (!entry)
            return;
        entry->Mesh->m_MaterialAssets = { material };
        if (material != 0)
            AssetManager::RequestAsset(material);
        s_Changed.push_back(handle);
    }

    bool DynamicMeshes::Exists(AssetHandle handle)
    {
        return s_Meshes.contains(handle);
    }

    const DynamicMeshes::Triangles* DynamicMeshes::GetTriangles(AssetHandle handle)
    {
        auto found = s_Meshes.find(handle);
        return found != s_Meshes.end() && !found->second.Triangles.Indices.empty() ? &found->second.Triangles : nullptr;
    }

    void DynamicMeshes::Destroy(AssetHandle handle)
    {
        auto found = s_Meshes.find(handle);
        if (found == s_Meshes.end())
            return;
        found->second.Mesh->ReleaseGeometry();
        if (Project::GetActive())
            Project::GetActiveAssetManager().RemoveMemoryAsset(handle);
        s_Meshes.erase(found);
        s_Changed.push_back(handle);
    }

    void DynamicMeshes::DestroyAll()
    {
        std::vector<AssetHandle> handles;
        handles.reserve(s_Meshes.size());
        for (const auto& [handle, mesh] : s_Meshes)
            handles.push_back(handle);
        for (AssetHandle handle : handles)
            Destroy(handle);
    }

    void DynamicMeshes::ConsumeChanged(std::vector<AssetHandle>& outMeshes)
    {
        outMeshes.swap(s_Changed);
        s_Changed.clear();
    }
}
