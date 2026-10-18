#pragma once
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    // Meshes made at run time from vertex arrays (docs/Runtime_And_Packaging_Plan_2026-10.md §4: a script's Mesh, a voxel
    // chunk, a procedural rock). Each one is a StaticMesh with one submesh that the active asset manager holds as a memory
    // asset, so a MeshComponent naming its handle draws, casts shadows, is ray traced, picked and outlined like an imported
    // mesh. New geometry swaps in under the same handle: entities drawing the mesh register again in the same frame.
    //
    // Main thread (scripts run there); the meshes live until Destroy, or DestroyAll when Play stops.
    class DynamicMeshes
    {
    public:
        // The triangles last given to SetData, kept for colliders (MeshCollider3DComponent).
        struct Triangles
        {
            std::vector<glm::vec3> Positions;
            std::vector<uint32_t> Indices;
        };

        // A new, empty mesh (white, rough, opaque); 0 without an active project.
        static AssetHandle Create();
        // Replaces the geometry: counter-clockwise front faces, three indices per triangle. Normals (smooth, area-weighted)
        // and UVs (zero) may be left empty; with UVs the tangents are generated from them. False, leaving the mesh as it was,
        // when the arrays do not fit together (an index out of range, a count that is not a multiple of 3, a normal / UV
        // array of another length than the positions).
        static bool SetData(AssetHandle mesh, std::span<const glm::vec3> positions, std::span<const uint32_t> indices,
                            std::span<const glm::vec3> normals = {}, std::span<const glm::vec2> uvs = {});
        // No geometry (the mesh stays, drawing nothing).
        static void Clear(AssetHandle mesh);
        // The mesh's own material: base color (alpha below 1 is drawn see-through), metallic, roughness, emission.
        static void SetColor(AssetHandle mesh, const glm::vec4& color, float metallic, float roughness, const glm::vec3& emissive = glm::vec3(0.0f));
        // A material asset (.nmat) instead of the own color; 0 goes back to it.
        static void SetMaterial(AssetHandle mesh, AssetHandle material);

        static bool Exists(AssetHandle mesh);
        static const Triangles* GetTriangles(AssetHandle mesh);
        static void Destroy(AssetHandle mesh);
        static void DestroyAll();

        // The meshes whose geometry, material or existence changed since the last call (Scene::SyncGpuScene registers the
        // entities drawing them again).
        static void ConsumeChanged(std::vector<AssetHandle>& outMeshes);
    };
}
