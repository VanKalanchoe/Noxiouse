#pragma once
// A MagicaVoxel scene (VoxReader) as triangle meshes (docs/Voxel_Plan_2026-11.md §4, VX0): per model its visible faces, merged into
// rectangles per palette entry (greedy meshing), opaque and transparent apart; per placed model a node with its transform. In the
// engine's axes (Y up: MagicaVoxel's x, y, z become x, z, -y -- a rotation, so nothing is mirrored) and in voxels (one voxel = 1;
// the import scale makes that the voxel size).
//
// Which faces exist: a voxel shows a face towards a neighbour cell that is empty, or that is transparent while the voxel itself is
// not (glass shows the opaque voxels behind it). Two transparent voxels touching show nothing between them, nor two opaque ones.

#include "VoxReader.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Nox::Vox
{
    // Where a mesh's origin sits (the import's Pivot option).
    enum class Pivot : uint8_t
    {
        BottomCenter = 0, // centre of the solid voxels' bounds, at their bottom: the model stands on the ground where it is placed
        Center,           // centre of the solid voxels' bounds
        FileOrigin        // MagicaVoxel's own: each model at the centre of its box (size / 2, rounded down), the scene at the file's origin
    };

    struct MeshOptions
    {
        Pivot Origin = Pivot::BottomCenter;
        // Faces of one palette entry in one plane merged into rectangles (fewer triangles; T-junctions where rectangles of different
        // sizes meet). Off: one quad per voxel face.
        bool MergeFaces = true;
    };

    // Triangles of one kind (opaque or transparent) of one model; each vertex knows its palette entry (the colour index).
    struct VoxGeometry
    {
        std::vector<glm::vec3> Positions; // voxels, relative to the model's pivot
        std::vector<glm::vec3> Normals;   // axis-aligned, per face
        std::vector<uint8_t> ColorIndices;
        std::vector<uint32_t> Indices;    // counter-clockwise seen from outside

        bool Empty() const { return Indices.empty(); }
        size_t QuadCount() const { return Indices.size() / 6; }
    };

    struct ModelMesh
    {
        VoxGeometry Opaque;
        VoxGeometry Transparent;
        bool HasVoxels = false;
        glm::ivec3 SolidMin{ 0 }; // MagicaVoxel axes, voxel cells (inclusive)
        glm::ivec3 SolidMax{ 0 };
    };

    struct MeshedNode
    {
        std::string Name;
        uint32_t ModelIndex = 0;
        // Relative to the scene root (MeshedScene::RootTranslation), engine axes, voxels.
        glm::vec3 Translation{ 0.0f };
        glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale{ 1.0f }; // (-1, 1, 1) for a mirrored placement (MagicaVoxel allows those)
    };

    struct MeshedScene
    {
        std::vector<ModelMesh> Models; // parallel to Scene::Models
        std::vector<MeshedNode> Nodes; // the visible placements of models with voxels
        glm::vec3 RootTranslation{ 0.0f }; // moves the scene's pivot to the origin
        // The placed scene's bounds after the root translation, engine axes, voxels.
        glm::vec3 BoundsMin{ 0.0f };
        glm::vec3 BoundsMax{ 0.0f };
    };

    // MagicaVoxel axes -> engine axes.
    inline glm::vec3 ToEngineAxes(const glm::vec3& v) { return { v.x, v.z, -v.y }; }

    MeshedScene MeshScene(const Scene& scene, const MeshOptions& options);
    // One model's faces (exposed for tests): positions relative to `pivot` (MagicaVoxel axes, voxels), converted to engine axes.
    void MeshModel(const Model& model, const std::array<Material, 256>& materials, bool mergeFaces, const glm::vec3& pivot, ModelMesh& out);
}
