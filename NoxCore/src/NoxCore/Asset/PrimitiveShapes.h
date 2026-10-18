#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

#include <glm/glm.hpp>

namespace Nox
{
    // The editor's basic shapes (UE's Place Actors > Shapes, Godot's primitive meshes), authored in meters like any glTF and
    // centered on the origin, +Y up. Cube, plane, cylinder, cone and pyramid are 1 m across and 1 m tall (UE's 100 cm shapes);
    // the sphere is 1 m in diameter; the capsule is 1 m wide and 2 m tall (Unity's / Godot's capsule).
    enum class PrimitiveShape : uint8_t
    {
        Cube,
        Sphere,
        Cylinder,
        Cone,
        Pyramid,
        Plane,
        Capsule,
        Count
    };

    struct PrimitiveVertex
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
        glm::vec2 TexCoord{ 0.0f };
    };

    // Counter-clockwise front faces (glTF), outward normals; hard edges get split vertices.
    struct PrimitiveGeometry
    {
        std::vector<PrimitiveVertex> Vertices;
        std::vector<uint32_t> Indices;
    };

    class PrimitiveShapes
    {
    public:
        static const char* GetName(PrimitiveShape shape);
        // segments: around the round shapes (clamped to 3..256); ignored by the flat ones.
        static PrimitiveGeometry Generate(PrimitiveShape shape, uint32_t segments = 32);

        // Writes <directory>/<Name>.gltf and its buffer <Name>.bin (an ordinary glTF the mesh importer takes like any model,
        // with one grey PBR material). Returns the .gltf path, empty when a file could not be written.
        static std::filesystem::path WriteGltf(PrimitiveShape shape, const std::filesystem::path& directory, uint32_t segments = 32);
    };
}
