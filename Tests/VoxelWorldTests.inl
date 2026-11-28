// Voxel world (docs/Voxel_Plan_2026-11.md §5, VX1): chunks, palettes, encoding, the material table, the generator, the world map and
// stamping a .vox. Included by ToolsTest.cpp (CHECK as there).
#include "NoxCore/Voxel/VoxelChunk.h"
#include "NoxCore/Voxel/VoxelGenerator.h"
#include "NoxCore/Voxel/VoxelMaterialTable.h"
#include "NoxCore/Voxel/VoxelStamp.h"
#include "NoxCore/Voxel/VoxelWorld.h"

#include <chrono>
#include <map>
#include <set>
#include <tuple>

#include <random>

namespace VoxelWorldTest
{
    using namespace Nox::Voxel;

    // A dense reference: the voxels as a plain array, compared voxel by voxel against a chunk.
    bool SameAsDense(const VoxelChunk& chunk, const std::vector<MaterialId>& dense)
    {
        for (int32_t z = 0; z < ChunkSize; ++z)
            for (int32_t y = 0; y < ChunkSize; ++y)
                for (int32_t x = 0; x < ChunkSize; ++x)
                    if (chunk.Get(x, y, z) != dense[VoxelInChunk(x, y, z)])
                        return false;
        return true;
    }

    // Terrain-like content: solid below a wavy height, a few materials, some air pockets -- uniform, empty and full bricks.
    std::vector<MaterialId> TerrainLike(std::mt19937& rng, int materials)
    {
        std::vector<MaterialId> dense(ChunkVoxels, Air);
        std::uniform_int_distribution<int> pick(1, materials);
        const int base = 20 + static_cast<int>(rng() % 20);
        for (int32_t z = 0; z < ChunkSize; ++z)
            for (int32_t x = 0; x < ChunkSize; ++x)
            {
                const int height = base + static_cast<int>(6.0 * std::sin(x * 0.2) + 4.0 * std::cos(z * 0.15));
                for (int32_t y = 0; y < std::min(height, ChunkSize); ++y)
                    dense[VoxelInChunk(x, y, z)] = y < height - 4 ? 1 : static_cast<MaterialId>(pick(rng));
            }
        for (int i = 0; i < 2000; ++i)
            dense[rng() % ChunkVoxels] = Air;
        return dense;
    }
}

static void TestVoxelChunk()
{
    using namespace VoxelWorldTest;
    std::printf("\n== voxel chunk\n");

    // Keys: chunk positions round-trip through the 64-bit key, negatives included; the floor division puts voxel -1 in chunk -1.
    {
        std::mt19937 rng(3);
        int ok = 0, rounds = 10000;
        for (int i = 0; i < rounds; ++i)
        {
            const glm::ivec3 c(static_cast<int>(rng() % (2 * ChunkCoordLimit)) - ChunkCoordLimit, static_cast<int>(rng() % 2000) - 1000,
                               static_cast<int>(rng() % (2 * ChunkCoordLimit)) - ChunkCoordLimit);
            ok += ChunkFromKey(ChunkKey(c)) == c;
        }
        CHECK(ok == rounds && ChunkOf(glm::ivec3(-1, 63, 64)) == glm::ivec3(-1, 0, 1) && LocalOf(glm::ivec3(-1, 63, 64)) == glm::ivec3(63, 63, 0),
              "chunk keys round-trip ({} of {}), voxel -1 in chunk -1 at 63", ok, rounds);
    }

    // Random edits against a dense reference; bricks start empty, become full, and Compact turns uniform ones back.
    {
        std::mt19937 rng(11);
        VoxelChunk chunk;
        std::vector<MaterialId> dense(ChunkVoxels, Air);
        for (int i = 0; i < 200000; ++i)
        {
            const int32_t x = rng() % ChunkSize, y = rng() % ChunkSize, z = rng() % 16;
            const MaterialId material = static_cast<MaterialId>(rng() % 6);
            chunk.Set(x, y, z, material);
            dense[VoxelInChunk(x, y, z)] = material;
        }
        const bool sameBefore = SameAsDense(chunk, dense);
        const uint64_t hashBefore = chunk.Hash();
        chunk.FillBrick(BrickInChunk(7, 7, 7), 4);
        for (int32_t z = 56; z < 64; ++z)
            for (int32_t y = 56; y < 64; ++y)
                for (int32_t x = 56; x < 64; ++x)
                    dense[VoxelInChunk(x, y, z)] = 4;
        chunk.Compact();
        CHECK(sameBefore && SameAsDense(chunk, dense) && chunk.GetBrickKind(BrickInChunk(0, 0, 4)) == BrickKind::Empty &&
                  chunk.GetBrickKind(BrickInChunk(7, 7, 7)) == BrickKind::Uniform && chunk.GetBrickKind(BrickInChunk(0, 0, 0)) == BrickKind::Full &&
                  chunk.Palette().size() <= 6 && chunk.IndexBytes() == 1 && hashBefore != chunk.Hash(),
              "200000 random edits match a dense reference; bricks empty / uniform / full; palette {} entries", chunk.Palette().size());
    }

    // Assign / Read round trip; the hash depends on the voxels only (not the palette's order).
    {
        std::mt19937 rng(21);
        const std::vector<MaterialId> dense = TerrainLike(rng, 5);
        VoxelChunk assigned, edited;
        assigned.Assign(dense);
        for (int32_t z = ChunkSize - 1; z >= 0; --z) // another order: another palette order
            for (int32_t y = ChunkSize - 1; y >= 0; --y)
                for (int32_t x = ChunkSize - 1; x >= 0; --x)
                    edited.Set(x, y, z, dense[VoxelInChunk(x, y, z)]);
        std::vector<MaterialId> back(ChunkVoxels);
        assigned.Read(back);
        int uniform = 0, empty = 0, full = 0;
        for (int32_t b = 0; b < BrickCount; ++b)
        {
            uniform += assigned.GetBrickKind(b) == BrickKind::Uniform;
            empty += assigned.GetBrickKind(b) == BrickKind::Empty;
            full += assigned.GetBrickKind(b) == BrickKind::Full;
        }
        CHECK(back == dense && SameAsDense(edited, dense) && assigned.Hash() == edited.Hash() && assigned.Palette() != edited.Palette() &&
                  uniform > 0 && empty > 0 && full > 0,
              "assign / read round trip; same hash from another palette order; bricks: {} uniform, {} empty, {} full", uniform, empty, full);
    }

    // Occupancy masks: word z, bit y * 8 + x, solid = not air.
    {
        std::mt19937 rng(31);
        const std::vector<MaterialId> dense = TerrainLike(rng, 3);
        VoxelChunk chunk;
        chunk.Assign(dense);
        int right = 0;
        for (int32_t b = 0; b < BrickCount; ++b)
        {
            const auto words = chunk.BrickOccupancy(b);
            const int32_t bx = b % BricksPerAxis, by = (b / BricksPerAxis) % BricksPerAxis, bz = b / (BricksPerAxis * BricksPerAxis);
            bool same = true;
            for (int32_t z = 0; z < BrickSize; ++z)
                for (int32_t y = 0; y < BrickSize; ++y)
                    for (int32_t x = 0; x < BrickSize; ++x)
                        same &= ((words[z] >> (y * 8 + x)) & 1) == (dense[VoxelInChunk(bx * 8 + x, by * 8 + y, bz * 8 + z)] != Air ? 1u : 0u);
            right += same;
        }
        CHECK(right == BrickCount, "occupancy masks of {} bricks match the voxels", right);
    }

    // More than 256 materials: unused entries dropped first; still too many -> two-byte indices; Compact goes back to one byte.
    {
        VoxelChunk chunk;
        std::vector<MaterialId> dense(ChunkVoxels, Air);
        // 200 materials placed, then overwritten: their entries unused.
        for (int i = 0; i < 200; ++i)
            chunk.Set(i % 64, 0, i / 64, static_cast<MaterialId>(1000 + i));
        for (int i = 0; i < 200; ++i)
            chunk.Set(i % 64, 0, i / 64, Air);
        // 255 live materials fit one byte after dropping the 200 dead ones.
        for (int i = 0; i < 255; ++i)
        {
            chunk.Set(i % 64, 1, i / 64, static_cast<MaterialId>(2000 + i));
            dense[VoxelInChunk(i % 64, 1, i / 64)] = static_cast<MaterialId>(2000 + i);
        }
        const bool stayedNarrow = chunk.IndexBytes() == 1 && SameAsDense(chunk, dense);
        // 300 live materials need two bytes.
        for (int i = 0; i < 300; ++i)
        {
            chunk.Set(i % 64, 2, i / 64, static_cast<MaterialId>(5000 + i));
            dense[VoxelInChunk(i % 64, 2, i / 64)] = static_cast<MaterialId>(5000 + i);
        }
        const bool widened = chunk.IndexBytes() == 2 && SameAsDense(chunk, dense);
        const uint64_t hashWide = chunk.Hash();
        for (int i = 0; i < 300; ++i)
        {
            chunk.Set(i % 64, 2, i / 64, Air);
            dense[VoxelInChunk(i % 64, 2, i / 64)] = Air;
        }
        chunk.Compact();
        CHECK(stayedNarrow && widened && chunk.IndexBytes() == 1 && chunk.Palette().size() == 256 && SameAsDense(chunk, dense) && hashWide != chunk.Hash(),
              "palette: 255 live of 455 used stays one byte, 555 live widen to two, compact back to one byte ({} entries)", chunk.Palette().size());
    }

    // Encode / decode: the same voxels, revision and hash, one- and two-byte palettes; a terrain chunk compresses well.
    {
        std::mt19937 rng(41);
        int ok = 0, rounds = 6;
        size_t terrainBytes = 0;
        for (int round = 0; round < rounds; ++round)
        {
            std::vector<MaterialId> dense = TerrainLike(rng, round < 3 ? 6 : 1);
            if (round >= 3) // a colourful chunk: 600 materials (two-byte indices)
                for (int i = 0; i < 600; ++i)
                    dense[rng() % ChunkVoxels] = static_cast<MaterialId>(100 + i);
            VoxelChunk chunk, back;
            chunk.Assign(dense);
            chunk.Set(1, 2, 3, 7);
            dense[VoxelInChunk(1, 2, 3)] = 7;
            const std::vector<uint8_t> bytes = chunk.Encode();
            std::string error;
            const bool decoded = VoxelChunk::Decode(bytes, back, error);
            ok += decoded && SameAsDense(back, dense) && back.Hash() == chunk.Hash() && back.Revision() == chunk.Revision() &&
                  back.IndexBytes() == (round >= 3 ? 2u : 1u);
            if (round == 0)
                terrainBytes = bytes.size();
        }
        VoxelChunk empty, emptyBack;
        std::string error;
        const std::vector<uint8_t> emptyBytes = empty.Encode();
        CHECK(ok == rounds && VoxelChunk::Decode(emptyBytes, emptyBack, error) && emptyBack.IsEmpty() && terrainBytes < 40000 && emptyBytes.size() < 64,
              "encode / decode {} of {} (one- and two-byte palettes); terrain chunk {} bytes (of {} voxels), empty chunk {} bytes", ok, rounds,
              terrainBytes, ChunkVoxels, emptyBytes.size());
    }

    // Damaged data is refused, never crashes: every truncation, random byte flips, and a hand-made bad palette.
    {
        std::mt19937 rng(51);
        VoxelChunk chunk;
        chunk.Assign(TerrainLike(rng, 4));
        const std::vector<uint8_t> bytes = chunk.Encode();
        int truncatedRefused = 0;
        for (size_t size = 0; size < bytes.size(); size += 7)
        {
            VoxelChunk out;
            std::string error;
            truncatedRefused += !VoxelChunk::Decode(std::span<const uint8_t>(bytes.data(), size), out, error);
        }
        int read = 0, refused = 0;
        for (int i = 0; i < 2000; ++i)
        {
            std::vector<uint8_t> damaged = bytes;
            for (int flips = 0; flips < 1 + static_cast<int>(rng() % 4); ++flips)
                damaged[rng() % damaged.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
            VoxelChunk out;
            std::string error;
            (VoxelChunk::Decode(damaged, out, error) ? read : refused)++;
        }
        CHECK(truncatedRefused == static_cast<int>((bytes.size() + 6) / 7) && read + refused == 2000,
              "every truncation refused ({}); 2000 damaged copies: {} read, {} refused, none crashed", truncatedRefused, read, refused);
    }
}

static void TestVoxelGenerator()
{
    using namespace VoxelWorldTest;
    std::printf("\n== voxel material table + generator\n");

    // Material table: YAML round trip (every field), ids kept, air implied; malformed files refused; nearest look.
    {
        VoxelMaterialTable table;
        const VoxelGeneratorSettings terrain = MakeDefaultTerrain(table);
        VoxelMaterial lamp;
        lamp.Name = "Lamp";
        lamp.BaseColor = glm::vec4(1.0f, 0.8f, 0.4f, 1.0f);
        lamp.Emission = glm::vec3(1.0f, 0.8f, 0.4f);
        lamp.EmissionStrength = 12.5f;
        lamp.TopTexture = 0x1234567890abcdefull;
        lamp.FootstepSurface = "Glass";
        const MaterialId lampId = table.Add(lamp);
        VoxelMaterialTable back;
        std::string error;
        const bool read = VoxelMaterialTable::Deserialize(table.Serialize(), back, error);
        bool same = read && back.Size() == table.Size();
        for (size_t id = 0; same && id < table.Size(); ++id)
            same = back.Get(static_cast<MaterialId>(id)) == table.Get(static_cast<MaterialId>(id));
        VoxelMaterialTable bad;
        const bool gapRefused = !VoxelMaterialTable::Deserialize("VoxelMaterials:\n  - {Id: 2, Name: x, BaseColor: [1,1,1,1], Emission: [0,0,0]}\n", bad, error);
        const bool shapeRefused = !VoxelMaterialTable::Deserialize("VoxelMaterials:\n  - {Id: 1, Name: x, BaseColor: [1,1], Emission: [0,0,0]}\n", bad, error);
        const bool garbageRefused = !VoxelMaterialTable::Deserialize("{{{ not yaml", bad, error);
        VoxelMaterial greyish;
        greyish.BaseColor = glm::vec4(0.31f, 0.29f, 0.30f, 1.0f);
        VoxelMaterial glowing = greyish;
        glowing.Emission = glm::vec3(1.0f);
        glowing.EmissionStrength = 3.0f;
        CHECK(same && gapRefused && shapeRefused && garbageRefused && table.Nearest(greyish) == terrain.BulkMaterial && table.Nearest(glowing) == lampId &&
                  table.Find("Dirt") == terrain.SubsurfaceMaterial && table.Get(0).Name == "Air",
              "material table: YAML round trip of {} materials, gaps / bad vectors / garbage refused, nearest: grey -> Stone, a glow -> Lamp", table.Size());
    }

    VoxelMaterialTable table;
    const VoxelGeneratorSettings settings = MakeDefaultTerrain(table);

    // Settings: YAML round trip, binary hash stable and sensitive; a tree that cannot be read is reported.
    {
        VoxelGeneratorSettings back;
        std::string error;
        const bool read = VoxelGeneratorSettings::Deserialize(settings.Serialize(), back, error);
        VoxelGeneratorSettings changed = settings;
        changed.Ores[0].Threshold += 0.01f;
        VoxelGeneratorSettings broken = settings;
        broken.CaveTree = "this is not a node tree";
        VoxelGenerator brokenGenerator(broken, 0.1f, 1);
        VoxelGenerator good(settings, 0.1f, 1);
        CHECK(read && back == settings && back.Hash() == settings.Hash() && changed.Hash() != settings.Hash() && good.IsValid() && !brokenGenerator.IsValid() &&
                  brokenGenerator.Error().find("caves") != std::string::npos,
              "generator settings: round trip and hash; an unreadable cave tree reported ('{}')", brokenGenerator.Error());
    }

    // Determinism: the same seed gives the same chunks on every SIMD level (strict floating point); another seed other chunks.
    {
        const std::vector<glm::ivec3> chunks = { { 0, 0, 0 }, { 0, -1, 0 }, { 3, -2, -5 }, { -7, -1, 2 }, { 1000, -3, -2000 } };
        auto hashes = [&](SimdLevel level, int32_t seed)
        {
            VoxelGenerator generator(settings, 0.1f, seed, level);
            std::vector<uint64_t> out;
            for (const glm::ivec3& c : chunks)
            {
                VoxelChunk chunk;
                generator.Generate(c, true, chunk);
                out.push_back(chunk.Hash());
            }
            return out;
        };
        const std::vector<uint64_t> best = hashes(SimdLevel::Best, 7);
        // Every level this build has (x86: SSE2, SSE4.1, AVX2, AVX-512 -- no scalar; a CPU without AVX-512 runs AVX2 for it).
        int levelsSame = 0, levelsBuilt = 0;
        std::string levelNames;
        for (auto [level, name] : { std::pair{ SimdLevel::Scalar, "scalar" }, std::pair{ SimdLevel::SSE2, "SSE2" }, std::pair{ SimdLevel::SSE41, "SSE4.1" },
                                    std::pair{ SimdLevel::AVX2, "AVX2" }, std::pair{ SimdLevel::AVX512, "AVX-512" } })
        {
            if (!VoxelGenerator(settings, 0.1f, 7, level).IsValid())
                continue;
            ++levelsBuilt;
            const bool same = hashes(level, 7) == best;
            levelsSame += same;
            levelNames += std::string(levelNames.empty() ? "" : " / ") + name + (same ? "" : " (DIFFERS)");
        }
        VoxelGenerator scalar(settings, 0.1f, 7, SimdLevel::Scalar);
        const std::vector<uint64_t> again = hashes(SimdLevel::Best, 7);
        const std::vector<uint64_t> otherSeed = hashes(SimdLevel::Best, 8);
        int differ = 0;
        for (size_t i = 0; i < best.size(); ++i)
            differ += best[i] != otherSeed[i];
        CHECK(levelsBuilt >= 2 && levelsSame == levelsBuilt && again == best && differ >= 3 &&
                  (scalar.IsValid() || scalar.Error().find("not built") != std::string::npos),
              "determinism: {} chunks bit-identical on {} ({} levels), repeatable; another seed changes {}", chunks.size(), levelNames, levelsBuilt, differ);
    }

    // Terrain rules: air above each column's surface, the surface material on it, subsurface below, bulk deeper (caves stay
    // CaveTopDepth below; ore only where bulk would be); a chunk above the column is empty, one deep below without caves / ore is bulk.
    {
        VoxelGenerator generator(settings, 0.1f, 42);
        const int32_t subsurface = 30, caveTop = 40; // 3 m / 4 m at 10 cm
        int rulesBroken = 0, carved = 0, ore = 0, checkedChunks = 0, columns = 0;
        ColumnHeights column;
        // Columns along x until caves showed up (a cave is ~30 m across: one 6.4 m column may hold none), every rule checked in each.
        for (int32_t cx = 0; cx < 24 && (carved == 0 || columns < 2); ++cx, ++columns)
        {
        column = generator.Column(cx, -3);
        for (int32_t cy = FloorDiv(column.MinSurface, ChunkSize) - 3; cy <= FloorDiv(column.MaxSurface, ChunkSize) + 1; ++cy)
        {
            VoxelChunk chunk;
            generator.Generate(glm::ivec3(cx, cy, -3), column, true, chunk);
            ++checkedChunks;
            for (int32_t z = 0; z < ChunkSize; ++z)
                for (int32_t x = 0; x < ChunkSize; ++x)
                {
                    const int32_t h = column.Heights[x + z * ChunkSize];
                    for (int32_t y = 0; y < ChunkSize; ++y)
                    {
                        const int32_t wy = cy * ChunkSize + y;
                        const MaterialId m = chunk.Get(x, y, z);
                        bool ok;
                        if (wy > h) ok = m == Air;
                        else if (wy == h) ok = m == settings.SurfaceMaterial;
                        else if (wy > h - subsurface) ok = m == settings.SubsurfaceMaterial;
                        else if (wy > h - caveTop) ok = m == settings.BulkMaterial || m == settings.Ores[0].Material;
                        else ok = m == settings.BulkMaterial || m == settings.Ores[0].Material || m == Air;
                        rulesBroken += !ok;
                        carved += wy < h && m == Air;
                        ore += m == settings.Ores[0].Material;
                    }
                }
        }
        }
        VoxelGeneratorSettings plain = settings;
        plain.CaveTree.clear();
        plain.Ores.clear();
        VoxelGenerator plainGenerator(plain, 0.1f, 42);
        VoxelChunk high, deep;
        plainGenerator.Generate(glm::ivec3(2, column.MaxSurface / ChunkSize + 3, -3), false, high);
        plainGenerator.Generate(glm::ivec3(2, column.MinSurface / ChunkSize - 4, -3), false, deep);
        bool deepBulk = true;
        for (int32_t b = 0; b < BrickCount; ++b)
            deepBulk &= deep.GetBrickKind(b) == BrickKind::Uniform && deep.GetBrickMaterial(b) == plain.BulkMaterial;
        CHECK(rulesBroken == 0 && carved > 0 && ore > 0 && high.IsEmpty() && deepBulk,
              "terrain rules hold in {} chunks of {} columns: {} cave voxels, {} ore; above empty, deep wholly bulk", checkedChunks, columns, carved, ore);
    }

    // The secret layer only adds ore where the public layer has bulk (a client without it sees bulk there).
    {
        VoxelGenerator generator(settings, 0.1f, 5);
        int mismatches = 0, revealedOre = 0;
        for (int32_t cy = -4; cy <= -1; ++cy)
        {
            VoxelChunk publicOnly, withSecret;
            generator.Generate(glm::ivec3(1, cy, 1), false, publicOnly);
            generator.Generate(glm::ivec3(1, cy, 1), true, withSecret);
            std::vector<MaterialId> a(ChunkVoxels), b(ChunkVoxels);
            publicOnly.Read(a);
            withSecret.Read(b);
            for (int32_t i = 0; i < ChunkVoxels; ++i)
            {
                if (a[i] == b[i])
                    continue;
                const bool oreInBulk = a[i] == settings.BulkMaterial && b[i] == settings.Ores[0].Material;
                mismatches += !oreInBulk;
                revealedOre += oreInBulk;
            }
        }
        CHECK(mismatches == 0 && revealedOre > 0, "secret layer: {} ore voxels, all inside public bulk ({} other differences)", revealedOre, mismatches);
    }

    // Voxel size: the same generator at 1 m blocks (Minecraft-style) gives the same hills in metres as at 10 cm.
    {
        VoxelGenerator tiny(settings, 0.1f, 9), blocks(settings, 1.0f, 9);
        const ColumnHeights tinyColumn = tiny.Column(0, 0);   // 6.4 m x 6.4 m
        const ColumnHeights blockColumn = blocks.Column(0, 0); // 64 m x 64 m
        double worst = 0.0;
        for (int32_t z = 0; z < 6; ++z)
            for (int32_t x = 0; x < 6; ++x)
            {
                // Block (x, z)'s centre is (x + 0.5) m: the 10 cm voxel 10 x + 5 starts there, 10 x + 4 ends there -- compare with both.
                const double blockTop = (blockColumn.Heights[x + z * ChunkSize] + 1) * 1.0;
                const double a = (tinyColumn.Heights[(10 * x + 4) + (10 * z + 4) * ChunkSize] + 1) * 0.1;
                const double b = (tinyColumn.Heights[(10 * x + 5) + (10 * z + 5) * ChunkSize] + 1) * 0.1;
                worst = std::max(worst, std::min(std::abs(blockTop - a), std::abs(blockTop - b)));
            }
        CHECK(worst <= 1.1, "voxel size: 1 m blocks follow the 10 cm terrain within a block ({:.2f} m at worst)", worst);
    }
}

static void TestVoxelWorld()
{
    using namespace VoxelWorldTest;
    namespace Vox = Nox::Vox;
    std::printf("\n== voxel world + stamping\n");
    VoxelMaterialTable table;
    const VoxelGeneratorSettings settings = MakeDefaultTerrain(table);
    VoxelWorldSettings worldSettings;
    worldSettings.Seed = 77;
    auto generator = std::make_shared<VoxelGenerator>(settings, worldSettings.VoxelSize, worldSettings.Seed);

    // A generated 10 cm terrain in memory (VX1's "done when"): a region loaded on worker threads equals the same chunks loaded one by
    // one; vertical limits respected.
    {
        VoxelWorld parallel(worldSettings, generator), serial(worldSettings, generator);
        const glm::ivec3 low(-4, -6, -4), high(3, 2, 3);
        const auto start = std::chrono::steady_clock::now();
        const size_t added = parallel.LoadRegion(low, high);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        int same = 0;
        parallel.ForEachChunk([&](const glm::ivec3& c, const VoxelChunk& chunk) { same += serial.Load(c)->Hash() == chunk.Hash(); });
        size_t solidChunks = 0;
        parallel.ForEachChunk([&](const glm::ivec3&, const VoxelChunk& chunk) { solidChunks += !chunk.IsEmpty(); });
        VoxelWorld limits(worldSettings, generator);
        const bool limitsKept = limits.Load(glm::ivec3(0, limits.MaxChunkY() + 1, 0)) == nullptr && limits.Load(glm::ivec3(0, limits.MinChunkY() - 1, 0)) == nullptr &&
                                limits.MinChunkY() == -80 && limits.MaxChunkY() == 79 && limits.LoadRegion(glm::ivec3(0, 70, 0), glm::ivec3(0, 90, 0)) == 10;
        CHECK(added == 8 * 9 * 8 && same == static_cast<int>(added) && solidChunks > 0 && limitsKept,
              "region of {} chunks ({} with voxels, {:.1f} x {:.1f} m) generated in {:.0f} ms on worker threads, {:.1f} MB; same as one by one; -512 .. 512 m kept",
              added, solidChunks, 8 * 6.4, 8 * 6.4, ms, parallel.MemoryBytes() / (1024.0 * 1024.0));
    }

    // Voxels across chunk borders and negative coordinates; a set voxel reads back and lands in the right chunk.
    {
        VoxelWorld world(worldSettings); // no generator: empty chunks
        std::mt19937 rng(61);
        std::vector<std::pair<glm::ivec3, MaterialId>> writes;
        for (int i = 0; i < 20000; ++i)
        {
            const glm::ivec3 v(static_cast<int>(rng() % 400) - 200, static_cast<int>(rng() % 400) - 200, static_cast<int>(rng() % 400) - 200);
            const MaterialId m = static_cast<MaterialId>(1 + rng() % 9);
            world.SetVoxel(v, m);
            writes.emplace_back(v, m);
        }
        std::map<std::tuple<int, int, int>, MaterialId> last;
        for (const auto& [v, m] : writes)
            last[{ v.x, v.y, v.z }] = m;
        int right = 0;
        for (const auto& [key, m] : last)
        {
            const glm::ivec3 v(std::get<0>(key), std::get<1>(key), std::get<2>(key));
            const VoxelChunk* chunk = world.Find(ChunkOf(v));
            const glm::ivec3 local = LocalOf(v);
            right += world.GetVoxel(v) == m && chunk && chunk->Get(local.x, local.y, local.z) == m;
        }
        CHECK(right == static_cast<int>(last.size()) && world.GetVoxel(glm::ivec3(5000, 0, 0)) == Air,
              "{} voxels set across {} chunks (negative coordinates, borders) read back", right, world.ChunkCount());
    }

    // Stamping: placement by hand (identity and a 90 degree turn), every rotation keeps the voxels and their shape; materials added once,
    // reused on a second stamp, or mapped to the nearest; emission as the import computes it.
    {
        Vox::Scene scene;
        scene.Palette = Vox::DefaultPalette();
        scene.Palette[1] = 0xff0000ffu; // red
        scene.Palette[2] = 0xff00ff00u; // green
        scene.Materials[2].Type = Vox::MaterialType::Emit;
        scene.Materials[2].Emission = 0.5f;
        scene.Materials[2].Flux = 1.0f;
        Vox::Model model;
        model.SizeX = 3; model.SizeY = 2; model.SizeZ = 4;
        model.Voxels.assign(24, 0);
        model.Voxels[0] = 1;                       // (0, 0, 0)
        model.Voxels[2 + 3 * (1 + 2 * 3)] = 2;     // (2, 1, 3)
        model.Voxels[1 + 3 * (0 + 2 * 2)] = 1;     // (1, 0, 2)
        model.VoxelCount = 3;
        scene.Models.push_back(model);
        Vox::Instance instance;
        instance.Translation = { 10, 20, 30 };
        scene.Instances.push_back(instance);

        // Identity: centre (v + 0.5 - (1, 1, 2)) + t; engine (x, z, -y), floored.
        const glm::ivec3 expected0(9, 28, -20);  // v (0,0,0): centre (9.5, 19.5, 28.5) -> (9, 28, floor(-19.5) = -20)
        const glm::ivec3 expected1(11, 31, -21); // v (2,1,3): centre (11.5, 20.5, 31.5) -> (11, 31, -21)
        const bool identity = StampedCell(scene, instance, 0, 0, 0) == expected0 && StampedCell(scene, instance, 2, 1, 3) == expected1;
        // 90 degrees about MagicaVoxel's z: x' = -y, y' = x (rows: x <- -y, y <- x, z <- z).
        Vox::Instance turned = instance;
        turned.Rotate.Column = { 1, 0, 2 };
        turned.Rotate.Sign = { -1, 1, 1 };
        const bool turn = StampedCell(scene, turned, 2, 1, 3) == glm::ivec3(9, 31, -22); // local (1.5, 0.5, 1.5) -> (-0.5, 1.5, 1.5) + t

        int rotationsOk = 0, rotations = 0;
        for (int byte = 0; byte < 128; ++byte)
        {
            Vox::Rotation rotation;
            if (!Vox::Rotation::FromByte(static_cast<uint8_t>(byte), rotation))
                continue;
            ++rotations;
            Vox::Instance placed = instance;
            placed.Rotate = rotation;
            std::set<std::tuple<int, int, int>> cells;
            glm::ivec3 lo(1 << 30), hi(-(1 << 30));
            Vox::Model full = model;
            full.Voxels.assign(24, 1);
            Vox::Scene fullScene = scene;
            fullScene.Models[0] = full;
            for (int32_t z = 0; z < 4; ++z)
                for (int32_t y = 0; y < 2; ++y)
                    for (int32_t x = 0; x < 3; ++x)
                    {
                        const glm::ivec3 c = StampedCell(fullScene, placed, x, y, z);
                        cells.insert({ c.x, c.y, c.z });
                        lo = glm::min(lo, c);
                        hi = glm::max(hi, c);
                    }
            const glm::ivec3 extent = hi - lo + glm::ivec3(1);
            rotationsOk += cells.size() == 24 && extent.x * extent.y * extent.z == 24; // a solid box stays a solid box, no two voxels in one cell
        }

        VoxelWorld world(worldSettings); // empty world
        VoxelMaterialTable stampTable;
        StampOptions options;
        options.Pivot = StampPivot::FileOrigin;
        options.Origin = glm::ivec3(100, 0, -100);
        const StampResult first = StampVoxScene(world, stampTable, scene, options);
        const size_t tableAfterFirst = stampTable.Size();
        const StampResult second = StampVoxScene(world, stampTable, scene, options);
        const VoxelMaterial& green = stampTable.Get(first.Mapping[2]);
        const bool placed = world.GetVoxel(expected0 + options.Origin) == first.Mapping[1] && world.GetVoxel(expected1 + options.Origin) == first.Mapping[2];
        VoxelMaterialTable nearestTable;
        MakeDefaultTerrain(nearestTable); // grass, dirt, stone, ore -- none red
        StampOptions nearest = options;
        nearest.Materials = StampMaterials::Nearest;
        const size_t nearestBefore = nearestTable.Size();
        const StampResult mapped = StampVoxScene(world, nearestTable, scene, nearest);
        StampOptions bottom;
        const StampResult centred = StampVoxScene(world, stampTable, scene, bottom);
        CHECK(identity && turn && rotationsOk == rotations && rotations == 48 && placed && first.Voxels == 3 && tableAfterFirst == 3 &&
                  stampTable.Size() == 3 && second.Mapping == first.Mapping && std::abs(green.EmissionStrength - 0.5f * 4.0f) < 1e-6f &&
                  nearestTable.Size() == nearestBefore && mapped.Mapping[1] != Air && centred.Min.y == 0 && centred.Min.x + centred.Max.x <= 1,
              "stamp: placement by hand, {} rotations keep a box, materials added once ({} entries) and reused, nearest maps without adding, emission {}",
              rotations, stampTable.Size() - 1, green.EmissionStrength);
    }
}
