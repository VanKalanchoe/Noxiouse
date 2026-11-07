// A mesh asset's triangles for the acoustic scene (SpatialAudio.h): its cooked file read on a worker -- the original triangles (LOD 0
// clusters, the copies streaming pages hold welded back, as physics' mesh colliders and HLODs take them) -- each submesh simplified
// (meshoptimizer: sound needs walls, not their bevels; Steam Audio recommends simplified geometry), tagged with its acoustic material.

#include "SpatialAudio.h"

#include <cstring>
#include <string>
#include <unordered_map>

#include <meshoptimizer.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/MeshImporter.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    namespace
    {
        constexpr size_t kSimplifyAbove = 256;      // triangles: smaller submeshes stay as they are
        constexpr float kSimplifyRatio = 0.25f;     // keep about a quarter of the triangles ...
        constexpr float kSimplifyError = 0.01f;     // ... moving the surface by at most 1 % of the submesh's size

        std::shared_ptr<AcousticMesh> ReadMesh(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata, uint32_t firstSubmesh,
                                               uint32_t submeshCount, const std::vector<AcousticMaterial>& materials)
        {
            std::optional<CookedMesh> cooked = MeshImporter::ReadCookedMesh(assetDirectory, metadata);
            if (!cooked)
            {
                NOX_CORE_WARN("Steam Audio: the cooked mesh {} cannot be read; it is left out of the acoustic scene", metadata.FilePath.generic_string());
                return nullptr;
            }
            auto mesh = std::make_shared<AcousticMesh>();
            const size_t first = std::min<size_t>(firstSubmesh, cooked->Submeshes.size());
            const size_t last = submeshCount == UINT32_MAX ? cooked->Submeshes.size() : std::min<size_t>(first + std::max(submeshCount, 1u), cooked->Submeshes.size());
            for (size_t submesh = first; submesh < last; ++submesh)
            {
                const MeshData& data = cooked->Submeshes[submesh];
                std::vector<glm::vec3> vertices;
                std::vector<uint32_t> indices;
                std::unordered_map<std::string, uint32_t> welded;
                for (size_t draw = 0; draw < data.Draws.size() && draw < data.Bounds.size(); ++draw)
                {
                    if (data.Bounds[draw].lodLevel != 0)
                        continue;
                    const shaderio::MeshletDraw& cluster = data.Draws[draw];
                    for (uint32_t triangle = 0; triangle < cluster.triangleCount; ++triangle)
                    {
                        for (uint32_t corner = 0; corner < 3; ++corner)
                        {
                            const glm::vec3 position = data.Vertices[cluster.globalVertexOffset +
                                data.MeshletVertices[cluster.vertexOffset + data.MeshletTriangles[cluster.triangleOffset + triangle * 3 + corner]]].pos;
                            auto [found, inserted] = welded.try_emplace(std::string(reinterpret_cast<const char*>(&position), sizeof(position)),
                                                                        static_cast<uint32_t>(vertices.size()));
                            if (inserted)
                                vertices.push_back(position);
                            indices.push_back(found->second);
                        }
                    }
                }
                if (indices.size() / 3 > kSimplifyAbove)
                {
                    std::vector<uint32_t> simplified(indices.size());
                    const size_t target = static_cast<size_t>(static_cast<float>(indices.size() / 3) * kSimplifyRatio) * 3;
                    const size_t count = meshopt_simplify(simplified.data(), indices.data(), indices.size(), &vertices[0].x, vertices.size(), sizeof(glm::vec3),
                                                          target, kSimplifyError, meshopt_SimplifyLockBorder, nullptr);
                    if (count >= 3)
                    {
                        simplified.resize(count);
                        indices.swap(simplified);
                    }
                }
                const AcousticMaterial material = submesh - first < materials.size() ? materials[submesh - first] : AcousticMaterial::Generic;
                const uint32_t base = static_cast<uint32_t>(mesh->Vertices.size());
                mesh->Vertices.insert(mesh->Vertices.end(), vertices.begin(), vertices.end());
                for (uint32_t index : indices)
                    mesh->Indices.push_back(base + index);
                mesh->TriangleMaterials.insert(mesh->TriangleMaterials.end(), indices.size() / 3, material);
            }
            return mesh;
        }
    }

    AcousticMeshRead StartAcousticMeshRead(AssetHandle meshHandle, uint32_t firstSubmesh, uint32_t submeshCount, std::vector<AcousticMaterial> materials)
    {
        const AssetType type = AssetManager::GetAssetType(meshHandle);
        if (type != AssetType::StaticMesh && type != AssetType::Mesh)
            return {};
        // (the metadata and the folder taken here, on the game thread: the worker reads files only)
        const AssetMetadata metadata = AssetManager::GetMetadata(meshHandle);
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        auto job = std::make_shared<TaskFuture<std::shared_ptr<AcousticMesh>>>(
            JobSystem::Get().Async("Acoustic Mesh", JobPriority::Normal,
                [assetDirectory, metadata, firstSubmesh, submeshCount, materials = std::move(materials)](const CancellationToken&)
                {
                    return ReadMesh(assetDirectory, metadata, firstSubmesh, submeshCount, materials);
                }));
        AcousticMeshRead read;
        read.IsReady = [job]() { return job->IsReady(); };
        read.Get = [job]() -> std::shared_ptr<AcousticMesh>
        {
            try
            {
                return job->Get();
            }
            catch (const std::exception& exception)
            {
                NOX_CORE_ERROR("Steam Audio: reading an acoustic mesh failed: {}", exception.what());
                return nullptr;
            }
        };
        return read;
    }
}
