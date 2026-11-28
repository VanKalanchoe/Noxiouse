#pragma once
// One chunk of the voxel world (docs/Voxel_Plan_2026-11.md §5, VX1): 64^3 voxels as 512 bricks of 8^3. A brick is empty (all air),
// uniform (one material) or full (512 palette indices). The chunk's palette maps those indices to material ids of the project's voxel
// material table -- index 0 is air -- so a brick costs one byte per voxel; a chunk that needs more than 256 materials first drops the
// palette entries no voxel uses any more (Compact), then widens its indices to two bytes for itself only (Minecraft's PalettedContainer
// does the same). Not thread-safe: one writer, or readers only.

#include "VoxelTypes.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace Nox::Voxel
{
    enum class BrickKind : uint8_t
    {
        Empty = 0,   // every voxel air
        Uniform = 1, // every voxel one material (not air)
        Full = 2     // a palette index per voxel
    };

    class VoxelChunk
    {
    public:
        VoxelChunk();

        // Chunk-local coordinates, 0 .. 63.
        MaterialId Get(int32_t x, int32_t y, int32_t z) const;
        void Set(int32_t x, int32_t y, int32_t z, MaterialId material);
        // Every voxel one material (Air empties the chunk).
        void Fill(MaterialId material);
        // A brick's voxels all one material.
        void FillBrick(int32_t brick, MaterialId material);
        // The whole chunk from a dense array (VoxelInChunk order) -- the generator's output: bricks found empty / uniform stay so.
        void Assign(std::span<const MaterialId> voxels);
        // The whole chunk as a dense array (VoxelInChunk order).
        void Read(std::span<MaterialId> voxels) const;

        BrickKind GetBrickKind(int32_t brick) const { return m_Bricks[brick].Kind; }
        // A uniform brick's material (Air for an empty one, an unspecified value for a full one).
        MaterialId GetBrickMaterial(int32_t brick) const;
        // Which voxels of a brick are solid (not air): word z, bit y * 8 + x -- the voxel pass's DDA mask (VX2).
        std::array<uint64_t, BrickSize> BrickOccupancy(int32_t brick) const;
        bool IsEmpty() const;

        // Index -> material id; entry 0 is Air. Entries may be unused until Compact.
        const std::vector<MaterialId>& Palette() const { return m_Palette; }
        // Bytes per palette index in full bricks: 1, or 2 once the palette outgrew 256 entries.
        uint32_t IndexBytes() const { return m_IndexBytes; }
        // Drops palette entries no voxel uses, returns to 1-byte indices when 256 entries are enough again, and turns full bricks
        // whose voxels are all one material into uniform / empty ones.
        void Compact();
        // Counts every change (Set, Fill, Assign, ...); saved with the chunk.
        uint32_t Revision() const { return m_Revision; }
        // Heap bytes held (bricks, palette, lookup).
        size_t MemoryBytes() const;

        // Content hashes over the voxels' material ids (not palette indices: two chunks with the same voxels hash the same whatever
        // their palettes' order) -- what multiplayer compares after an edit (§1 Edits) and after generation (§3a).
        uint64_t BrickHash(int32_t brick) const;
        uint64_t Hash() const;

        // Saved form: the bricks' kinds, the palette, uniform bricks' indices and full bricks' indices bit-packed to the palette's size,
        // compressed with zstd. Decode refuses anything malformed (`error` says what) and leaves `out` unspecified then.
        std::vector<uint8_t> Encode(int zstdLevel = 3) const;
        static bool Decode(std::span<const uint8_t> data, VoxelChunk& out, std::string& error);

    private:
        struct Brick
        {
            BrickKind Kind = BrickKind::Empty;
            uint16_t Value = 0; // uniform: its palette index
            uint32_t Slot = 0;  // full: its storage slot
        };

        uint16_t IndexOf(MaterialId material); // adds the material when missing (compacting / widening as needed)
        int32_t FindIndex(MaterialId material) const; // -1 when missing
        uint16_t ReadIndex(uint32_t slot, int32_t voxel) const;
        void WriteIndex(uint32_t slot, int32_t voxel, uint16_t index);
        uint32_t AllocateSlot();
        void FreeSlot(uint32_t slot);
        void Widen();
        void RebuildLookup();
        void ResetToEmpty();

        std::array<Brick, BrickCount> m_Bricks{};
        std::vector<uint8_t> m_Storage;     // full bricks: slot * BrickVoxels * m_IndexBytes bytes each
        std::vector<uint32_t> m_FreeSlots;
        std::vector<MaterialId> m_Palette;  // index -> material id, [0] = Air
        std::vector<uint32_t> m_Lookup;     // open addressing: material id -> palette index + 1 (0: empty)
        uint32_t m_IndexBytes = 1;
        uint32_t m_Revision = 0;
    };
}
