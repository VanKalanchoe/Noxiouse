#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace Nox
{
    struct MeshCollider3DComponent;

    // A mesh collider's triangles as physics takes them: the original triangles of the cooked mesh (LOD level 0 clusters, as the
    // ray tracing BLAS and HLOD take them), the copies of a vertex that streaming pages hold welded back into one. In the entity's
    // space, world units, unscaled. False when the mesh is not a mesh asset or its cooked file cannot be read.
    // `triangleSubmeshes` (optional): each triangle's submesh index in the mesh (its material).
    bool ReadMeshColliderTriangles(const MeshCollider3DComponent& collider, std::vector<glm::vec3>& vertices, std::vector<uint32_t>& indices,
                                   std::vector<uint32_t>* triangleSubmeshes = nullptr);

    // The collider's wireframe as line segments (pairs of points), entity space, world units, unscaled: each edge of its triangles
    // once, or with `convex` the edges of the convex hull physics builds from them (Jolt's, so the drawing is the simulated shape).
    // Cached per mesh, submesh range and kind; empty when the mesh cannot be read.
    const std::vector<glm::vec3>& MeshColliderWireframe(const MeshCollider3DComponent& collider, bool convex);
}
