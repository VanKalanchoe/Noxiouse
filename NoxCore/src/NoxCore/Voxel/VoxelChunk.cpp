#include "VoxelChunk.h"

#include <xxhash.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>

namespace Nox::Voxel
{
    namespace
    {
        constexpr uint32_t RawMagic = 0x4358564E;  // "NVXC": the chunk's fields
        constexpr uint32_t FileMagic = 0x5A58564E; // "NVXZ": raw size + the zstd frame of the fields
        constexpr uint16_t FormatVersion = 1;
        constexpr size_t MaxPaletteSize = 65536;
        // The largest raw form: header, a full palette, kinds, 512 two-byte-wide bricks.
        constexpr size_t MaxRawSize = 32 + MaxPaletteSize * 2 + BrickCount / 4 + 1 + BrickCount * (BrickVoxels * 16 / 8);

        uint32_t BitsFor(size_t paletteSize)
        {
            uint32_t bits = 0;
            while ((size_t(1) << bits) < paletteSize)
                ++bits;
            return bits; // 1 entry: 0 bits (every index 0)
        }

        uint32_t HashMaterial(MaterialId material, size_t tableSize) { return (material * 2654435761u) & static_cast<uint32_t>(tableSize - 1); }

        struct Writer
        {
            std::vector<uint8_t>& Out;
            void U8(uint8_t v) { Out.push_back(v); }
            void U16(uint16_t v) { Out.push_back(uint8_t(v)); Out.push_back(uint8_t(v >> 8)); }
            void U32(uint32_t v) { for (int i = 0; i < 4; ++i) Out.push_back(uint8_t(v >> (8 * i))); }
        };

        struct Reader
        {
            std::span<const uint8_t> In;
            size_t At = 0;
            bool Has(size_t bytes) const { return In.size() - At >= bytes; }
            uint8_t U8() { return In[At++]; }
            uint16_t U16() { const uint16_t v = uint16_t(In[At] | (In[At + 1] << 8)); At += 2; return v; }
            uint32_t U32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(In[At + i]) << (8 * i); At += 4; return v; }
        };
    }

    VoxelChunk::VoxelChunk() { ResetToEmpty(); }

    void VoxelChunk::ResetToEmpty()
    {
        m_Bricks.fill(Brick{});
        m_Storage.clear();
        m_FreeSlots.clear();
        m_Palette.assign(1, Air);
        m_IndexBytes = 1;
        RebuildLookup();
    }

    void VoxelChunk::RebuildLookup()
    {
        size_t size = 16;
        while (size < m_Palette.size() * 2)
            size *= 2;
        m_Lookup.assign(size, 0);
        for (size_t index = 0; index < m_Palette.size(); ++index)
        {
            uint32_t at = HashMaterial(m_Palette[index], size);
            while (m_Lookup[at] != 0)
                at = (at + 1) & static_cast<uint32_t>(size - 1);
            m_Lookup[at] = static_cast<uint32_t>(index) + 1;
        }
    }

    int32_t VoxelChunk::FindIndex(MaterialId material) const
    {
        const size_t size = m_Lookup.size();
        for (uint32_t at = HashMaterial(material, size);; at = (at + 1) & static_cast<uint32_t>(size - 1))
        {
            const uint32_t entry = m_Lookup[at];
            if (entry == 0)
                return -1;
            if (m_Palette[entry - 1] == material)
                return static_cast<int32_t>(entry - 1);
        }
    }

    uint16_t VoxelChunk::IndexOf(MaterialId material)
    {
        if (const int32_t found = FindIndex(material); found >= 0)
            return static_cast<uint16_t>(found);
        // A new entry: one-byte indices hold 256. First drop what no voxel uses, then widen this chunk's indices.
        if (m_IndexBytes == 1 && m_Palette.size() == 256)
        {
            Compact();
            if (m_Palette.size() == 256)
                Widen();
        }
        // (two-byte indices hold every material id: the palette cannot be full without this material in it)
        m_Palette.push_back(material);
        const uint16_t index = static_cast<uint16_t>(m_Palette.size() - 1);
        if (m_Palette.size() * 2 > m_Lookup.size())
            RebuildLookup();
        else
        {
            uint32_t at = HashMaterial(material, m_Lookup.size());
            while (m_Lookup[at] != 0)
                at = (at + 1) & static_cast<uint32_t>(m_Lookup.size() - 1);
            m_Lookup[at] = static_cast<uint32_t>(index) + 1;
        }
        return index;
    }

    uint16_t VoxelChunk::ReadIndex(uint32_t slot, int32_t voxel) const
    {
        const size_t at = (static_cast<size_t>(slot) * BrickVoxels + voxel) * m_IndexBytes;
        return m_IndexBytes == 1 ? m_Storage[at] : static_cast<uint16_t>(m_Storage[at] | (m_Storage[at + 1] << 8));
    }

    void VoxelChunk::WriteIndex(uint32_t slot, int32_t voxel, uint16_t index)
    {
        const size_t at = (static_cast<size_t>(slot) * BrickVoxels + voxel) * m_IndexBytes;
        m_Storage[at] = static_cast<uint8_t>(index);
        if (m_IndexBytes == 2)
            m_Storage[at + 1] = static_cast<uint8_t>(index >> 8);
    }

    uint32_t VoxelChunk::AllocateSlot()
    {
        if (!m_FreeSlots.empty())
        {
            const uint32_t slot = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            return slot;
        }
        const uint32_t slot = static_cast<uint32_t>(m_Storage.size() / (BrickVoxels * m_IndexBytes));
        m_Storage.resize(m_Storage.size() + BrickVoxels * m_IndexBytes);
        return slot;
    }

    void VoxelChunk::FreeSlot(uint32_t slot) { m_FreeSlots.push_back(slot); }

    void VoxelChunk::Widen()
    {
        std::vector<uint8_t> wide(m_Storage.size() * 2);
        for (size_t i = 0; i < m_Storage.size(); ++i)
            wide[i * 2] = m_Storage[i]; // (high bytes 0)
        m_Storage = std::move(wide);
        m_IndexBytes = 2;
    }

    MaterialId VoxelChunk::Get(int32_t x, int32_t y, int32_t z) const
    {
        const Brick& brick = m_Bricks[BrickInChunk(x / BrickSize, y / BrickSize, z / BrickSize)];
        switch (brick.Kind)
        {
        case BrickKind::Empty: return Air;
        case BrickKind::Uniform: return m_Palette[brick.Value];
        case BrickKind::Full: break;
        }
        return m_Palette[ReadIndex(brick.Slot, VoxelInBrick(x % BrickSize, y % BrickSize, z % BrickSize))];
    }

    void VoxelChunk::Set(int32_t x, int32_t y, int32_t z, MaterialId material)
    {
        const uint16_t index = IndexOf(material); // (first: it may compact the bricks)
        Brick& brick = m_Bricks[BrickInChunk(x / BrickSize, y / BrickSize, z / BrickSize)];
        const int32_t voxel = VoxelInBrick(x % BrickSize, y % BrickSize, z % BrickSize);
        if (brick.Kind != BrickKind::Full)
        {
            const uint16_t current = brick.Kind == BrickKind::Uniform ? brick.Value : 0;
            if (current == index)
                return;
            const uint32_t slot = AllocateSlot();
            for (int32_t v = 0; v < BrickVoxels; ++v)
                WriteIndex(slot, v, current);
            brick = { BrickKind::Full, 0, slot };
        }
        else if (ReadIndex(brick.Slot, voxel) == index)
            return;
        WriteIndex(brick.Slot, voxel, index);
        ++m_Revision;
    }

    void VoxelChunk::Fill(MaterialId material)
    {
        ResetToEmpty();
        if (material != Air)
        {
            const uint16_t index = IndexOf(material);
            for (Brick& brick : m_Bricks)
                brick = { BrickKind::Uniform, index, 0 };
        }
        ++m_Revision;
    }

    void VoxelChunk::FillBrick(int32_t brickIndex, MaterialId material)
    {
        const uint16_t index = IndexOf(material);
        Brick& brick = m_Bricks[brickIndex];
        if (brick.Kind == BrickKind::Full)
            FreeSlot(brick.Slot);
        brick = index == 0 ? Brick{} : Brick{ BrickKind::Uniform, index, 0 };
        ++m_Revision;
    }

    void VoxelChunk::Assign(std::span<const MaterialId> voxels)
    {
        // The palette first, whole: every material present (ascending ids, so the same voxels give the same palette), the index width
        // it needs -- then the bricks with lookups only (adding entries brick by brick could compact half-written storage).
        ResetToEmpty();
        std::vector<uint64_t> present(65536 / 64, 0);
        for (MaterialId material : voxels)
            present[material / 64] |= 1ull << (material % 64);
        for (uint32_t material = 1; material < 65536; ++material)
            if (present[material / 64] & (1ull << (material % 64)))
                m_Palette.push_back(static_cast<MaterialId>(material));
        m_IndexBytes = m_Palette.size() <= 256 ? 1 : 2;
        RebuildLookup();

        MaterialId lastMaterial = Air;
        uint16_t lastIndex = 0;
        std::array<MaterialId, BrickVoxels> brickVoxels;
        for (int32_t bz = 0; bz < BricksPerAxis; ++bz)
            for (int32_t by = 0; by < BricksPerAxis; ++by)
                for (int32_t bx = 0; bx < BricksPerAxis; ++bx)
                {
                    bool uniform = true;
                    for (int32_t z = 0; z < BrickSize; ++z)
                        for (int32_t y = 0; y < BrickSize; ++y)
                        {
                            const MaterialId* row = &voxels[VoxelInChunk(bx * BrickSize, by * BrickSize + y, bz * BrickSize + z)];
                            for (int32_t x = 0; x < BrickSize; ++x)
                            {
                                brickVoxels[VoxelInBrick(x, y, z)] = row[x];
                                uniform &= row[x] == brickVoxels[0];
                            }
                        }
                    Brick& brick = m_Bricks[BrickInChunk(bx, by, bz)];
                    if (uniform)
                    {
                        if (brickVoxels[0] != Air)
                            brick = { BrickKind::Uniform, static_cast<uint16_t>(FindIndex(brickVoxels[0])), 0 };
                        continue;
                    }
                    const uint32_t slot = AllocateSlot();
                    for (int32_t v = 0; v < BrickVoxels; ++v)
                    {
                        if (brickVoxels[v] != lastMaterial)
                        {
                            lastMaterial = brickVoxels[v];
                            lastIndex = static_cast<uint16_t>(FindIndex(lastMaterial));
                        }
                        WriteIndex(slot, v, lastIndex);
                    }
                    brick = { BrickKind::Full, 0, slot };
                }
        ++m_Revision;
    }

    void VoxelChunk::Read(std::span<MaterialId> voxels) const
    {
        for (int32_t bz = 0; bz < BricksPerAxis; ++bz)
            for (int32_t by = 0; by < BricksPerAxis; ++by)
                for (int32_t bx = 0; bx < BricksPerAxis; ++bx)
                {
                    const Brick& brick = m_Bricks[BrickInChunk(bx, by, bz)];
                    for (int32_t z = 0; z < BrickSize; ++z)
                        for (int32_t y = 0; y < BrickSize; ++y)
                        {
                            MaterialId* row = &voxels[VoxelInChunk(bx * BrickSize, by * BrickSize + y, bz * BrickSize + z)];
                            for (int32_t x = 0; x < BrickSize; ++x)
                                row[x] = brick.Kind == BrickKind::Empty     ? Air
                                         : brick.Kind == BrickKind::Uniform ? m_Palette[brick.Value]
                                                                            : m_Palette[ReadIndex(brick.Slot, VoxelInBrick(x, y, z))];
                        }
                }
    }

    MaterialId VoxelChunk::GetBrickMaterial(int32_t brick) const
    {
        return m_Bricks[brick].Kind == BrickKind::Uniform ? m_Palette[m_Bricks[brick].Value] : Air;
    }

    std::array<uint64_t, BrickSize> VoxelChunk::BrickOccupancy(int32_t brickIndex) const
    {
        std::array<uint64_t, BrickSize> words{};
        const Brick& brick = m_Bricks[brickIndex];
        if (brick.Kind == BrickKind::Uniform)
            words.fill(~0ull);
        else if (brick.Kind == BrickKind::Full)
            for (int32_t v = 0; v < BrickVoxels; ++v)
                if (ReadIndex(brick.Slot, v) != 0)
                    words[v / 64] |= 1ull << (v % 64); // v = x + 8 y + 64 z: word z, bit y * 8 + x
        return words;
    }

    bool VoxelChunk::IsEmpty() const
    {
        for (const Brick& brick : m_Bricks)
        {
            if (brick.Kind == BrickKind::Uniform)
                return false;
            if (brick.Kind == BrickKind::Full)
                for (int32_t v = 0; v < BrickVoxels; ++v)
                    if (ReadIndex(brick.Slot, v) != 0)
                        return false;
        }
        return true;
    }

    void VoxelChunk::Compact()
    {
        std::vector<uint8_t> used(m_Palette.size(), 0);
        used[0] = 1; // air stays index 0
        for (Brick& brick : m_Bricks)
        {
            if (brick.Kind == BrickKind::Uniform)
                used[brick.Value] = 1;
            else if (brick.Kind == BrickKind::Full)
            {
                const uint16_t first = ReadIndex(brick.Slot, 0);
                bool uniform = true;
                for (int32_t v = 0; v < BrickVoxels; ++v)
                {
                    const uint16_t index = ReadIndex(brick.Slot, v);
                    used[index] = 1;
                    uniform &= index == first;
                }
                if (uniform)
                {
                    FreeSlot(brick.Slot);
                    brick = first == 0 ? Brick{} : Brick{ BrickKind::Uniform, first, 0 };
                }
            }
        }
        // (a full brick that became uniform marked its index used: harmless, at most one extra entry until the next Compact)
        std::vector<uint16_t> remap(m_Palette.size(), 0);
        std::vector<MaterialId> palette;
        palette.reserve(m_Palette.size());
        for (size_t index = 0; index < m_Palette.size(); ++index)
            if (used[index])
            {
                remap[index] = static_cast<uint16_t>(palette.size());
                palette.push_back(m_Palette[index]);
            }

        const uint32_t indexBytes = palette.size() <= 256 ? 1 : 2;
        std::vector<uint8_t> storage;
        uint32_t slots = 0;
        for (Brick& brick : m_Bricks)
        {
            if (brick.Kind == BrickKind::Uniform)
                brick.Value = remap[brick.Value];
            else if (brick.Kind == BrickKind::Full)
            {
                storage.resize(storage.size() + BrickVoxels * indexBytes);
                uint8_t* out = storage.data() + static_cast<size_t>(slots) * BrickVoxels * indexBytes;
                for (int32_t v = 0; v < BrickVoxels; ++v)
                {
                    const uint16_t index = remap[ReadIndex(brick.Slot, v)];
                    out[v * indexBytes] = static_cast<uint8_t>(index);
                    if (indexBytes == 2)
                        out[v * indexBytes + 1] = static_cast<uint8_t>(index >> 8);
                }
                brick.Slot = slots++;
            }
        }
        m_Storage = std::move(storage);
        m_FreeSlots.clear();
        m_Palette = std::move(palette);
        m_IndexBytes = indexBytes;
        RebuildLookup();
    }

    size_t VoxelChunk::MemoryBytes() const
    {
        return sizeof(*this) + m_Storage.capacity() + m_FreeSlots.capacity() * sizeof(uint32_t) + m_Palette.capacity() * sizeof(MaterialId) +
               m_Lookup.capacity() * sizeof(uint32_t);
    }

    uint64_t VoxelChunk::BrickHash(int32_t brickIndex) const
    {
        std::array<uint16_t, BrickVoxels> ids;
        const Brick& brick = m_Bricks[brickIndex];
        if (brick.Kind == BrickKind::Full)
            for (int32_t v = 0; v < BrickVoxels; ++v)
                ids[v] = m_Palette[ReadIndex(brick.Slot, v)];
        else
            ids.fill(brick.Kind == BrickKind::Uniform ? m_Palette[brick.Value] : Air);
        return XXH3_64bits(ids.data(), sizeof(ids)); // (little-endian material ids)
    }

    uint64_t VoxelChunk::Hash() const
    {
        std::array<uint64_t, BrickCount> hashes;
        for (int32_t brick = 0; brick < BrickCount; ++brick)
            hashes[brick] = BrickHash(brick);
        return XXH3_64bits(hashes.data(), sizeof(hashes));
    }

    std::vector<uint8_t> VoxelChunk::Encode(int zstdLevel) const
    {
        // The bricks as they are saved: a full brick whose voxels are all one index as uniform / empty.
        std::array<BrickKind, BrickCount> kinds;
        std::array<uint16_t, BrickCount> uniformIndex{};
        for (int32_t b = 0; b < BrickCount; ++b)
        {
            const Brick& brick = m_Bricks[b];
            kinds[b] = brick.Kind;
            uniformIndex[b] = brick.Value;
            if (brick.Kind == BrickKind::Full)
            {
                const uint16_t first = ReadIndex(brick.Slot, 0);
                bool uniform = true;
                for (int32_t v = 1; v < BrickVoxels && uniform; ++v)
                    uniform = ReadIndex(brick.Slot, v) == first;
                if (uniform)
                {
                    kinds[b] = first == 0 ? BrickKind::Empty : BrickKind::Uniform;
                    uniformIndex[b] = first;
                }
            }
        }

        std::vector<uint8_t> raw;
        Writer w{ raw };
        w.U32(RawMagic);
        w.U16(FormatVersion);
        w.U16(0);
        w.U32(m_Revision);
        w.U32(static_cast<uint32_t>(m_Palette.size()));
        for (MaterialId material : m_Palette)
            w.U16(material);
        for (int32_t b = 0; b < BrickCount; b += 4)
            w.U8(static_cast<uint8_t>(uint8_t(kinds[b]) | (uint8_t(kinds[b + 1]) << 2) | (uint8_t(kinds[b + 2]) << 4) | (uint8_t(kinds[b + 3]) << 6)));
        const uint32_t bits = BitsFor(m_Palette.size());
        w.U8(static_cast<uint8_t>(bits));
        for (int32_t b = 0; b < BrickCount; ++b)
        {
            if (kinds[b] == BrickKind::Uniform)
                w.U16(uniformIndex[b]);
            else if (kinds[b] == BrickKind::Full)
            {
                // 512 indices x `bits` bits, least significant first: 64 x bits bytes.
                const size_t start = raw.size();
                raw.resize(start + 64 * static_cast<size_t>(bits), 0);
                uint8_t* out = raw.data() + start;
                for (int32_t v = 0; v < BrickVoxels; ++v)
                {
                    const uint32_t index = ReadIndex(m_Bricks[b].Slot, v);
                    const size_t bit = static_cast<size_t>(v) * bits;
                    for (uint32_t i = 0; i < bits; ++i)
                        if (index & (1u << i))
                            out[(bit + i) / 8] |= static_cast<uint8_t>(1u << ((bit + i) % 8));
                }
            }
        }

        std::vector<uint8_t> file(8 + ZSTD_compressBound(raw.size()));
        Writer header{ file };
        file.clear();
        header.U32(FileMagic);
        header.U32(static_cast<uint32_t>(raw.size()));
        file.resize(8 + ZSTD_compressBound(raw.size()));
        const size_t compressed = ZSTD_compress(file.data() + 8, file.size() - 8, raw.data(), raw.size(), zstdLevel);
        file.resize(ZSTD_isError(compressed) ? 8 : 8 + compressed);
        return file;
    }

    bool VoxelChunk::Decode(std::span<const uint8_t> data, VoxelChunk& out, std::string& error)
    {
        Reader file{ data };
        if (!file.Has(8) || file.U32() != FileMagic)
        {
            error = "not a voxel chunk (no NVXZ header)";
            return false;
        }
        const uint32_t rawSize = file.U32();
        if (rawSize > MaxRawSize)
        {
            error = "voxel chunk: raw size " + std::to_string(rawSize) + " is larger than any chunk";
            return false;
        }
        const unsigned long long frameSize = ZSTD_getFrameContentSize(data.data() + 8, data.size() - 8);
        if (frameSize != rawSize)
        {
            error = "voxel chunk: the compressed frame is damaged or its size does not match";
            return false;
        }
        std::vector<uint8_t> raw(rawSize);
        const size_t decompressed = ZSTD_decompress(raw.data(), raw.size(), data.data() + 8, data.size() - 8);
        if (ZSTD_isError(decompressed) || decompressed != rawSize)
        {
            error = "voxel chunk: zstd could not decompress it";
            return false;
        }

        Reader r{ raw };
        if (!r.Has(16) || r.U32() != RawMagic)
        {
            error = "voxel chunk: fields without their NVXC header";
            return false;
        }
        const uint16_t version = r.U16();
        r.U16();
        if (version != FormatVersion)
        {
            error = "voxel chunk: format version " + std::to_string(version) + " is not supported";
            return false;
        }
        const uint32_t revision = r.U32();
        const uint32_t paletteSize = r.U32();
        if (paletteSize == 0 || paletteSize > MaxPaletteSize || !r.Has(static_cast<size_t>(paletteSize) * 2 + BrickCount / 4 + 1))
        {
            error = "voxel chunk: palette size " + std::to_string(paletteSize) + " is impossible";
            return false;
        }
        std::vector<MaterialId> palette(paletteSize);
        for (MaterialId& material : palette)
            material = r.U16();
        if (palette[0] != Air)
        {
            error = "voxel chunk: palette entry 0 is not air";
            return false;
        }
        {
            std::vector<MaterialId> sorted = palette;
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            {
                error = "voxel chunk: a material appears twice in the palette";
                return false;
            }
        }
        std::array<BrickKind, BrickCount> kinds;
        for (int32_t b = 0; b < BrickCount; b += 4)
        {
            const uint8_t packed = r.U8();
            for (int32_t i = 0; i < 4; ++i)
            {
                const uint8_t kind = (packed >> (2 * i)) & 3;
                if (kind > 2)
                {
                    error = "voxel chunk: unknown brick kind";
                    return false;
                }
                kinds[b + i] = static_cast<BrickKind>(kind);
            }
        }
        const uint32_t bits = r.U8();
        if (bits != BitsFor(paletteSize))
        {
            error = "voxel chunk: index width " + std::to_string(bits) + " does not match a palette of " + std::to_string(paletteSize);
            return false;
        }

        out.ResetToEmpty();
        out.m_Palette = std::move(palette);
        out.m_IndexBytes = paletteSize <= 256 ? 1 : 2;
        out.RebuildLookup();
        for (int32_t b = 0; b < BrickCount; ++b)
        {
            if (kinds[b] == BrickKind::Uniform)
            {
                if (!r.Has(2))
                {
                    error = "voxel chunk: ends inside a brick";
                    return false;
                }
                const uint16_t index = r.U16();
                if (index == 0 || index >= paletteSize)
                {
                    error = "voxel chunk: a uniform brick's index is outside the palette";
                    return false;
                }
                out.m_Bricks[b] = { BrickKind::Uniform, index, 0 };
            }
            else if (kinds[b] == BrickKind::Full)
            {
                const size_t bytes = 64 * static_cast<size_t>(bits);
                if (!r.Has(bytes))
                {
                    error = "voxel chunk: ends inside a brick";
                    return false;
                }
                const uint8_t* in = raw.data() + r.At;
                r.At += bytes;
                const uint32_t slot = out.AllocateSlot();
                for (int32_t v = 0; v < BrickVoxels; ++v)
                {
                    uint32_t index = 0;
                    const size_t bit = static_cast<size_t>(v) * bits;
                    for (uint32_t i = 0; i < bits; ++i)
                        index |= static_cast<uint32_t>((in[(bit + i) / 8] >> ((bit + i) % 8)) & 1) << i;
                    if (index >= paletteSize)
                    {
                        error = "voxel chunk: a voxel's index is outside the palette";
                        return false;
                    }
                    out.WriteIndex(slot, v, static_cast<uint16_t>(index));
                }
                out.m_Bricks[b] = { BrickKind::Full, 0, slot };
            }
        }
        if (r.At != raw.size())
        {
            error = "voxel chunk: " + std::to_string(raw.size() - r.At) + " bytes after the last brick";
            return false;
        }
        out.m_Revision = revision;
        return true;
    }
}
