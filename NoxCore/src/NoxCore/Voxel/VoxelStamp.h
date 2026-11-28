#pragma once
// A MagicaVoxel scene stamped into the voxel world (docs/Voxel_Plan_2026-11.md §5, VX1): every visible model's voxels written at its
// place in the scene graph, one .vox voxel = one world voxel, in engine axes (MagicaVoxel's x, y, z become x, z, -y, as for meshes:
// VoxMesher.h). Its palette entries become voxel materials: new table entries, or the nearest material the table already has.
// Air in the model leaves the world's voxels as they are.

#include "VoxReader.h"
#include "VoxelMaterialTable.h"
#include "VoxelWorld.h"

#include <array>

namespace Nox::Voxel
{
    enum class StampMaterials : uint8_t
    {
        AddEntries, // a table entry per palette entry used (an existing one reused when it looks the same and has the same name)
        Nearest     // the table's nearest look per palette entry (added when the table holds only air)
    };

    enum class StampPivot : uint8_t
    {
        BottomCenter, // the stamped voxels' bounds centred on Origin in x / z, their bottom on Origin's y (as the import's default)
        FileOrigin    // MagicaVoxel's own coordinates, moved by Origin
    };

    struct StampOptions
    {
        glm::ivec3 Origin{ 0 }; // world voxel coordinates
        StampPivot Pivot = StampPivot::BottomCenter;
        StampMaterials Materials = StampMaterials::AddEntries;
        std::string NamePrefix = "Vox"; // new entries are called "<prefix> <palette index>"
        float EmissionScale = 1.0f;     // as the import's Emission Scale (emission = _emit x 2^(_flux + 1) x this)
    };

    struct StampResult
    {
        uint64_t Voxels = 0;                   // voxels written
        std::array<MaterialId, 256> Mapping{}; // palette index -> material id (Air for entries not used)
        glm::ivec3 Min{ 0 };                   // world voxel bounds written (inclusive)
        glm::ivec3 Max{ -1 };
    };

    // The voxel material a .vox palette entry describes (colour linearised from sRGB, roughness, metal, emission, glass as opacity).
    VoxelMaterial MaterialFromVox(const Vox::Scene& scene, uint8_t paletteIndex, float emissionScale);
    // The world cell of model voxel (x, y, z) of `instance` (MagicaVoxel: R (v + 0.5 - size / 2) + t is its centre), engine axes.
    glm::ivec3 StampedCell(const Vox::Scene& scene, const Vox::Instance& instance, int32_t x, int32_t y, int32_t z);

    StampResult StampVoxScene(VoxelWorld& world, VoxelMaterialTable& table, const Vox::Scene& scene, const StampOptions& options);
}
