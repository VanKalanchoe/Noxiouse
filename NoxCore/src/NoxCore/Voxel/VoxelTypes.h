#pragma once
// The voxel world's units (docs/Voxel_Plan_2026-11.md §1, §5): chunks of 64^3 voxels, each 8 x 8 x 8 bricks of 8^3 voxels; a voxel is a
// material id of the project's voxel material table (0: air). Engine axes (Y up), integer voxel coordinates; the voxel size (a project
// setting, 5 cm - 1 m, 10 cm by default) only turns them into metres.

#include <glm/glm.hpp>

#include <cstdint>

namespace Nox::Voxel
{
    using MaterialId = uint16_t;
    inline constexpr MaterialId Air = 0;

    inline constexpr int32_t ChunkSize = 64;
    inline constexpr int32_t BrickSize = 8;
    inline constexpr int32_t BricksPerAxis = ChunkSize / BrickSize; // 8
    inline constexpr int32_t BrickCount = BricksPerAxis * BricksPerAxis * BricksPerAxis; // 512
    inline constexpr int32_t BrickVoxels = BrickSize * BrickSize * BrickSize;             // 512
    inline constexpr int32_t ChunkVoxels = ChunkSize * ChunkSize * ChunkSize;             // 262144
    // The binary greedy mesher's unit (VX4): 32 voxels + a one-voxel border on each side fit a 64-bit column word; a chunk is 2 x 2 x 2.
    inline constexpr int32_t MeshPartSize = 32;

    // Voxel / brick / chunk-local orders: x fastest, then y, then z.
    inline constexpr int32_t VoxelInBrick(int32_t x, int32_t y, int32_t z) { return x + y * BrickSize + z * BrickSize * BrickSize; }
    inline constexpr int32_t BrickInChunk(int32_t bx, int32_t by, int32_t bz) { return bx + by * BricksPerAxis + bz * BricksPerAxis * BricksPerAxis; }
    inline constexpr int32_t VoxelInChunk(int32_t x, int32_t y, int32_t z) { return x + y * ChunkSize + z * ChunkSize * ChunkSize; }

    // Floor division / modulo for negative coordinates (the chunk of voxel -1 is -1, its local coordinate 63).
    inline constexpr int32_t FloorDiv(int32_t value, int32_t divisor)
    {
        return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
    }
    inline glm::ivec3 ChunkOf(const glm::ivec3& voxel)
    {
        return { FloorDiv(voxel.x, ChunkSize), FloorDiv(voxel.y, ChunkSize), FloorDiv(voxel.z, ChunkSize) };
    }
    inline glm::ivec3 LocalOf(const glm::ivec3& voxel) { return voxel & glm::ivec3(ChunkSize - 1); }

    // A chunk position packed into 64 bits (21 bits per axis, two's complement): the chunk index's key. +-1,048,576 chunks per axis --
    // +-6,710 km at 10 cm voxels.
    inline constexpr int32_t ChunkCoordLimit = 1 << 20;
    inline constexpr uint64_t ChunkKey(const glm::ivec3& chunk)
    {
        constexpr uint64_t mask = (1ull << 21) - 1;
        return (static_cast<uint64_t>(static_cast<uint32_t>(chunk.x)) & mask) | ((static_cast<uint64_t>(static_cast<uint32_t>(chunk.y)) & mask) << 21) |
               ((static_cast<uint64_t>(static_cast<uint32_t>(chunk.z)) & mask) << 42);
    }
    inline constexpr glm::ivec3 ChunkFromKey(uint64_t key)
    {
        auto axis = [](uint64_t bits) { return static_cast<int32_t>(static_cast<uint32_t>(bits << 11)) >> 11; }; // sign-extend 21 bits
        return { axis(key & 0x1fffff), axis((key >> 21) & 0x1fffff), axis((key >> 42) & 0x1fffff) };
    }
}
