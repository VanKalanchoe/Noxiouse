#include "VoxelGenerator.h"
#include "VoxelMaterialTable.h"

#include <FastNoise/FastNoise.h>
#include <xxhash.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Nox::Voxel
{
    namespace
    {
        FastSIMD::FeatureSet ToFeatureSet(SimdLevel level)
        {
            switch (level)
            {
            case SimdLevel::Scalar: return FastSIMD::FeatureSet::SCALAR;
            case SimdLevel::SSE2: return FastSIMD::FeatureSet::SSE2;
            case SimdLevel::SSE41: return FastSIMD::FeatureSet::SSE41;
            case SimdLevel::AVX2: return FastSIMD::FeatureSet::AVX2;
            case SimdLevel::AVX512: return FastSIMD::FeatureSet::AVX512;
            case SimdLevel::Best: break;
            }
            return FastSIMD::FeatureSet::Max;
        }

        // Seeds of the layers: the world seed and a fixed offset each (the same tree in two layers gives different noise).
        constexpr uint32_t HeightSeedOffset = 0;
        constexpr uint32_t CaveSeedOffset = 0x1b873593;
        constexpr uint32_t OreSeedOffset = 0x68e31da4;
        int32_t LayerSeed(int32_t seed, uint32_t offset) { return static_cast<int32_t>(static_cast<uint32_t>(seed) + offset); } // (wraps)

        // Binary hashing: fields appended little-endian, floats as their bits.
        struct HashBuffer
        {
            std::vector<uint8_t> Bytes;
            void U32(uint32_t v) { for (int i = 0; i < 4; ++i) Bytes.push_back(uint8_t(v >> (8 * i))); }
            void F32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); U32(bits); }
            void Str(const std::string& s) { U32(static_cast<uint32_t>(s.size())); Bytes.insert(Bytes.end(), s.begin(), s.end()); }
        };

        // A height (a position): the voxel it falls in. A depth (a length): the nearest whole number of voxels -- 3 m / 0.1 m is
        // 29.99999... in floating point, and flooring it made a 3 m layer 29 voxels deep.
        int32_t HeightToVoxels(float metres, float voxelSize) { return static_cast<int32_t>(std::floor(static_cast<double>(metres) / voxelSize)); }
        int32_t DepthToVoxels(float metres, float voxelSize) { return static_cast<int32_t>(std::lround(static_cast<double>(metres) / voxelSize)); }
    }

    struct VoxelGenerator::Nodes
    {
        FastNoise::SmartNode<> Height;
        FastNoise::SmartNode<> Cave;
        std::vector<FastNoise::SmartNode<>> Ores;
    };

    uint64_t VoxelGeneratorSettings::Hash() const
    {
        HashBuffer h;
        h.U32(Version);
        h.Str(HeightTree);
        h.F32(BaseHeight);
        h.F32(HeightAmplitude);
        h.Str(CaveTree);
        h.F32(CaveThreshold);
        h.F32(CaveTopDepth);
        h.U32(SurfaceMaterial);
        h.U32(SubsurfaceMaterial);
        h.F32(SubsurfaceDepth);
        h.U32(BulkMaterial);
        h.U32(static_cast<uint32_t>(Ores.size()));
        for (const VoxelOreLayer& ore : Ores)
        {
            h.Str(ore.Name);
            h.U32(ore.Material);
            h.Str(ore.NoiseTree);
            h.F32(ore.Threshold);
            h.F32(ore.MinHeight);
            h.F32(ore.MaxHeight);
        }
        return XXH3_64bits(h.Bytes.data(), h.Bytes.size());
    }

    std::string VoxelGeneratorSettings::Serialize() const
    {
        YAML::Emitter out;
        out.SetFloatPrecision(9);
        out << YAML::BeginMap;
        out << YAML::Key << "VoxelGenerator" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Version" << YAML::Value << Version;
        out << YAML::Key << "Public" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "HeightTree" << YAML::Value << HeightTree;
        out << YAML::Key << "BaseHeight" << YAML::Value << BaseHeight;
        out << YAML::Key << "HeightAmplitude" << YAML::Value << HeightAmplitude;
        out << YAML::Key << "CaveTree" << YAML::Value << CaveTree;
        out << YAML::Key << "CaveThreshold" << YAML::Value << CaveThreshold;
        out << YAML::Key << "CaveTopDepth" << YAML::Value << CaveTopDepth;
        out << YAML::Key << "SurfaceMaterial" << YAML::Value << SurfaceMaterial;
        out << YAML::Key << "SubsurfaceMaterial" << YAML::Value << SubsurfaceMaterial;
        out << YAML::Key << "SubsurfaceDepth" << YAML::Value << SubsurfaceDepth;
        out << YAML::Key << "BulkMaterial" << YAML::Value << BulkMaterial;
        out << YAML::EndMap;
        out << YAML::Key << "Secret" << YAML::Value << YAML::BeginSeq;
        for (const VoxelOreLayer& ore : Ores)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << ore.Name;
            out << YAML::Key << "Material" << YAML::Value << ore.Material;
            out << YAML::Key << "NoiseTree" << YAML::Value << ore.NoiseTree;
            out << YAML::Key << "Threshold" << YAML::Value << ore.Threshold;
            out << YAML::Key << "MinHeight" << YAML::Value << ore.MinHeight;
            out << YAML::Key << "MaxHeight" << YAML::Value << ore.MaxHeight;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
        out << YAML::EndMap << YAML::EndMap;
        return out.c_str();
    }

    bool VoxelGeneratorSettings::Deserialize(const std::string& text, VoxelGeneratorSettings& out, std::string& error)
    {
        try
        {
            const YAML::Node root = YAML::Load(text)["VoxelGenerator"];
            if (!root || !root["Public"])
            {
                error = "no VoxelGenerator with a Public layer";
                return false;
            }
            VoxelGeneratorSettings s;
            s.Version = root["Version"].as<uint32_t>(1);
            const YAML::Node p = root["Public"];
            s.HeightTree = p["HeightTree"].as<std::string>(DefaultHeightTree);
            s.BaseHeight = p["BaseHeight"].as<float>(0.0f);
            s.HeightAmplitude = p["HeightAmplitude"].as<float>(24.0f);
            s.CaveTree = p["CaveTree"].as<std::string>("");
            s.CaveThreshold = p["CaveThreshold"].as<float>(0.65f);
            s.CaveTopDepth = p["CaveTopDepth"].as<float>(4.0f);
            s.SurfaceMaterial = p["SurfaceMaterial"].as<MaterialId>(Air);
            s.SubsurfaceMaterial = p["SubsurfaceMaterial"].as<MaterialId>(Air);
            s.SubsurfaceDepth = p["SubsurfaceDepth"].as<float>(3.0f);
            s.BulkMaterial = p["BulkMaterial"].as<MaterialId>(Air);
            if (const YAML::Node secret = root["Secret"])
                for (const YAML::Node& node : secret)
                {
                    VoxelOreLayer ore;
                    ore.Name = node["Name"].as<std::string>("");
                    ore.Material = node["Material"].as<MaterialId>(Air);
                    ore.NoiseTree = node["NoiseTree"].as<std::string>(DefaultOreTree);
                    ore.Threshold = node["Threshold"].as<float>(0.75f);
                    ore.MinHeight = node["MinHeight"].as<float>(-512.0f);
                    ore.MaxHeight = node["MaxHeight"].as<float>(0.0f);
                    s.Ores.push_back(std::move(ore));
                }
            out = std::move(s);
            return true;
        }
        catch (const YAML::Exception& e)
        {
            error = e.what();
            return false;
        }
    }

    VoxelGeneratorSettings MakeDefaultTerrain(VoxelMaterialTable& table)
    {
        auto material = [&](const char* name, glm::vec3 colour, float roughness, float metallic, float hardness, const char* surface)
        {
            if (const MaterialId found = table.Find(name); found != Air)
                return found;
            VoxelMaterial m;
            m.Name = name;
            m.BaseColor = glm::vec4(colour, 1.0f);
            m.Roughness = roughness;
            m.Metallic = metallic;
            m.Hardness = hardness;
            m.FootstepSurface = surface;
            return table.Add(m);
        };
        VoxelGeneratorSettings settings;
        settings.SurfaceMaterial = material("Grass", { 0.12f, 0.32f, 0.06f }, 0.9f, 0.0f, 0.6f, "Grass");
        settings.SubsurfaceMaterial = material("Dirt", { 0.22f, 0.13f, 0.06f }, 0.95f, 0.0f, 0.5f, "Dirt");
        settings.BulkMaterial = material("Stone", { 0.30f, 0.30f, 0.31f }, 0.85f, 0.0f, 2.0f, "Stone");
        VoxelOreLayer iron;
        iron.Name = "Iron";
        iron.Material = material("Iron Ore", { 0.45f, 0.30f, 0.22f }, 0.6f, 0.4f, 3.0f, "Stone");
        iron.MinHeight = -512.0f;
        iron.MaxHeight = -8.0f;
        settings.Ores.push_back(iron);
        return settings;
    }

    VoxelGenerator::VoxelGenerator(const VoxelGeneratorSettings& settings, float voxelSize, int32_t seed, SimdLevel simd)
        : m_Settings(settings), m_VoxelSize(voxelSize), m_Seed(seed), m_Nodes(std::make_unique<Nodes>())
    {
        const FastSIMD::FeatureSet level = ToFeatureSet(simd);
        auto load = [&](const std::string& tree, const char* what) -> FastNoise::SmartNode<>
        {
            FastNoise::SmartNode<> node = FastNoise::NewFromEncodedNodeTree(tree.c_str(), level);
            if (!node && m_Error.empty())
            {
                if (level != FastSIMD::FeatureSet::Max && FastNoise::NewFromEncodedNodeTree(tree.c_str()))
                    m_Error = std::string(what) + ": the forced CPU instruction set is not built (x86: SSE2, SSE4.1, AVX2, AVX-512)";
                else
                    m_Error = std::string(what) + ": its node tree cannot be read (\"" + tree + "\")";
            }
            return node;
        };
        if (!(voxelSize > 0.0f))
            m_Error = "the voxel size must be above 0";
        m_Nodes->Height = load(settings.HeightTree, "height");
        if (!settings.CaveTree.empty())
            m_Nodes->Cave = load(settings.CaveTree, "caves");
        for (const VoxelOreLayer& ore : settings.Ores)
        {
            m_Nodes->Ores.push_back(load(ore.NoiseTree, ("ore '" + ore.Name + "'").c_str()));
            if (ore.Material == Air && m_Error.empty())
                m_Error = "ore '" + ore.Name + "': no material";
        }
        if (m_Error.empty() && (settings.SurfaceMaterial == Air || settings.SubsurfaceMaterial == Air || settings.BulkMaterial == Air))
            m_Error = "the surface, subsurface and bulk materials must not be air";
    }

    VoxelGenerator::~VoxelGenerator() = default;

    ColumnHeights VoxelGenerator::Column(int32_t chunkX, int32_t chunkZ) const
    {
        ColumnHeights column;
        column.Heights.assign(ChunkSize * ChunkSize, 0);
        if (!IsValid())
            return column;
        std::vector<float> noise(ChunkSize * ChunkSize);
        // Voxel centres in metres: x = (voxel + 0.5) * size. (The 2D grid's second axis is the world's z.)
        const float x0 = static_cast<float>((static_cast<double>(chunkX) * ChunkSize + 0.5) * m_VoxelSize);
        const float z0 = static_cast<float>((static_cast<double>(chunkZ) * ChunkSize + 0.5) * m_VoxelSize);
        m_Nodes->Height->GenUniformGrid2D(noise.data(), x0, z0, ChunkSize, ChunkSize, m_VoxelSize, m_VoxelSize, LayerSeed(m_Seed, HeightSeedOffset));
        column.MinSurface = std::numeric_limits<int32_t>::max();
        column.MaxSurface = std::numeric_limits<int32_t>::min();
        for (size_t i = 0; i < noise.size(); ++i)
        {
            const double metres = static_cast<double>(m_Settings.BaseHeight) + static_cast<double>(m_Settings.HeightAmplitude) * noise[i];
            const int32_t height = static_cast<int32_t>(std::floor(metres / m_VoxelSize));
            column.Heights[i] = height;
            column.MinSurface = std::min(column.MinSurface, height);
            column.MaxSurface = std::max(column.MaxSurface, height);
        }
        return column;
    }

    void VoxelGenerator::Generate(const glm::ivec3& chunk, bool secretLayer, VoxelChunk& out) const
    {
        Generate(chunk, Column(chunk.x, chunk.z), secretLayer, out);
    }

    void VoxelGenerator::Generate(const glm::ivec3& chunk, const ColumnHeights& column, bool secretLayer, VoxelChunk& out) const
    {
        if (!IsValid())
        {
            out.Fill(Air);
            return;
        }
        const int32_t y0 = chunk.y * ChunkSize;
        const int32_t y1 = y0 + ChunkSize - 1;
        // Above the column's highest surface: air.
        if (y0 > column.MaxSurface)
        {
            out.Fill(Air);
            return;
        }
        const int32_t subsurfaceVoxels = std::max(1, DepthToVoxels(m_Settings.SubsurfaceDepth, m_VoxelSize));
        const int32_t caveTopVoxels = std::max(0, DepthToVoxels(m_Settings.CaveTopDepth, m_VoxelSize));
        const bool caves = m_Nodes->Cave && y0 <= column.MaxSurface - caveTopVoxels;
        std::vector<size_t> ores;
        if (secretLayer)
            for (size_t i = 0; i < m_Settings.Ores.size(); ++i)
            {
                const int32_t low = HeightToVoxels(m_Settings.Ores[i].MinHeight, m_VoxelSize);
                const int32_t high = HeightToVoxels(m_Settings.Ores[i].MaxHeight, m_VoxelSize);
                if (high >= y0 && low <= y1)
                    ores.push_back(i);
            }
        // Wholly bulk: below every subsurface voxel, nothing carved, no ore.
        if (y1 <= column.MinSurface - subsurfaceVoxels && !caves && ores.empty())
        {
            out.Fill(m_Settings.BulkMaterial);
            return;
        }

        thread_local std::vector<MaterialId> voxels;
        thread_local std::vector<float> noise;
        voxels.resize(ChunkVoxels);
        for (int32_t z = 0; z < ChunkSize; ++z)
            for (int32_t x = 0; x < ChunkSize; ++x)
            {
                const int32_t height = column.Heights[x + z * ChunkSize];
                for (int32_t y = 0; y < ChunkSize; ++y)
                {
                    const int32_t worldY = y0 + y;
                    MaterialId material = Air;
                    if (worldY == height)
                        material = m_Settings.SurfaceMaterial;
                    else if (worldY < height)
                        material = worldY > height - subsurfaceVoxels ? m_Settings.SubsurfaceMaterial : m_Settings.BulkMaterial;
                    voxels[VoxelInChunk(x, y, z)] = material;
                }
            }

        // 3D noise over the chunk's voxel centres (x fastest, then y, then z: VoxelInChunk's order).
        const float x0 = static_cast<float>((static_cast<double>(chunk.x) * ChunkSize + 0.5) * m_VoxelSize);
        const float yo = static_cast<float>((static_cast<double>(y0) + 0.5) * m_VoxelSize);
        const float z0 = static_cast<float>((static_cast<double>(chunk.z) * ChunkSize + 0.5) * m_VoxelSize);
        auto grid = [&](const FastNoise::SmartNode<>& node, int32_t seed)
        {
            noise.resize(ChunkVoxels);
            node->GenUniformGrid3D(noise.data(), x0, yo, z0, ChunkSize, ChunkSize, ChunkSize, m_VoxelSize, m_VoxelSize, m_VoxelSize, seed);
        };
        if (caves)
        {
            grid(m_Nodes->Cave, LayerSeed(m_Seed, CaveSeedOffset));
            for (int32_t z = 0; z < ChunkSize; ++z)
                for (int32_t x = 0; x < ChunkSize; ++x)
                {
                    const int32_t limit = column.Heights[x + z * ChunkSize] - caveTopVoxels;
                    for (int32_t y = 0; y < ChunkSize && y0 + y <= limit; ++y)
                    {
                        const int32_t i = VoxelInChunk(x, y, z);
                        if (noise[i] > m_Settings.CaveThreshold)
                            voxels[i] = Air;
                    }
                }
        }
        for (size_t oreIndex : ores)
        {
            const VoxelOreLayer& ore = m_Settings.Ores[oreIndex];
            const int32_t low = HeightToVoxels(ore.MinHeight, m_VoxelSize);
            const int32_t high = HeightToVoxels(ore.MaxHeight, m_VoxelSize);
            grid(m_Nodes->Ores[oreIndex], LayerSeed(m_Seed, OreSeedOffset + static_cast<uint32_t>(oreIndex)));
            for (int32_t z = 0; z < ChunkSize; ++z)
                for (int32_t y = std::max(0, low - y0); y <= std::min(ChunkSize - 1, high - y0); ++y)
                    for (int32_t x = 0; x < ChunkSize; ++x)
                    {
                        const int32_t i = VoxelInChunk(x, y, z);
                        if (voxels[i] == m_Settings.BulkMaterial && noise[i] > ore.Threshold)
                            voxels[i] = ore.Material;
                    }
        }
        out.Assign(voxels);
    }
}
