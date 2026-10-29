#pragma once
#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

#include "NoxCore/Core/UUID.h"

namespace Nox
{
    // World partition (docs/Engine_Architecture_Plan_2026.md §5.13.1, Unreal 5's World Partition): a scene's spatially loaded
    // entities are kept in grid cells, each in its own file, and streamed in and out around the streaming sources like levels.

    // A cell of a hierarchical grid: level 0 cells are the grid's cell size wide, each level up twice as wide (big things live in big
    // cells). Grid 0 holds the world's entities; grid 1 the HLODs (§5.13.2), with a cell size and loading range of its own.
    struct WorldCellCoord
    {
        int32_t Level = 0;
        int32_t X = 0;
        int32_t Z = 0;
        int32_t Grid = 0;

        auto operator<=>(const WorldCellCoord&) const = default;
        // Written out, not defaulted: a defaulted == makes the type "trivially equality comparable", and MSVC's STL then takes its
        // vectorized std::find / std::remove path, which only handles 1, 2, 4 and 8 byte elements (a static_assert on this 12 byte one).
        bool operator==(const WorldCellCoord& other) const { return Level == other.Level && X == other.X && Z == other.Z && Grid == other.Grid; }
    };

    // A cell the scene has a file for: what is in it, as of the last save (the streaming decides by the bounds).
    struct WorldCellInfo
    {
        glm::vec3 BoundsMin{ 0.0f };
        glm::vec3 BoundsMax{ 0.0f };
        uint32_t Entities = 0;
    };

    struct WorldPartitionSettings
    {
        bool Enabled = false;
        float CellSize = 128.0f;     // meters, level 0
        float LoadingRange = 256.0f; // meters from a streaming source to a cell's content bounds (horizontally)
        // The HLOD grid (§5.13.2): Unreal's HLOD layer defaults, twice the world's cells, four times their range.
        float HLODCellSize = 256.0f;
        float HLODLoadingRange = 1024.0f;
        // Building: the simplification error, the triangle budget of one HLOD, the baked texel density.
        float HLODError = 0.5f;          // meters
        uint32_t HLODMaxTriangles = 20000;
        float HLODTexelsPerMeter = 4.0f;
    };

    namespace WorldGrid
    {
        constexpr int32_t MaxLevel = 16;
        constexpr int32_t WorldGridIndex = 0;
        constexpr int32_t HLODGridIndex = 1;
        // Beyond the loading range by this fraction before a loaded cell goes (a source on the edge does not load and unload it).
        constexpr float UnloadHysteresis = 0.1f;

        // The ID a cell's streamed entities carry (LevelComponent): never 0, the same for the same cell in every session.
        UUID CellID(const WorldCellCoord& cell);
        // "L0_X3_Z-2" ("HLOD_L0_X3_Z-2" in the HLOD grid): the cell's file name without extension, and its name in logs.
        std::string CellName(const WorldCellCoord& cell);
        constexpr const char* CellExtension = ".noxcell";

        // The cell for content with these world bounds (world units): the smallest level whose cells hold the larger horizontal side,
        // and there the cell holding the center.
        // World bounds in double precision (large world coordinates): the cell is picked from their centre exactly.
        WorldCellCoord CellFor(const glm::dvec3& boundsMin, const glm::dvec3& boundsMax, float cellSizeWorld, int32_t grid = WorldGridIndex);
        // A cell's square on the ground (x, z), world units.
        void CellRect(const WorldCellCoord& cell, float cellSizeWorld, glm::vec2& outMin, glm::vec2& outMax);
        // The horizontal distance from a point to a box (0 inside).
        // From a world position (double precision: large world coordinates) to a cell's bounds.
        float HorizontalDistance(const glm::dvec3& point, const glm::vec3& boundsMin, const glm::vec3& boundsMax);
    }

    // A scene's world partition: its settings and cell index (saved in the scene file), and what the editor and a Play session
    // keep about the cells (never saved).
    struct WorldPartition
    {
        WorldPartitionSettings Settings;
        // The cells' folder, relative to the project's asset directory ("Scenes/World_Cells"); empty until first saved.
        std::filesystem::path CellDirectory;
        std::map<WorldCellCoord, WorldCellInfo> Cells;

        // The cell each spatially loaded root entity was loaded from or last saved to: where it goes when its bounds are not known
        // (its mesh not loaded) and what is unloaded with a cell in the editor (the components of a restored undo step are new).
        std::unordered_map<UUID, WorldCellCoord> Origins;
        // A Play / Simulate copy of an edited world: the cells the editor had loaded (their entities are in the copy, and stay in the
        // world: Scene::m_WorldSnapshots), and the cells entities were moved into that it had not loaded, as text streamed from memory
        // instead of their files (unsaved changes included). Empty text: nothing to load (a cell emptied in the editor).
        std::set<WorldCellCoord> EditorLoaded;
        std::map<WorldCellCoord, std::string> Overrides;

        std::filesystem::path CellFile(const WorldCellCoord& cell) const;
    };
}
