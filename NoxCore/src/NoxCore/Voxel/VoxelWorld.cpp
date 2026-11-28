#include "VoxelWorld.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

namespace Nox::Voxel
{
    VoxelWorld::VoxelWorld(const VoxelWorldSettings& settings, std::shared_ptr<const VoxelGenerator> generator)
        : m_Settings(settings), m_Generator(std::move(generator))
    {
        // In whole voxels first (512 m / 6.4 m is 79.99999... in floating point): the chunks that hold any voxel of [min, max).
        const int32_t minVoxel = static_cast<int32_t>(std::lround(static_cast<double>(settings.MinHeight) / settings.VoxelSize));
        const int32_t maxVoxel = static_cast<int32_t>(std::lround(static_cast<double>(settings.MaxHeight) / settings.VoxelSize));
        m_MinChunkY = FloorDiv(minVoxel, ChunkSize);
        m_MaxChunkY = FloorDiv(maxVoxel - 1, ChunkSize);
    }

    VoxelChunk* VoxelWorld::Find(const glm::ivec3& chunk)
    {
        const auto found = m_Chunks.find(ChunkKey(chunk));
        return found != m_Chunks.end() ? found->second.get() : nullptr;
    }

    const VoxelChunk* VoxelWorld::Find(const glm::ivec3& chunk) const
    {
        const auto found = m_Chunks.find(ChunkKey(chunk));
        return found != m_Chunks.end() ? found->second.get() : nullptr;
    }

    const ColumnHeights* VoxelWorld::Column(int32_t chunkX, int32_t chunkZ)
    {
        if (!m_Generator)
            return nullptr;
        std::unique_ptr<ColumnHeights>& column = m_Columns[ChunkKey(glm::ivec3(chunkX, 0, chunkZ))];
        if (!column)
            column = std::make_unique<ColumnHeights>(m_Generator->Column(chunkX, chunkZ));
        return column.get();
    }

    VoxelChunk* VoxelWorld::Load(const glm::ivec3& chunk, bool secretLayer)
    {
        if (!InVerticalRange(chunk.y))
            return nullptr;
        std::unique_ptr<VoxelChunk>& slot = m_Chunks[ChunkKey(chunk)];
        if (!slot)
        {
            slot = std::make_unique<VoxelChunk>();
            if (m_Generator)
                m_Generator->Generate(chunk, *Column(chunk.x, chunk.z), secretLayer, *slot);
        }
        return slot.get();
    }

    size_t VoxelWorld::LoadRegion(const glm::ivec3& minChunk, const glm::ivec3& maxChunk, bool secretLayer, uint32_t threads)
    {
        const int32_t yLow = std::max(minChunk.y, m_MinChunkY);
        const int32_t yHigh = std::min(maxChunk.y, m_MaxChunkY);
        std::vector<glm::ivec3> missing;
        for (int32_t z = minChunk.z; z <= maxChunk.z; ++z)
            for (int32_t x = minChunk.x; x <= maxChunk.x; ++x)
                for (int32_t y = yLow; y <= yHigh; ++y)
                    if (!m_Chunks.contains(ChunkKey(glm::ivec3(x, y, z))))
                        missing.emplace_back(x, y, z);
        if (missing.empty())
            return 0;
        std::vector<std::unique_ptr<VoxelChunk>> made(missing.size());
        if (!m_Generator)
        {
            for (std::unique_ptr<VoxelChunk>& chunk : made)
                chunk = std::make_unique<VoxelChunk>();
        }
        else
        {
            // The columns first (one height grid per column, shared by its chunks; the map is touched on this thread only), then the
            // chunks on the workers -- the generator is thread-safe.
            std::vector<const ColumnHeights*> columns(missing.size());
            for (size_t i = 0; i < missing.size(); ++i)
                columns[i] = Column(missing[i].x, missing[i].z);
            const uint32_t workers = std::max(1u, std::min<uint32_t>(threads ? threads : std::max(1u, std::thread::hardware_concurrency()),
                                                                    static_cast<uint32_t>(missing.size())));
            std::atomic<size_t> next{ 0 };
            auto work = [&]()
            {
                for (size_t i = next++; i < missing.size(); i = next++)
                {
                    made[i] = std::make_unique<VoxelChunk>();
                    m_Generator->Generate(missing[i], *columns[i], secretLayer, *made[i]);
                }
            };
            std::vector<std::thread> pool;
            for (uint32_t t = 1; t < workers; ++t)
                pool.emplace_back(work);
            work();
            for (std::thread& thread : pool)
                thread.join();
        }
        for (size_t i = 0; i < missing.size(); ++i)
            m_Chunks[ChunkKey(missing[i])] = std::move(made[i]);
        return missing.size();
    }

    bool VoxelWorld::Insert(const glm::ivec3& chunk, std::unique_ptr<VoxelChunk> data)
    {
        if (!InVerticalRange(chunk.y) || !data)
            return false;
        m_Chunks[ChunkKey(chunk)] = std::move(data);
        return true;
    }

    bool VoxelWorld::Unload(const glm::ivec3& chunk) { return m_Chunks.erase(ChunkKey(chunk)) > 0; }

    MaterialId VoxelWorld::GetVoxel(const glm::ivec3& voxel) const
    {
        const VoxelChunk* chunk = Find(ChunkOf(voxel));
        if (!chunk)
            return Air;
        const glm::ivec3 local = LocalOf(voxel);
        return chunk->Get(local.x, local.y, local.z);
    }

    void VoxelWorld::SetVoxel(const glm::ivec3& voxel, MaterialId material)
    {
        VoxelChunk* chunk = Load(ChunkOf(voxel));
        if (!chunk)
            return;
        const glm::ivec3 local = LocalOf(voxel);
        chunk->Set(local.x, local.y, local.z, material);
    }

    size_t VoxelWorld::MemoryBytes() const
    {
        size_t bytes = sizeof(*this);
        for (const auto& [key, chunk] : m_Chunks)
            bytes += sizeof(key) + sizeof(chunk) + chunk->MemoryBytes();
        for (const auto& [key, column] : m_Columns)
            bytes += sizeof(key) + sizeof(column) + sizeof(ColumnHeights) + column->Heights.capacity() * sizeof(int32_t);
        return bytes;
    }

    void VoxelWorld::ForEachChunk(const std::function<void(const glm::ivec3&, const VoxelChunk&)>& visit) const
    {
        for (const auto& [key, chunk] : m_Chunks)
            visit(ChunkFromKey(key), *chunk);
    }
}
