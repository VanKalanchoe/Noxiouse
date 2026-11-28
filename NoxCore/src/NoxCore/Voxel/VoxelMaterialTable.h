#pragma once
// The project's voxel materials (docs/Voxel_Plan_2026-11.md §5): what a voxel's material id means -- its look (a colour, or block
// textures for 50 cm - 1 m blocks: top / side / bottom), its light, and its gameplay (hardness for brushes, footstep surface). Id 0 is
// air and always the first entry. Chunk palettes store these ids; a stamped .vox adds entries or maps to the nearest (VoxelStamp.h).

#include "VoxelTypes.h"

#include <string>
#include <vector>

namespace Nox::Voxel
{
    struct VoxelMaterial
    {
        std::string Name;
        glm::vec4 BaseColor{ 1.0f };   // linear RGB, alpha = opacity
        float Roughness = 1.0f;
        float Metallic = 0.0f;
        glm::vec3 Emission{ 0.0f };    // linear RGB
        float EmissionStrength = 0.0f; // the emissive colour's multiplier (as a mesh material's Emissive Strength)
        float Hardness = 1.0f;         // how long a brush takes to remove it (VX5); 0: removed at once
        std::string FootstepSurface;   // the footstep / impact sound surface (VX4)
        // Block textures (AssetHandles of textures; 0: the base colour instead) for block-style worlds, sampled at the hit point on the
        // voxel face: top (+Y), side, bottom (-Y).
        uint64_t TopTexture = 0;
        uint64_t SideTexture = 0;
        uint64_t BottomTexture = 0;

        bool IsEmissive() const { return EmissionStrength > 0.0f && (Emission.r > 0.0f || Emission.g > 0.0f || Emission.b > 0.0f); }
        bool operator==(const VoxelMaterial& other) const = default;
    };

    class VoxelMaterialTable
    {
    public:
        VoxelMaterialTable(); // just air

        // The new entry's id; Air when the table is full (65535 materials).
        MaterialId Add(const VoxelMaterial& material);
        const VoxelMaterial& Get(MaterialId id) const { return m_Materials[id < m_Materials.size() ? id : 0]; }
        VoxelMaterial& Edit(MaterialId id) { return m_Materials[id]; }
        size_t Size() const { return m_Materials.size(); }
        // The first material with this name; Air when none.
        MaterialId Find(const std::string& name) const;
        // The non-air material whose look is closest (colour distance in linear RGB, emission and metal mismatches weighed in); Air when
        // the table holds only air.
        MaterialId Nearest(const VoxelMaterial& look) const;

        // YAML (a project file); Deserialize refuses a malformed table (`error` says what).
        std::string Serialize() const;
        static bool Deserialize(const std::string& text, VoxelMaterialTable& out, std::string& error);

    private:
        std::vector<VoxelMaterial> m_Materials;
    };
}
