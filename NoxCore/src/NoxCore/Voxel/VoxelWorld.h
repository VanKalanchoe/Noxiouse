#pragma once
// The voxel world's chunks in memory (docs/Voxel_Plan_2026-11.md §5, VX1): an index from chunk position to chunk (ankerl
// unordered_dense, the position packed into 64 bits), the generator that fills a chunk the first time it is loaded, and the columns'
// surface heights (generated once per 64 x 64 column, shared by its chunks). Endless horizontally; vertically between the world's
// minimum and maximum height (-512 m .. +512 m by default) -- chunks outside it are never loaded. Not thread-safe: one thread uses the
// world (LoadRegion generates on worker threads internally). Saving, streaming by distance (VX6) and the GPU side (VX2) build on it.

#include "VoxelGenerator.h"

#include <ankerl/unordered_dense.h>

#include <functional>
#include <memory>

namespace Nox::Voxel
{
    struct VoxelWorldSettings
    {
        float VoxelSize = 0.1f; // metres (a project setting, 5 cm - 1 m)
        int32_t Seed = 1337;
        float MinHeight = -512.0f; // metres
        float MaxHeight = 512.0f;
    };

    class VoxelWorld
    {
    public:
        // `generator` may be null: chunks then load empty. Its voxel size must be the world's.
        explicit VoxelWorld(const VoxelWorldSettings& settings, std::shared_ptr<const VoxelGenerator> generator = nullptr);

        const VoxelWorldSettings& Settings() const { return m_Settings; }
        const VoxelGenerator* Generator() const { return m_Generator.get(); }
        // Chunk Y range inside the vertical limits.
        int32_t MinChunkY() const { return m_MinChunkY; }
        int32_t MaxChunkY() const { return m_MaxChunkY; }

        // Null when not loaded.
        VoxelChunk* Find(const glm::ivec3& chunk);
        const VoxelChunk* Find(const glm::ivec3& chunk) const;
        // The chunk, loaded first when needed (generated, or empty without a generator); null outside the vertical limits.
        // secretLayer: generate the secret layer too (the server, single player; §3b).
        VoxelChunk* Load(const glm::ivec3& chunk, bool secretLayer = true);
        // Generates every chunk of the box [minChunk, maxChunk] (clamped to the vertical limits) not loaded yet, on `threads` worker
        // threads (0: one per hardware thread); returns how many it added.
        size_t LoadRegion(const glm::ivec3& minChunk, const glm::ivec3& maxChunk, bool secretLayer = true, uint32_t threads = 0);
        // A chunk from elsewhere (a save, the network) replaces whatever is loaded there; refused outside the vertical limits.
        bool Insert(const glm::ivec3& chunk, std::unique_ptr<VoxelChunk> data);
        bool Unload(const glm::ivec3& chunk);

        // World voxel coordinates. Get: Air where no chunk is loaded. Set: loads the chunk first; ignored outside the vertical limits.
        MaterialId GetVoxel(const glm::ivec3& voxel) const;
        void SetVoxel(const glm::ivec3& voxel, MaterialId material);

        // A column's surface heights (generated and kept on first use); null without a generator.
        const ColumnHeights* Column(int32_t chunkX, int32_t chunkZ);

        size_t ChunkCount() const { return m_Chunks.size(); }
        size_t MemoryBytes() const;
        void ForEachChunk(const std::function<void(const glm::ivec3& chunk, const VoxelChunk& data)>& visit) const;

    private:
        bool InVerticalRange(int32_t chunkY) const { return chunkY >= m_MinChunkY && chunkY <= m_MaxChunkY; }

        VoxelWorldSettings m_Settings;
        std::shared_ptr<const VoxelGenerator> m_Generator;
        int32_t m_MinChunkY = 0;
        int32_t m_MaxChunkY = 0;
        ankerl::unordered_dense::map<uint64_t, std::unique_ptr<VoxelChunk>> m_Chunks;
        ankerl::unordered_dense::map<uint64_t, std::unique_ptr<ColumnHeights>> m_Columns; // key: ChunkKey(x, 0, z)
    };
}
