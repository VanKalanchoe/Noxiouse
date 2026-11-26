// The MagicaVoxel reader and mesher (NoxCore/Voxel, docs/Voxel_Plan_2026-11.md §4 / §8, VX0) on .vox files built here byte by byte:
// every chunk kind, the default palette, materials, the scene graph's rotations and hidden nodes / layers, broken files (every
// truncation and random corruption: an error, never a crash), and the meshes (closed surfaces, outward faces, greedy merging, the
// glass rule, placements against MagicaVoxel's own formula). Included by ToolsTest.cpp (needs CHECK).
#include "NoxCore/Voxel/VoxMesher.h"

#include <cstring>
#include <map>
#include <random>

namespace VoxTest
{
    using namespace Nox::Vox;

    // ---- a .vox writer for the tests ----
    struct Bytes
    {
        std::vector<uint8_t> Data;
        void U32(uint32_t v) { for (int i = 0; i < 4; ++i) Data.push_back(static_cast<uint8_t>(v >> (8 * i))); }
        void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
        void F32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); U32(bits); }
        void Str(const std::string& s) { U32(static_cast<uint32_t>(s.size())); Data.insert(Data.end(), s.begin(), s.end()); }
        void Dict(const std::vector<std::pair<std::string, std::string>>& d) { U32(static_cast<uint32_t>(d.size())); for (auto& [k, v] : d) { Str(k); Str(v); } }
        void Append(const Bytes& other) { Data.insert(Data.end(), other.Data.begin(), other.Data.end()); }
    };

    void Chunk(Bytes& out, const char* id, const Bytes& content)
    {
        out.Data.insert(out.Data.end(), id, id + 4);
        out.U32(static_cast<uint32_t>(content.Data.size()));
        out.U32(0);
        out.Append(content);
    }

    struct TestModel
    {
        int32_t X, Y, Z;
        std::vector<std::array<uint8_t, 4>> Voxels;
    };

    void ModelChunks(Bytes& out, const TestModel& model)
    {
        Bytes size;
        size.I32(model.X); size.I32(model.Y); size.I32(model.Z);
        Chunk(out, "SIZE", size);
        Bytes xyzi;
        xyzi.U32(static_cast<uint32_t>(model.Voxels.size()));
        for (const auto& v : model.Voxels) xyzi.Data.insert(xyzi.Data.end(), v.begin(), v.end());
        Chunk(out, "XYZI", xyzi);
    }

    std::vector<uint8_t> File(const Bytes& children, uint32_t version = 200)
    {
        Bytes file;
        file.Data = { 'V', 'O', 'X', ' ' };
        file.U32(version);
        file.Data.insert(file.Data.end(), { 'M', 'A', 'I', 'N' });
        file.U32(0);
        file.U32(static_cast<uint32_t>(children.Data.size()));
        file.Append(children);
        return file.Data;
    }

    void Transform(Bytes& out, int32_t id, int32_t child, int32_t layer, const std::vector<std::pair<std::string, std::string>>& attributes,
                   const std::vector<std::pair<std::string, std::string>>& frame)
    {
        Bytes c;
        c.I32(id); c.Dict(attributes); c.I32(child); c.I32(-1); c.I32(layer); c.U32(1); c.Dict(frame);
        Chunk(out, "nTRN", c);
    }
    void Group(Bytes& out, int32_t id, const std::vector<int32_t>& children)
    {
        Bytes c;
        c.I32(id); c.Dict({}); c.U32(static_cast<uint32_t>(children.size()));
        for (int32_t child : children) c.I32(child);
        Chunk(out, "nGRP", c);
    }
    void Shape(Bytes& out, int32_t id, int32_t model)
    {
        Bytes c;
        c.I32(id); c.Dict({}); c.U32(1); c.I32(model); c.Dict({});
        Chunk(out, "nSHP", c);
    }
    // A shape animated through its models: (model, frame) pairs.
    void AnimatedShape(Bytes& out, int32_t id, const std::vector<std::pair<int32_t, int32_t>>& frames)
    {
        Bytes c;
        c.I32(id); c.Dict({}); c.U32(static_cast<uint32_t>(frames.size()));
        for (const auto& [model, frame] : frames) { c.I32(model); c.Dict({ { "_f", std::to_string(frame) } }); }
        Chunk(out, "nSHP", c);
    }
    void Layer(Bytes& out, int32_t id, bool hidden)
    {
        Bytes c;
        c.I32(id); c.Dict(hidden ? std::vector<std::pair<std::string, std::string>>{ { "_hidden", "1" } } : std::vector<std::pair<std::string, std::string>>{});
        c.I32(-1);
        Chunk(out, "LAYR", c);
    }
    void Matl(Bytes& out, int32_t id, const std::vector<std::pair<std::string, std::string>>& dict)
    {
        Bytes c;
        c.I32(id); c.Dict(dict);
        Chunk(out, "MATL", c);
    }

    // ---- geometry checks ----
    // Signed volume (divergence theorem): the voxel count of a closed, outward-facing surface, whatever its T-junctions.
    double SignedVolume(const VoxGeometry& g)
    {
        double volume = 0.0;
        for (size_t t = 0; t + 2 < g.Indices.size(); t += 3)
        {
            const glm::dvec3 a(g.Positions[g.Indices[t]]), b(g.Positions[g.Indices[t + 1]]), c(g.Positions[g.Indices[t + 2]]);
            volume += glm::dot(a, glm::cross(b, c)) / 6.0;
        }
        return volume;
    }
    // Every triangle's winding agrees with its normal (counter-clockwise seen from outside), and the normal is axis-aligned unit.
    bool WindingMatchesNormals(const VoxGeometry& g)
    {
        for (size_t t = 0; t + 2 < g.Indices.size(); t += 3)
        {
            const glm::vec3 a = g.Positions[g.Indices[t]], b = g.Positions[g.Indices[t + 1]], c = g.Positions[g.Indices[t + 2]];
            const glm::vec3 n = g.Normals[g.Indices[t]];
            if (glm::dot(glm::cross(b - a, c - a), n) <= 0.0f || std::abs(glm::length(n) - 1.0f) > 1e-6f)
                return false;
        }
        return true;
    }
    double Area(const VoxGeometry& g)
    {
        double area = 0.0;
        for (size_t t = 0; t + 2 < g.Indices.size(); t += 3)
        {
            const glm::dvec3 a(g.Positions[g.Indices[t]]), b(g.Positions[g.Indices[t + 1]]), c(g.Positions[g.Indices[t + 2]]);
            area += glm::length(glm::cross(b - a, c - a)) * 0.5;
        }
        return area;
    }

    // Faces the rule says exist (a voxel towards an empty cell, or an opaque voxel towards a transparent one), counted per kind.
    void ExpectedFaces(const Model& m, const std::array<Material, 256>& materials, size_t& opaque, size_t& transparent)
    {
        opaque = transparent = 0;
        auto clear = [&](uint8_t i) { return i != 0 && materials[i].IsTransparent(); };
        const int offsets[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for (int z = 0; z < m.SizeZ; ++z)
            for (int y = 0; y < m.SizeY; ++y)
                for (int x = 0; x < m.SizeX; ++x)
                {
                    const uint8_t self = m.At(x, y, z);
                    if (!self)
                        continue;
                    for (const auto& o : offsets)
                    {
                        const uint8_t n = m.At(x + o[0], y + o[1], z + o[2]);
                        if (n == 0 || (clear(n) && !clear(self)))
                            (clear(self) ? transparent : opaque)++;
                    }
                }
    }

    Model RandomModel(std::mt19937& rng, int sx, int sy, int sz, float fill, int colors)
    {
        Model m;
        m.SizeX = sx; m.SizeY = sy; m.SizeZ = sz;
        m.Voxels.assign(static_cast<size_t>(sx) * sy * sz, 0);
        std::uniform_real_distribution<float> chance(0.0f, 1.0f);
        std::uniform_int_distribution<int> color(1, colors);
        for (uint8_t& v : m.Voxels)
            if (chance(rng) < fill)
            {
                v = static_cast<uint8_t>(color(rng));
                ++m.VoxelCount;
            }
        return m;
    }
}

static void TestVoxReader()
{
    using namespace VoxTest;
    std::printf("\n== .vox reader\n");

    // Default palette: MagicaVoxel's (white first, its 6^3 cube blue fastest, then the red / green / blue / grey ramps).
    const auto& palette = DefaultPalette();
    CHECK(palette[1] == 0xffffffffu && palette[2] == 0xffccffffu && palette[6] == 0xff00ffffu && palette[7] == 0xffffccffu,
          "default palette starts white, blue varies fastest: {:08x} {:08x} {:08x} {:08x}", palette[1], palette[2], palette[6], palette[7]);
    CHECK(palette[215] == 0xff330000u && palette[216] == 0xff0000eeu && palette[226] == 0xff00ee00u && palette[236] == 0xffee0000u &&
          palette[246] == 0xffeeeeeeu && palette[255] == 0xff111111u,
          "default palette ramps: {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}", palette[215], palette[216], palette[226], palette[236], palette[246], palette[255]);

    // Old animations (no scene graph, several models = frames): the first frame placed, the others kept as models.
    {
        Bytes body;
        Bytes pack; pack.U32(3);
        Chunk(body, "PACK", pack);
        for (int frame = 0; frame < 3; ++frame)
            ModelChunks(body, { 2, 2, 2, { { static_cast<uint8_t>(frame % 2), 0, 0, 1 } } });
        Scene scene;
        std::string error;
        const auto file = File(body, 150);
        const bool ok = ReadVox(file.data(), file.size(), scene, error);
        CHECK(ok && scene.Models.size() == 3 && scene.Instances.size() == 1 && scene.Instances[0].ModelIndex == 0,
              "an old animation: {} models, {} placed ({})", scene.Models.size(), scene.Instances.size(), error);
    }

    // One model, no RGBA, no scene graph (an old file): one instance at the origin, the default palette.
    {
        Bytes body;
        ModelChunks(body, { 3, 2, 1, { { 0, 0, 0, 5 }, { 2, 1, 0, 7 } } });
        Scene scene;
        std::string error;
        const bool ok = ReadVox(File(body, 150).data(), File(body, 150).size(), scene, error);
        CHECK(ok && scene.Version == 150 && scene.Models.size() == 1 && scene.Models[0].VoxelCount == 2 && scene.Models[0].At(0, 0, 0) == 5 &&
              scene.Models[0].At(2, 1, 0) == 7 && scene.Models[0].At(1, 0, 0) == 0 && !scene.HasPaletteChunk && scene.Palette[5] == palette[5] &&
              scene.Instances.size() == 1 && !scene.HasSceneGraph, "a minimal file: models, voxels, default palette, one instance ({})", error);
    }

    // RGBA: entry k is colour index k + 1; MATL types and values (_ri wins over _ior; diffuse is matte whatever _rough says).
    {
        Bytes body;
        ModelChunks(body, { 1, 1, 1, { { 0, 0, 0, 1 } } });
        Bytes rgba;
        for (int k = 0; k < 256; ++k)
            rgba.U32(0xff000000u | static_cast<uint32_t>(k));
        Chunk(body, "RGBA", rgba);
        Matl(body, 1, { { "_type", "_diffuse" }, { "_rough", "0.3" } });
        Matl(body, 2, { { "_type", "_metal" }, { "_rough", "0.25" }, { "_metal", "0.75" } });
        Matl(body, 3, { { "_type", "_glass" }, { "_rough", "0.05" }, { "_trans", "0.6" }, { "_ior", "0.4" } });
        Matl(body, 4, { { "_type", "_glass" }, { "_ior", "0.4" }, { "_ri", "1.33" } });
        Matl(body, 5, { { "_type", "_emit" }, { "_emit", "0.5" }, { "_flux", "2" } });
        Matl(body, 6, { { "_type", "_blend" }, { "_alpha", "0.25" }, { "_metal", "0.5" }, { "_rough", "0.4" } });
        Matl(body, 0, { { "_type", "_metal" } });   // no palette entry: ignored
        Matl(body, 300, { { "_type", "_metal" } }); // likewise
        Scene scene;
        std::string error;
        const auto file = File(body);
        const bool ok = ReadVox(file.data(), file.size(), scene, error);
        CHECK(ok && scene.HasPaletteChunk && scene.Palette[1] == 0xff000000u && scene.Palette[255] == 0xff0000feu && scene.Palette[0] == 0,
              "RGBA entry k is colour index k + 1: {:08x} {:08x} ({})", scene.Palette[1], scene.Palette[255], error);
        const auto& m = scene.Materials;
        CHECK(m[1].Type == MaterialType::Diffuse && m[1].Roughness == 1.0f && m[1].Metallic == 0.0f, "diffuse: matte (rough {})", m[1].Roughness);
        CHECK(m[2].Type == MaterialType::Metal && m[2].Roughness == 0.25f && m[2].Metallic == 0.75f, "metal: rough {} metal {}", m[2].Roughness, m[2].Metallic);
        CHECK(m[3].Type == MaterialType::Glass && m[3].IsTransparent() && m[3].Transparency == 0.6f && std::abs(m[3].IOR - 1.4f) < 1e-6f &&
              m[3].Roughness == 0.05f, "glass: trans {} ior {} (from _ior + 1)", m[3].Transparency, m[3].IOR);
        CHECK(std::abs(m[4].IOR - 1.33f) < 1e-6f && m[4].Transparency == 1.0f, "glass: _ri wins over _ior ({}), clear by default", m[4].IOR);
        CHECK(m[5].Type == MaterialType::Emit && m[5].Emission == 0.5f && m[5].Flux == 2.0f && !m[5].IsTransparent(), "emit: {} flux {}", m[5].Emission, m[5].Flux);
        CHECK(m[6].Type == MaterialType::Blend && m[6].Transparency == 0.25f && m[6].Metallic == 0.5f && m[6].IsTransparent(), "blend: alpha as transparency {}", m[6].Transparency);
        CHECK(m[7].Type == MaterialType::Diffuse && m[0].Type == MaterialType::Diffuse, "entries without MATL (and 0 / 256) stay diffuse");
    }

    // MATT (0.98): metal with a roughness value, glass with IOR, emissive with power.
    {
        Bytes body;
        ModelChunks(body, { 1, 1, 1, { { 0, 0, 0, 1 } } });
        Bytes metal;
        metal.I32(9); metal.I32(1); metal.F32(0.8f); metal.U32(1u << 1); metal.F32(0.35f);
        Chunk(body, "MATT", metal);
        Bytes glass;
        glass.I32(10); glass.I32(2); glass.F32(0.9f); glass.U32((1u << 1) | (1u << 3)); glass.F32(0.2f); glass.F32(0.5f);
        Chunk(body, "MATT", glass);
        Bytes emit;
        emit.I32(11); emit.I32(3); emit.F32(0.7f); emit.U32((1u << 5) | (1u << 7)); emit.F32(3.0f);
        Chunk(body, "MATT", emit);
        Scene scene;
        std::string error;
        const auto file = File(body, 150);
        const bool ok = ReadVox(file.data(), file.size(), scene, error);
        const auto& m = scene.Materials;
        CHECK(ok && m[9].Type == MaterialType::Metal && m[9].Metallic == 0.8f && m[9].Roughness == 0.35f && m[10].Type == MaterialType::Glass &&
              m[10].Transparency == 0.9f && std::abs(m[10].IOR - 1.5f) < 1e-6f && m[10].Roughness == 0.2f && m[11].Type == MaterialType::Emit &&
              m[11].Emission == 0.7f && m[11].Flux == 3.0f, "MATT materials: metal / glass / emissive ({})", error);
    }

    // Rotations: all 48 valid bytes round-trip; determinant and products agree with matrices; 3 invalid column pairs are refused.
    {
        int valid = 0, roundTrips = 0, determinants = 0, products = 0;
        std::vector<Rotation> all;
        for (int byte = 0; byte < 128; ++byte)
        {
            Rotation r;
            if (!Rotation::FromByte(static_cast<uint8_t>(byte), r))
                continue;
            ++valid;
            all.push_back(r);
            roundTrips += r.ToByte() == byte;
            glm::mat3 m(0.0f);
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    m[col][row] = static_cast<float>(r.Entry(row, col));
            determinants += static_cast<int>(glm::determinant(m)) == r.Determinant();
        }
        for (const Rotation& a : all)
            for (const Rotation& b : all)
            {
                const Rotation ab = a * b;
                bool same = true;
                for (int row = 0; row < 3; ++row)
                    for (int col = 0; col < 3; ++col)
                    {
                        int sum = 0;
                        for (int k = 0; k < 3; ++k)
                            sum += a.Entry(row, k) * b.Entry(k, col);
                        same &= sum == ab.Entry(row, col);
                    }
                products += same;
            }
        Rotation dummy;
        const bool refused = !Rotation::FromByte(0x00, dummy) && !Rotation::FromByte(0x03, dummy) && !Rotation::FromByte(0x0F, dummy);
        CHECK(valid == 48 && roundTrips == 48 && determinants == 48 && products == 48 * 48 && refused,
              "rotation bytes: {} valid, {} round trips, {} determinants, {} products right", valid, roundTrips, determinants, products);
        Rotation identity;
        Rotation::FromByte(4, identity); // row 0 column 0, row 1 column 1, row 2 column 2
        CHECK((identity.ToByte() == 4 && identity.Determinant() == 1 && identity.Apply(std::array<int32_t, 3>{ 1, 2, 3 }) == std::array<int32_t, 3>{ 1, 2, 3 }),
              "byte 4 is the identity (MagicaVoxel's default _r)");
    }

    // Scene graph: root transform -> group -> two transforms -> shapes; nested rotation and translation; a hidden node; a hidden layer.
    {
        Bytes body;
        ModelChunks(body, { 2, 1, 1, { { 0, 0, 0, 1 }, { 1, 0, 0, 1 } } });
        ModelChunks(body, { 1, 1, 1, { { 0, 0, 0, 2 } } });
        // 90 degrees about z: x -> y, y -> -x. Rows: row 0 = (0 -1 0): column 1, negative; row 1 = (1 0 0): column 0; row 2 column 2.
        const uint8_t rotZ = static_cast<uint8_t>(1 | (0 << 2) | 0x10);
        Transform(body, 0, 1, -1, {}, { { "_t", "10 0 0" }, { "_r", std::to_string(rotZ) } });
        Group(body, 1, { 2, 4, 6, 8 });
        Transform(body, 2, 3, 0, { { "_name", "first" } }, { { "_t", "1 2 3" } });
        Shape(body, 3, 0);
        Transform(body, 4, 5, 0, {}, { { "_t", "-4 0 1" } });
        AnimatedShape(body, 5, { { 0, 3 }, { 1, 0 }, { 0, 1 } }); // frame 0 is model 1
        Transform(body, 6, 7, 0, { { "_hidden", "1" } }, {});
        Shape(body, 7, 0);
        Transform(body, 8, 9, 1, {}, {});
        Shape(body, 9, 1);
        Layer(body, 0, false);
        Layer(body, 1, true);
        Scene scene;
        std::string error;
        const auto file = File(body);
        const bool ok = ReadVox(file.data(), file.size(), scene, error);
        CHECK(ok && scene.HasSceneGraph && scene.Instances.size() == 2, "scene graph: hidden node and hidden layer left out, an animated shape placed once ({} instances, {})",
              scene.Instances.size(), error);
        if (ok && scene.Instances.size() == 2)
        {
            const Instance& a = scene.Instances[0];
            const Instance& b = scene.Instances[1];
            // world t = R_root * t_child + t_root: R (1, 2, 3) = (-2, 1, 3), + (10, 0, 0) = (8, 1, 3).
            CHECK((a.Name == "first" && a.ModelIndex == 0 && a.Translation == std::array<int32_t, 3>{ 8, 1, 3 } && a.Rotate.ToByte() == rotZ),
                  "nested transform: translation ({} {} {}), rotation {}", a.Translation[0], a.Translation[1], a.Translation[2], a.Rotate.ToByte());
            CHECK((b.Name.empty() && b.ModelIndex == 1 && b.Translation == std::array<int32_t, 3>{ 10, -4, 1 }),
                  "second instance: ({} {} {})", b.Translation[0], b.Translation[1], b.Translation[2]);
        }
    }

    // Broken files: every truncation and 2000 random corruptions of a full file are refused or read, never crash (ASan builds check
    // the reads); some fixed cases name their problem.
    {
        Bytes body;
        ModelChunks(body, { 4, 4, 4, { { 0, 0, 0, 1 }, { 3, 3, 3, 2 }, { 1, 2, 3, 3 } } });
        Bytes rgba;
        for (int k = 0; k < 256; ++k) rgba.U32(0xff00ff00u);
        Chunk(body, "RGBA", rgba);
        Matl(body, 1, { { "_type", "_glass" }, { "_trans", "0.5" } });
        Transform(body, 0, 1, -1, {}, { { "_t", "0 0 0" } });
        Group(body, 1, { 2 });
        Transform(body, 2, 3, 0, {}, { { "_t", "1 1 1" }, { "_r", "20" } });
        Shape(body, 3, 0);
        Layer(body, 0, false);
        const std::vector<uint8_t> file = File(body);
        Scene scene;
        std::string error;
        const bool full = ReadVox(file.data(), file.size(), scene, error);
        int truncatedRefused = 0;
        for (size_t length = 0; length < file.size(); ++length)
        {
            std::vector<uint8_t> cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(length));
            truncatedRefused += !ReadVox(cut.data(), cut.size(), scene, error);
        }
        std::mt19937 rng(1234);
        int read = 0, refused = 0;
        for (int round = 0; round < 2000; ++round)
        {
            std::vector<uint8_t> broken = file;
            const int flips = 1 + static_cast<int>(rng() % 4);
            for (int f = 0; f < flips; ++f)
                broken[8 + rng() % (broken.size() - 8)] = static_cast<uint8_t>(rng());
            (ReadVox(broken.data(), broken.size(), scene, error) ? read : refused)++;
        }
        CHECK(full && truncatedRefused == static_cast<int>(file.size()), "every truncation refused ({} of {})", truncatedRefused, file.size());
        CHECK(read + refused == 2000, "2000 corrupted files: {} read, {} refused, none crashed", read, refused);

        Bytes noSize;
        Bytes xyzi; xyzi.U32(0);
        Chunk(noSize, "XYZI", xyzi);
        const auto f1 = File(noSize);
        const bool r1 = ReadVox(f1.data(), f1.size(), scene, error);
        CHECK(!r1 && error.find("SIZE") != std::string::npos, "XYZI without SIZE: '{}'", error);
        Bytes huge;
        Bytes size; size.I32(4096); size.I32(1); size.I32(1); // MagicaVoxel models are at most 256 (the reader takes up to 1024)
        Chunk(huge, "SIZE", size);
        const auto f2 = File(huge);
        const bool r2 = ReadVox(f2.data(), f2.size(), scene, error);
        CHECK(!r2 && error.find("size") != std::string::npos, "an impossible size: '{}'", error);
        Bytes cycle;
        ModelChunks(cycle, { 1, 1, 1, { { 0, 0, 0, 1 } } });
        Transform(cycle, 0, 1, -1, {}, {});
        Group(cycle, 1, { 0 });
        const auto f3 = File(cycle);
        const bool r3 = ReadVox(f3.data(), f3.size(), scene, error);
        CHECK(!r3 && error.find("deep") != std::string::npos, "a cycle in the scene graph: '{}'", error);
        const uint8_t notVox[] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0 };
        const bool r4 = ReadVox(notVox, sizeof(notVox), scene, error);
        CHECK(!r4 && error.find("VOX") != std::string::npos, "not a .vox: '{}'", error);
    }
}

static void TestVoxMesher()
{
    using namespace VoxTest;
    std::printf("\n== .vox mesher\n");
    std::array<Material, 256> plain{};

    // One voxel: 6 faces, closed (volume 1), outward, in engine axes (MagicaVoxel z is up: the top face's normal is +y).
    {
        Model m;
        m.SizeX = m.SizeY = m.SizeZ = 1;
        m.Voxels = { 3 };
        m.VoxelCount = 1;
        ModelMesh mesh;
        MeshModel(m, plain, true, glm::vec3(0.0f), mesh);
        int up = 0;
        for (size_t i = 0; i < mesh.Opaque.Normals.size(); i += 4)
            up += mesh.Opaque.Normals[i] == glm::vec3(0, 1, 0);
        CHECK(mesh.Opaque.QuadCount() == 6 && mesh.Transparent.Empty() && std::abs(SignedVolume(mesh.Opaque) - 1.0) < 1e-9 &&
              WindingMatchesNormals(mesh.Opaque) && up == 1 && mesh.Opaque.ColorIndices[0] == 3,
              "one voxel: {} quads, volume {}, one face up", mesh.Opaque.QuadCount(), SignedVolume(mesh.Opaque));
    }

    // A 4 x 3 x 2 block of one colour: greedy gives 6 rectangles; per voxel 2 (4x3 + 4x2 + 3x2) = 52 quads; same volume and area.
    {
        Model m;
        m.SizeX = 4; m.SizeY = 3; m.SizeZ = 2;
        m.Voxels.assign(24, 9);
        m.VoxelCount = 24;
        ModelMesh merged, single;
        MeshModel(m, plain, true, glm::vec3(0.0f), merged);
        MeshModel(m, plain, false, glm::vec3(0.0f), single);
        CHECK(merged.Opaque.QuadCount() == 6 && single.Opaque.QuadCount() == 52 && std::abs(SignedVolume(merged.Opaque) - 24.0) < 1e-9 &&
              std::abs(SignedVolume(single.Opaque) - 24.0) < 1e-9 && std::abs(Area(merged.Opaque) - Area(single.Opaque)) < 1e-9,
              "block: {} merged quads, {} single, volume {} / {}", merged.Opaque.QuadCount(), single.Opaque.QuadCount(), SignedVolume(merged.Opaque),
              SignedVolume(single.Opaque));
        CHECK(merged.SolidMin == glm::ivec3(0) && merged.SolidMax == glm::ivec3(3, 2, 1), "solid bounds");
    }

    // Random models, several colours: closed surface (volume = voxel count), outward, the face count the rule gives, the same area
    // merged or not, and merging never merges two colours.
    {
        std::mt19937 rng(77);
        int closed = 0, outward = 0, counts = 0, areas = 0, rounds = 30;
        for (int round = 0; round < rounds; ++round)
        {
            const Model m = RandomModel(rng, 3 + static_cast<int>(rng() % 10), 3 + static_cast<int>(rng() % 10), 3 + static_cast<int>(rng() % 10), 0.45f,
                                        1 + static_cast<int>(rng() % 4));
            ModelMesh merged, single;
            MeshModel(m, plain, true, glm::vec3(0.0f), merged);
            MeshModel(m, plain, false, glm::vec3(0.0f), single);
            size_t opaque = 0, transparent = 0;
            ExpectedFaces(m, plain, opaque, transparent);
            closed += std::abs(SignedVolume(merged.Opaque) - m.VoxelCount) < 1e-6;
            outward += WindingMatchesNormals(merged.Opaque) && WindingMatchesNormals(single.Opaque);
            counts += single.Opaque.QuadCount() == opaque && merged.Opaque.QuadCount() <= opaque;
            areas += std::abs(Area(merged.Opaque) - static_cast<double>(opaque)) < 1e-6;
        }
        CHECK(closed == rounds && outward == rounds && counts == rounds && areas == rounds,
              "{} random models: closed {}, outward {}, face counts {}, areas {}", rounds, closed, outward, counts, areas);
    }

    // Glass: an opaque voxel shows its face towards glass, glass none towards opaque or glass; the opaque surface stays closed.
    {
        std::array<Material, 256> materials{};
        materials[2].Type = MaterialType::Glass;
        materials[3].Type = MaterialType::Glass;
        std::mt19937 rng(5);
        int ok = 0, rounds = 30;
        for (int round = 0; round < rounds; ++round)
        {
            Model m = RandomModel(rng, 6, 5, 4, 0.6f, 3);
            ModelMesh mesh;
            MeshModel(m, materials, true, glm::vec3(0.0f), mesh);
            ModelMesh single;
            MeshModel(m, materials, false, glm::vec3(0.0f), single);
            size_t opaque = 0, transparent = 0;
            ExpectedFaces(m, materials, opaque, transparent);
            uint32_t opaqueVoxels = 0;
            for (uint8_t v : m.Voxels)
                opaqueVoxels += v == 1;
            bool coloursSplit = true;
            for (uint8_t c : mesh.Transparent.ColorIndices) coloursSplit &= c == 2 || c == 3;
            for (uint8_t c : mesh.Opaque.ColorIndices) coloursSplit &= c == 1;
            ok += single.Opaque.QuadCount() == opaque && single.Transparent.QuadCount() == transparent &&
                  std::abs(SignedVolume(mesh.Opaque) - opaqueVoxels) < 1e-6 && coloursSplit && WindingMatchesNormals(mesh.Transparent);
        }
        Model pair;
        pair.SizeX = 2; pair.SizeY = 1; pair.SizeZ = 1;
        pair.Voxels = { 1, 2 };
        pair.VoxelCount = 2;
        ModelMesh mesh;
        MeshModel(pair, materials, true, glm::vec3(0.0f), mesh);
        CHECK(ok == rounds && mesh.Opaque.QuadCount() == 6 && mesh.Transparent.QuadCount() == 5,
              "glass rule on {} random models ({} right); opaque + glass pair: {} opaque faces, {} glass", rounds, ok, mesh.Opaque.QuadCount(),
              mesh.Transparent.QuadCount());
    }

    // Placement: every voxel's centre through the mesh + node transform lands where MagicaVoxel puts it (R (v + 0.5 - size / 2) + t,
    // engine axes), for every rotation byte, odd and even sizes; File Origin keeps the scene where it is.
    {
        int right = 0, total = 0;
        for (int byte = 0; byte < 128; ++byte)
        {
            Rotation rotation;
            if (!Rotation::FromByte(static_cast<uint8_t>(byte), rotation))
                continue;
            for (Pivot origin : { Pivot::FileOrigin, Pivot::BottomCenter, Pivot::Center })
            {
                std::mt19937 rng(static_cast<uint32_t>(byte * 3 + static_cast<int>(origin)));
                Scene scene;
                scene.Models.push_back(RandomModel(rng, 3 + byte % 4, 2 + byte % 3, 5, 0.3f, 2));
                scene.Models[0].Voxels[0] = 1; // never empty
                scene.Models[0].VoxelCount = 0;
                for (uint8_t v : scene.Models[0].Voxels) scene.Models[0].VoxelCount += v != 0;
                Instance& instance = scene.Instances.emplace_back();
                instance.Rotate = rotation;
                instance.Translation = { 5, -3, 7 };
                const MeshedScene meshed = MeshScene(scene, MeshOptions{ origin, true });
                if (meshed.Nodes.size() != 1)
                    continue;
                const MeshedNode& node = meshed.Nodes[0];
                const ModelMesh& mesh = meshed.Models[0];
                const Model& m = scene.Models[0];
                // The mesh's local pivot (engine axes) from any vertex: vertices are cell corners minus the pivot.
                glm::vec3 low(1e9f);
                for (const glm::vec3& p : mesh.Opaque.Positions) low = glm::min(low, p);
                const glm::vec3 solidLow = glm::min(ToEngineAxes(glm::vec3(mesh.SolidMin)), ToEngineAxes(glm::vec3(mesh.SolidMax) + glm::vec3(1.0f)));
                const glm::vec3 pivotEngine = solidLow - low;
                bool same = true;
                const glm::vec3 half(m.SizeX / 2, m.SizeY / 2, m.SizeZ / 2);
                for (int z = 0; z < m.SizeZ; ++z)
                    for (int y = 0; y < m.SizeY; ++y)
                        for (int x = 0; x < m.SizeX; ++x)
                        {
                            if (!m.At(x, y, z))
                                continue;
                            const glm::vec3 centre = glm::vec3(x, y, z) + glm::vec3(0.5f);
                            const std::array<double, 3> r = rotation.Apply(std::array<double, 3>{ centre.x - half.x, centre.y - half.y, centre.z - half.z });
                            const glm::vec3 expected = ToEngineAxes(glm::vec3(static_cast<float>(r[0]) + 5.0f, static_cast<float>(r[1]) - 3.0f,
                                                                              static_cast<float>(r[2]) + 7.0f)) + meshed.RootTranslation;
                            const glm::vec3 local = ToEngineAxes(centre) - pivotEngine;
                            const glm::vec3 placed = meshed.RootTranslation + node.Translation + node.Rotation * (node.Scale * local); // root, node, mesh
                            same &= glm::length(placed - expected) < 1e-4f;
                        }
                right += same;
                ++total;
                if (origin == Pivot::BottomCenter)
                {
                    // Bottom centre: the placed scene stands on y = 0, centred on x / z.
                    right += std::abs(meshed.BoundsMin.y) < 1e-4f && std::abs(meshed.BoundsMin.x + meshed.BoundsMax.x) < 1e-4f &&
                             std::abs(meshed.BoundsMin.z + meshed.BoundsMax.z) < 1e-4f;
                    ++total;
                }
                if (origin == Pivot::FileOrigin)
                {
                    right += meshed.RootTranslation == glm::vec3(0.0f);
                    ++total;
                }
            }
        }
        CHECK(right == total && total == 48 * 5, "placements through the node transforms match MagicaVoxel's for 48 rotations x 3 pivots ({} of {})", right, total);
    }

    // Empty models and their instances are left out; a mirrored placement gets a negative scale and a unit quaternion.
    {
        Scene scene;
        Model empty;
        empty.SizeX = empty.SizeY = empty.SizeZ = 2;
        empty.Voxels.assign(8, 0);
        scene.Models.push_back(empty);
        Model one;
        one.SizeX = one.SizeY = one.SizeZ = 1;
        one.Voxels = { 1 };
        one.VoxelCount = 1;
        scene.Models.push_back(one);
        scene.Instances.push_back({ "empty", 0, Rotation{}, { 0, 0, 0 }, -1 });
        Rotation mirror;
        Rotation::FromByte(static_cast<uint8_t>(4 | 0x10), mirror); // x negated
        scene.Instances.push_back({ "mirrored", 1, mirror, { 0, 0, 0 }, -1 });
        const MeshedScene meshed = MeshScene(scene, {});
        CHECK(meshed.Nodes.size() == 1 && meshed.Nodes[0].Name == "mirrored" && meshed.Nodes[0].Scale == glm::vec3(-1, 1, 1) &&
              std::abs(glm::length(meshed.Nodes[0].Rotation) - 1.0f) < 1e-6f && !meshed.Models[0].HasVoxels,
              "empty model skipped; mirror as scale ({} {} {})", meshed.Nodes.empty() ? 0.f : meshed.Nodes[0].Scale.x,
              meshed.Nodes.empty() ? 0.f : meshed.Nodes[0].Scale.y, meshed.Nodes.empty() ? 0.f : meshed.Nodes[0].Scale.z);
    }
}
