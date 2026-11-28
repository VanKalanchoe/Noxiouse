#pragma once
// The voxel world's generator (docs/Voxel_Plan_2026-11.md §5, §3a, §3b; VX1): FastNoise2 node trees -- the encoded strings its Node
// Editor makes -- turned into chunks. Two layers:
//   public: the shape (a 2D height tree, a 3D cave tree carving below the surface) and the surface materials by depth (surface,
//           subsurface, bulk); every machine generates it.
//   secret: ore and loot pockets inside the bulk material (3D trees with thresholds and height ranges); only the server generates it in
//           multiplayer (§3b), clients see a pocket once it is revealed.
// Positions are metres (voxel centres), so a tree's Feature Scale sets the size of hills and caves whatever the voxel size. FastNoise2 is
// built with strict floating point: every SIMD level gives the same noise bit for bit, so every machine generates the same chunk (§3a).
// Thread-safe: any number of threads may generate at once.

#include "VoxelChunk.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Nox::Voxel
{
    // Defaults (made with FastNoise2's metadata, checked to decode in the tests): heights -- fBm of SuperSimplex, 160 m features, 5
    // octaves; caves -- fBm of Simplex, 30 m, 2 octaves; ore pockets -- Simplex, 4 m.
    inline constexpr const char* DefaultHeightTree = "DQAF@BCQc@BgQww=";
    inline constexpr const char* DefaultCaveTree = "DQAC@BCQY@ADwQQw=";
    inline constexpr const char* DefaultOreTree = "Bg@AIBABA==";

    struct VoxelOreLayer
    {
        std::string Name;
        MaterialId Material = Air;
        std::string NoiseTree = DefaultOreTree;
        float Threshold = 0.75f;   // bulk voxels where the noise is above it become the ore
        float MinHeight = -512.0f; // metres
        float MaxHeight = 0.0f;
        bool operator==(const VoxelOreLayer&) const = default;
    };

    struct VoxelGeneratorSettings
    {
        // Raised whenever the generation's rules change: a world keeps the version it was made with (§1 Generator versions).
        uint32_t Version = 1;

        // Public layer.
        std::string HeightTree = DefaultHeightTree; // 2D, output about -1 .. 1
        float BaseHeight = 0.0f;                    // metres: the surface where the height noise is 0
        float HeightAmplitude = 24.0f;              // metres per unit of height noise
        std::string CaveTree = DefaultCaveTree;     // 3D; empty: no caves
        float CaveThreshold = 0.65f;                // carved where the noise is above it (0.65: ~10% of the underground with the
                                                    // default tree, measured over +-5 km; 0.45 carved 20%)
        float CaveTopDepth = 4.0f;                  // metres: caves stay at least this far below the surface
        MaterialId SurfaceMaterial = Air;           // the topmost solid voxel of each column
        MaterialId SubsurfaceMaterial = Air;        // below it, SubsurfaceDepth deep
        float SubsurfaceDepth = 3.0f;               // metres
        MaterialId BulkMaterial = Air;              // everything deeper

        // Secret layer.
        std::vector<VoxelOreLayer> Ores;

        // Over every field (binary, platform independent): stored with a world next to the version.
        uint64_t Hash() const;
        std::string Serialize() const;
        static bool Deserialize(const std::string& text, VoxelGeneratorSettings& out, std::string& error);
        bool operator==(const VoxelGeneratorSettings&) const = default;
    };

    class VoxelMaterialTable;
    // Adds Grass, Dirt, Stone and Iron Ore to `table` (or finds them by name) and returns settings using them: a ready terrain.
    VoxelGeneratorSettings MakeDefaultTerrain(VoxelMaterialTable& table);

    // A chunk column's 64 x 64 surface heights (index x + z * 64; the topmost solid voxel's Y) and their range.
    struct ColumnHeights
    {
        std::vector<int32_t> Heights;
        int32_t MinSurface = 0;
        int32_t MaxSurface = 0;
    };

    // The CPU instruction set FastNoise2 uses: the best this CPU has, or one forced (the determinism tests compare them). FastSIMD builds
    // SSE2, SSE4.1, AVX2 and AVX-512 on x86 (no scalar), NEON on ARM; forcing one not built makes the generator invalid.
    enum class SimdLevel : uint8_t
    {
        Best,
        Scalar,
        SSE2,
        SSE41,
        AVX2,
        AVX512
    };

    class VoxelGenerator
    {
    public:
        VoxelGenerator(const VoxelGeneratorSettings& settings, float voxelSize, int32_t seed, SimdLevel simd = SimdLevel::Best);
        ~VoxelGenerator();
        VoxelGenerator(const VoxelGenerator&) = delete;
        VoxelGenerator& operator=(const VoxelGenerator&) = delete;

        // False when a tree cannot be read or a layer's material is air (Error() says which); Generate then fills chunks with air.
        bool IsValid() const { return m_Error.empty(); }
        const std::string& Error() const { return m_Error; }
        const VoxelGeneratorSettings& Settings() const { return m_Settings; }
        float VoxelSize() const { return m_VoxelSize; }
        int32_t Seed() const { return m_Seed; }

        ColumnHeights Column(int32_t chunkX, int32_t chunkZ) const;
        // `column`: this chunk's column (Column(chunk.x, chunk.z)); secretLayer: the ore pockets too (the server, single player).
        void Generate(const glm::ivec3& chunk, const ColumnHeights& column, bool secretLayer, VoxelChunk& out) const;
        void Generate(const glm::ivec3& chunk, bool secretLayer, VoxelChunk& out) const;

    private:
        struct Nodes;
        VoxelGeneratorSettings m_Settings;
        float m_VoxelSize = 0.1f;
        int32_t m_Seed = 0;
        std::unique_ptr<Nodes> m_Nodes;
        std::string m_Error;
    };
}
