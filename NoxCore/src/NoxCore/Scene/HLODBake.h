#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace Nox::HLOD
{
    // The geometry and material bake of an HLOD (docs/Engine_Architecture_Plan_2026.md §5.13.2), Unreal 5's "Approximated Mesh"
    // HLOD: a cell's meshes merged, simplified, laid out in a new UV atlas, and their materials baked into that atlas by projection
    // onto the source surfaces (as Unreal's ProxyLOD bakes, on the CPU). Engine independent (glm, meshoptimizer, xatlas), any thread.

    // A texture in memory, RGBA8 as stored (no colour space conversion), sampled bilinearly with wrapping.
    struct Texture
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        std::vector<uint8_t> Texels; // RGBA8, rows top to bottom

        bool Valid() const { return Width > 0 && Height > 0 && Texels.size() >= size_t(Width) * Height * 4; }
        glm::vec4 Sample(glm::vec2 uv) const; // channels 0..1
    };

    // A source material, the glTF metallic-roughness model (specular-glossiness converted on evaluation).
    struct Material
    {
        glm::vec4 BaseColorFactor{ 1.0f };
        std::shared_ptr<const Texture> BaseColor; // sRGB
        float MetallicFactor = 0.0f;
        float RoughnessFactor = 1.0f;
        std::shared_ptr<const Texture> MetallicRoughness; // G: roughness, B: metallic
        std::shared_ptr<const Texture> Normal;            // tangent space
        bool NormalTwoChannel = false;                     // BC5: z rebuilt from x and y
        glm::vec3 EmissiveFactor{ 0.0f };
        std::shared_ptr<const Texture> Emissive;           // sRGB
        bool SpecularGlossiness = false;                   // then BaseColorFactor is the diffuse factor, Glossiness its gloss
        float Glossiness = 0.0f;
        bool AlphaMask = false;
        float AlphaCutoff = 0.5f;
    };

    // One mesh placed in the world: its vertices in its own space, the transform, its material (an index into the materials).
    struct SourceMesh
    {
        std::vector<glm::vec3> Positions;
        std::vector<glm::vec3> Normals;
        std::vector<glm::vec4> Tangents; // xyz, w: bitangent sign (bitangent = cross(normal, tangent) * w)
        std::vector<glm::vec2> UVs;
        std::vector<uint32_t> Indices;
        glm::mat4 Transform{ 1.0f };
        uint32_t Material = 0;
    };

    struct Settings
    {
        float Error = 50.0f;            // world units: the simplification error, and how far the bake searches for the source
        uint32_t MaxTriangles = 20000;  // over the error's result, the triangles are cut down to this (parts are joined)
        float TexelsPerUnit = 0.04f;    // texels per world unit (cm: 4 per meter)
        uint32_t MinTextureSize = 128;
        uint32_t MaxTextureSize = 2048;
        float HardEdgeAngle = 60.0f;    // degrees between faces where the HLOD's normals are split
        float MinPartSize = 0.0f;       // world units: smaller parts are left out of the geometry (still baked); 0: 2 x Error
        // Runs body(i) for i in [0, count), possibly in parallel. Empty: a plain loop.
        std::function<void(uint32_t count, const std::function<void(uint32_t)>& body)> ParallelFor;
    };

    struct Result
    {
        bool Ok = false;
        std::string Error;

        glm::vec3 Origin{ 0.0f }; // the vertices are relative to it (the HLOD entity stands here)
        std::vector<glm::vec3> Positions;
        std::vector<glm::vec3> Normals;
        std::vector<glm::vec4> Tangents;
        std::vector<glm::vec2> UVs; // 0..1
        std::vector<uint32_t> Indices;
        glm::vec3 BoundsMin{ 0.0f }; // world
        glm::vec3 BoundsMax{ 0.0f };

        uint32_t TextureSize = 0;              // square
        std::vector<uint8_t> BaseColor;        // RGBA8, sRGB, alpha: coverage (alpha-masked sources)
        std::vector<uint8_t> Normal;           // RGBA8, tangent space in the HLOD's own tangent frame
        std::vector<uint8_t> MetallicRoughness;// RGBA8, G: roughness, B: metallic (glTF)
        std::vector<uint8_t> Emissive;         // RGBA8, sRGB, times EmissiveScale; empty when nothing glows
        float EmissiveScale = 0.0f;
        bool Masked = false;                   // some texels are cut out (alpha-masked sources): alpha test the HLOD

        uint32_t SourceTriangles = 0;
        uint32_t BakedTexels = 0; // texels that found their source
        uint32_t SurfaceTexels = 0;
    };

    Result Build(const std::vector<SourceMesh>& meshes, const std::vector<Material>& materials, const Settings& settings);
}
