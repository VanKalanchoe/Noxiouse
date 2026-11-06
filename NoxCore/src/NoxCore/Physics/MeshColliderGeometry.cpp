#include "MeshColliderGeometry.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <spdlog/spdlog.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/MeshImporter.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Physics/PhysicsEngine.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Components.h"

namespace Nox
{
    bool ReadMeshColliderTriangles(const MeshCollider3DComponent& collider, std::vector<glm::vec3>& vertices, std::vector<uint32_t>& indices)
    {
        vertices.clear();
        indices.clear();
        const AssetType type = AssetManager::GetAssetType(collider.Mesh);
        if (type != AssetType::StaticMesh && type != AssetType::Mesh)
            return false;
        const std::optional<CookedMesh> cooked = MeshImporter::ReadCookedMesh(Project::GetActiveAssetDirectory(), AssetManager::GetMetadata(collider.Mesh));
        if (!cooked)
        {
            SPDLOG_WARN("Mesh collider: the cooked mesh of asset {} cannot be read", static_cast<uint64_t>(collider.Mesh));
            return false;
        }

        std::unordered_map<std::string, uint32_t> welded;
        const size_t first = std::min<size_t>(collider.SubmeshIndex, cooked->Submeshes.size());
        const size_t last = collider.SubmeshCount == UINT32_MAX ? cooked->Submeshes.size()
                                                                : std::min<size_t>(first + collider.SubmeshCount, cooked->Submeshes.size());
        for (size_t submesh = first; submesh < last; ++submesh)
        {
            const MeshData& data = cooked->Submeshes[submesh];
            for (size_t index = 0; index < data.Draws.size() && index < data.Bounds.size(); ++index)
            {
                if (data.Bounds[index].lodLevel != 0)
                    continue;
                const shaderio::MeshletDraw& draw = data.Draws[index];
                for (uint32_t triangle = 0; triangle < draw.triangleCount; ++triangle)
                {
                    for (uint32_t corner = 0; corner < 3; ++corner)
                    {
                        const glm::vec3 position = data.Vertices[draw.globalVertexOffset +
                            data.MeshletVertices[draw.vertexOffset + data.MeshletTriangles[draw.triangleOffset + triangle * 3 + corner]]].pos;
                        auto [found, inserted] = welded.try_emplace(std::string(reinterpret_cast<const char*>(&position), sizeof(position)),
                                                                    static_cast<uint32_t>(vertices.size()));
                        if (inserted)
                            vertices.push_back(position);
                        indices.push_back(found->second);
                    }
                }
            }
        }
        return !indices.empty();
    }

    const std::vector<glm::vec3>& MeshColliderWireframe(const MeshCollider3DComponent& collider, bool convex)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, std::vector<glm::vec3>> cache;
        const std::string key = std::to_string(static_cast<uint64_t>(collider.Mesh)) + ':' + std::to_string(collider.SubmeshIndex) + ':' +
                                std::to_string(collider.SubmeshCount) + (convex ? ":hull" : ":mesh");
        std::lock_guard lock(mutex);
        if (const auto found = cache.find(key); found != cache.end())
            return found->second;
        std::vector<glm::vec3>& lines = cache[key]; // a mesh that cannot be read is not tried again this session

        std::vector<glm::vec3> vertices;
        std::vector<uint32_t> indices;
        if (!ReadMeshColliderTriangles(collider, vertices, indices))
            return lines;

        if (!convex)
        {
            // Each edge once (two triangles share most of them).
            std::unordered_set<uint64_t> edges;
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t a = indices[i + k], b = indices[i + (k + 1) % 3];
                    const uint64_t edge = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
                    if (edges.insert(edge).second)
                    {
                        lines.push_back(vertices[a]);
                        lines.push_back(vertices[b]);
                    }
                }
            }
            return lines;
        }

        // The hull physics builds (JoltPhysics3DScene::MeshColliderShape makes the same one, from the same points in Jolt's meters
        // with its default settings): its faces' edges. Its points are kept relative to its centre of mass. Jolt's allocator must be
        // up (the editor draws this outside Play too).
        PhysicsEngine::Init();
        const float metersPerWorld = 1.0f / std::max(WorldUnits::PerMeter(), 1e-6f);
        JPH::Array<JPH::Vec3> points;
        points.reserve(vertices.size());
        for (const glm::vec3& vertex : vertices)
            points.push_back(JPH::Vec3(vertex.x, vertex.y, vertex.z) * metersPerWorld);
        const JPH::Shape::ShapeResult result = JPH::ConvexHullShapeSettings(points).Create();
        if (result.HasError())
        {
            SPDLOG_WARN("Mesh collider of asset {}: no convex hull ({})", static_cast<uint64_t>(collider.Mesh), result.GetError().c_str());
            return lines;
        }
        const auto* hull = static_cast<const JPH::ConvexHullShape*>(result.Get().GetPtr());
        const JPH::Vec3 centre = hull->GetCenterOfMass();
        std::vector<JPH::uint> face;
        for (JPH::uint f = 0; f < hull->GetNumFaces(); ++f)
        {
            face.resize(hull->GetNumVerticesInFace(f));
            const JPH::uint count = hull->GetFaceVertices(f, static_cast<JPH::uint>(face.size()), face.data());
            for (JPH::uint k = 0; k < count; ++k)
            {
                const JPH::Vec3 a = (hull->GetPoint(face[k]) + centre) / metersPerWorld;
                const JPH::Vec3 b = (hull->GetPoint(face[(k + 1) % count]) + centre) / metersPerWorld;
                lines.push_back(glm::vec3(a.GetX(), a.GetY(), a.GetZ()));
                lines.push_back(glm::vec3(b.GetX(), b.GetY(), b.GetZ()));
            }
        }
        return lines;
    }
}
