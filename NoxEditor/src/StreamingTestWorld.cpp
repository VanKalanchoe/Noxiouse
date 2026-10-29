#include "StreamingTestWorld.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/MeshImporter.h"
#include "NoxCore/Asset/MeshSerializer.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneSerializer.h"
#include "NoxCore/Scene/WorldPartition.h"

namespace Nox
{
    namespace
    {
        // --- The terrain: one height function over the whole world (meters), so the tiles meet without seams ---

        float Hash(int32_t x, int32_t z)
        {
            uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(z) * 0xD8163841u;
            h = (h ^ (h >> 13)) * 0x85EBCA6Bu;
            h ^= h >> 16;
            return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF); // 0..1
        }

        // Value noise, smoothly interpolated: 0..1.
        float Noise(float x, float z)
        {
            const float fx = std::floor(x), fz = std::floor(z);
            const int32_t ix = static_cast<int32_t>(fx), iz = static_cast<int32_t>(fz);
            float tx = x - fx, tz = z - fz;
            tx = tx * tx * (3.0f - 2.0f * tx);
            tz = tz * tz * (3.0f - 2.0f * tz);
            const float a = Hash(ix, iz), b = Hash(ix + 1, iz), c = Hash(ix, iz + 1), d = Hash(ix + 1, iz + 1);
            return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
        }

        // Rolling hills with ridges, 0..~45 m.
        float HeightMeters(float x, float z)
        {
            float height = 0.0f, amplitude = 26.0f, frequency = 1.0f / 420.0f;
            for (int octave = 0; octave < 6; ++octave)
            {
                const float n = Noise(x * frequency + 17.3f * octave, z * frequency - 9.1f * octave);
                height += (octave == 1 ? 1.0f - std::abs(n * 2.0f - 1.0f) : n) * amplitude; // the second octave: ridges
                amplitude *= 0.47f;
                frequency *= 2.03f;
            }
            return height;
        }

        glm::vec3 Mix(const glm::vec3& a, const glm::vec3& b, float t) { return a + (b - a) * std::clamp(t, 0.0f, 1.0f); }

        // The ground's colour (sRGB, 0..1) from height, slope (0 flat .. 1 vertical) and fine detail.
        glm::vec3 GroundColor(float heightMeters, float slope, float detail)
        {
            const glm::vec3 grass(0.22f, 0.33f, 0.13f), dry(0.45f, 0.42f, 0.24f), rock(0.40f, 0.38f, 0.35f), snow(0.88f, 0.89f, 0.92f);
            glm::vec3 color = Mix(grass, dry, (heightMeters - 18.0f) / 14.0f + (detail - 0.5f) * 0.6f);
            color = Mix(color, rock, (slope - 0.35f) / 0.2f);
            color = Mix(color, snow, (heightMeters - 38.0f) / 4.0f - slope);
            return color * (0.82f + 0.36f * detail);
        }

        std::filesystem::path TileName(uint32_t x, uint32_t z) { return "Tile_" + std::to_string(x) + "_" + std::to_string(z); }
        std::filesystem::path MeshPath(const std::string& folder, uint32_t x, uint32_t z) { return std::filesystem::path(folder) / "Meshes" / (TileName(x, z).string() + ".nsmesh"); }
        std::filesystem::path TexturePath(const std::string& folder, uint32_t x, uint32_t z) { return std::filesystem::path(folder) / "Textures" / (TileName(x, z).string() + ".ntex"); }

        // One tile: its height field mesh (cooked, cluster LOD) and its texture (BC1), at assetDirectory / their paths. Returns the
        // bytes written; its height range (world units) in min / max.
        uint64_t WriteTile(const std::filesystem::path& assetDirectory, const StreamingTestWorldGenerator::Settings& settings, uint32_t tileX,
                           uint32_t tileZ, float& outMin, float& outMax, std::string& outError)
        {
            const uint32_t quads = std::max(settings.TileQuads, 2u);
            const uint32_t side = quads + 1;
            const float tileMeters = settings.CellSizeMeters;
            const float stepMeters = tileMeters / static_cast<float>(quads);
            const float originX = tileX * tileMeters, originZ = tileZ * tileMeters;

            // Heights on the vertex grid, one ring wider for the normals.
            std::vector<float> heights(static_cast<size_t>(side + 2) * (side + 2));
            auto heightAt = [&](int32_t i, int32_t j) -> float& { return heights[static_cast<size_t>(j + 1) * (side + 2) + (i + 1)]; };
            for (int32_t j = -1; j <= static_cast<int32_t>(side); ++j)
                for (int32_t i = -1; i <= static_cast<int32_t>(side); ++i)
                    heightAt(i, j) = HeightMeters(originX + i * stepMeters, originZ + j * stepMeters);

            // The mesh, in the tile's space (the entity stands at the tile's corner), world units.
            MeshData mesh;
            mesh.Name = TileName(tileX, tileZ).string();
            mesh.Vertices.resize(static_cast<size_t>(side) * side);
            outMin = FLT_MAX;
            outMax = -FLT_MAX;
            for (uint32_t j = 0; j < side; ++j)
            {
                for (uint32_t i = 0; i < side; ++i)
                {
                    shaderio::Vertex& vertex = mesh.Vertices[static_cast<size_t>(j) * side + i];
                    const float h = heightAt(i, j);
                    vertex.pos = glm::vec3(WorldUnits::FromMeters(i * stepMeters), WorldUnits::FromMeters(h), WorldUnits::FromMeters(j * stepMeters));
                    vertex.normal = glm::normalize(glm::vec3(heightAt(i - 1, j) - heightAt(i + 1, j), 2.0f * stepMeters, heightAt(i, j - 1) - heightAt(i, j + 1)));
                    vertex.uv0 = vertex.uv1 = glm::vec2(static_cast<float>(i) / quads, static_cast<float>(j) / quads);
                    vertex.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                    outMin = std::min(outMin, vertex.pos.y);
                    outMax = std::max(outMax, vertex.pos.y);
                }
            }
            std::vector<uint32_t> indices;
            indices.reserve(static_cast<size_t>(quads) * quads * 6);
            for (uint32_t j = 0; j < quads; ++j)
            {
                for (uint32_t i = 0; i < quads; ++i)
                {
                    const uint32_t v00 = j * side + i, v10 = v00 + 1, v01 = v00 + side, v11 = v01 + 1;
                    indices.insert(indices.end(), { v00, v01, v10, v10, v01, v11 }); // counter-clockwise seen from above
                }
            }
            MeshImporter::BuildCookedGeometry(mesh, std::move(indices), true);

            MaterialData material;
            material.Name = mesh.Name;
            material.BaseColorTexturePath = TexturePath(settings.Folder, tileX, tileZ).generic_string();
            material.RoughnessFactor = 0.92f;
            material.MetallicFactor = 0.0f;

            const std::filesystem::path meshFile = assetDirectory / MeshPath(settings.Folder, tileX, tileZ);
            std::error_code error;
            std::filesystem::create_directories(meshFile.parent_path(), error);
            MeshSerializer::SerializeStaticMesh(meshFile, { mesh }, { material });

            // The texture: height and slope from the vertex grid, fine detail per texel (every tile's texels are its own).
            const uint32_t size = std::max(settings.TextureSize, 4u);
            TextureData texture;
            texture.Width = texture.Height = size;
            texture.Format = NRI::ImageFormat::SRGBA8;
            texture.Data.Allocate(static_cast<uint64_t>(size) * size * 4);
            uint8_t* texels = texture.Data.Data;
            JobSystem::Get().ParallelFor("Streaming Test Texture", size, 16, [&](uint32_t, uint32_t begin, uint32_t end)
            {
                for (uint32_t y = begin; y < end; ++y)
                {
                    for (uint32_t x = 0; x < size; ++x)
                    {
                        // Texel centre in vertex grid units.
                        const float gx = (x + 0.5f) / size * quads, gz = (y + 0.5f) / size * quads;
                        const int32_t i = std::min(static_cast<int32_t>(gx), static_cast<int32_t>(quads) - 1);
                        const int32_t j = std::min(static_cast<int32_t>(gz), static_cast<int32_t>(quads) - 1);
                        const float fx = gx - i, fz = gz - j;
                        const float h = (heightAt(i, j) * (1 - fx) + heightAt(i + 1, j) * fx) * (1 - fz) +
                                        (heightAt(i, j + 1) * (1 - fx) + heightAt(i + 1, j + 1) * fx) * fz;
                        const float dx = (heightAt(i + 1, j) - heightAt(i, j)) / stepMeters;
                        const float dz = (heightAt(i, j + 1) - heightAt(i, j)) / stepMeters;
                        const float slope = 1.0f - 1.0f / std::sqrt(1.0f + dx * dx + dz * dz);
                        const float worldX = originX + gx * stepMeters, worldZ = originZ + gz * stepMeters;
                        const float detail = 0.6f * Noise(worldX * 2.1f, worldZ * 2.1f) + 0.4f * Noise(worldX * 9.7f, worldZ * 9.7f);
                        const glm::vec3 color = GroundColor(h, slope, detail);
                        uint8_t* texel = texels + (static_cast<size_t>(y) * size + x) * 4;
                        texel[0] = static_cast<uint8_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f + 0.5f);
                        texel[1] = static_cast<uint8_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f + 0.5f);
                        texel[2] = static_cast<uint8_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f + 0.5f);
                        texel[3] = 255;
                    }
                }
            });
            const std::filesystem::path textureFile = assetDirectory / TexturePath(settings.Folder, tileX, tileZ);
            if (!TextureImporter::CookGeneratedTexture(texture, textureFile, true))
            {
                outError = "could not write " + textureFile.generic_string();
                return 0;
            }
            return std::filesystem::file_size(meshFile, error) + std::filesystem::file_size(textureFile, error);
        }
    }

    StreamingTestWorldGenerator::StreamingTestWorldGenerator(const Settings& settings)
        : m_Settings(settings), m_AssetDirectory(Project::GetActiveAssetDirectory()), m_Error(std::make_shared<std::string>())
    {
        NOX_CORE_INFO("Streaming test world: {:.1f} GB of {} m tiles into '{}'", m_Settings.TargetGigabytes, m_Settings.CellSizeMeters, m_Settings.Folder);
        m_Job = JobSystem::Get().Async("Generate Streaming Test World", JobPriority::Normal, [this](const CancellationToken& token)
        {
            // The first tile tells the size of one; the grid is the target over that.
            float minHeight, maxHeight;
            const uint64_t first = WriteTile(m_AssetDirectory, m_Settings, 0, 0, minHeight, maxHeight, *m_Error);
            if (first == 0)
                return;
            const double target = static_cast<double>(m_Settings.TargetGigabytes) * 1024.0 * 1024.0 * 1024.0;
            const uint32_t grid = std::clamp(static_cast<uint32_t>(std::sqrt(target / static_cast<double>(first))), 2u, 512u);
            m_Tiles.assign(static_cast<size_t>(grid) * grid, Tile{});
            m_Tiles[0] = { minHeight, maxHeight, true };
            m_Bytes = first;
            m_Done = 1;
            m_Grid = grid;
            for (uint32_t index = 1; index < grid * grid; ++index)
            {
                if (token.IsCancelled())
                    return;
                Tile& tile = m_Tiles[index];
                const uint64_t bytes = WriteTile(m_AssetDirectory, m_Settings, index % grid, index / grid, tile.MinHeight, tile.MaxHeight, *m_Error);
                if (bytes == 0)
                    return;
                tile.Written = true;
                m_Bytes += bytes;
                ++m_Done;
            }
        });
    }

    StreamingTestWorldGenerator::~StreamingTestWorldGenerator()
    {
        m_Job.Cancel();
        m_Job.Wait();
    }

    float StreamingTestWorldGenerator::GetProgress() const
    {
        const uint32_t grid = m_Grid;
        return grid == 0 ? 0.0f : static_cast<float>(m_Done) / static_cast<float>(grid * grid);
    }

    std::string StreamingTestWorldGenerator::GetStage() const
    {
        const uint32_t grid = m_Grid;
        if (grid == 0)
            return "Generating the first tile";
        return "Tile " + std::to_string(m_Done.load()) + " / " + std::to_string(grid * grid) + " (" + std::to_string(m_Bytes.load() >> 20) + " MB)";
    }

    bool StreamingTestWorldGenerator::Step()
    {
        if (m_Finished)
            return true;
        if (!m_Job.IsReady())
            return false;
        m_Finished = true;
        const uint32_t grid = m_Grid;
        if (!m_Error->empty() || grid == 0 || m_Done != grid * grid)
        {
            NOX_CORE_ERROR("Streaming test world: not generated ({})", m_Error->empty() ? std::string("stopped") : *m_Error);
            m_Failed = true;
            return true;
        }
        Finish();
        return true;
    }

    void StreamingTestWorldGenerator::Finish()
    {
        // Main thread: the tiles registered as assets (a new one, or the one already at that path: generated again), their cells
        // written with the index, then the scene.
        EditorAssetManager& assets = *Project::GetActive()->GetEditorAssetManager();
        auto registered = [&](AssetType type, const std::filesystem::path& path) { return assets.RegisterGeneratedFile(path, type); };

        const uint32_t grid = m_Grid;
        const float cellSize = WorldUnits::FromMeters(m_Settings.CellSizeMeters);
        const std::filesystem::path scenePath = std::filesystem::path(m_Settings.Folder) / "StreamingTestWorld.nox";
        Ref<Scene> world = CreateRef<Scene>();
        WorldPartition& partition = world->GetWorldPartition();
        partition.Settings.Enabled = true;
        partition.Settings.CellSize = m_Settings.CellSizeMeters;
        partition.Settings.LoadingRange = m_Settings.LoadingRangeMeters;
        partition.CellDirectory = std::filesystem::path(m_Settings.Folder) / "StreamingTestWorld_Cells";
        std::error_code error;
        std::filesystem::create_directories(m_AssetDirectory / partition.CellDirectory, error);

        // The tiles, one entity and one cell each (written as a save writes a cell).
        Ref<Scene> tiles = CreateRef<Scene>();
        for (uint32_t z = 0; z < grid; ++z)
        {
            for (uint32_t x = 0; x < grid; ++x)
            {
                const Tile& tile = m_Tiles[static_cast<size_t>(z) * grid + x];
                registered(AssetType::Texture2D, TexturePath(m_Settings.Folder, x, z));
                const AssetHandle mesh = registered(AssetType::StaticMesh, MeshPath(m_Settings.Folder, x, z));
                Entity entity = tiles->CreateEntity(TileName(x, z).string());
                entity.GetComponent<TransformComponent>().Translation = glm::vec3(x * cellSize, 0.0f, z * cellSize);
                entity.AddComponent<MeshComponent>(mesh);

                const glm::vec3 boundsMin(x * cellSize, tile.MinHeight, z * cellSize);
                const glm::vec3 boundsMax((x + 1) * cellSize, tile.MaxHeight, (z + 1) * cellSize);
                const WorldCellCoord cell = WorldGrid::CellFor(boundsMin, boundsMax, cellSize);
                uint32_t count = 0;
                const std::string text = SceneSerializer::WorldCellText(*tiles, { entity }, WorldGrid::CellName(cell), &count);
                std::ofstream out(m_AssetDirectory / partition.CellFile(cell), std::ios::binary | std::ios::trunc);
                out << text;
                if (!out)
                {
                    NOX_CORE_ERROR("Streaming test world: could not write {}", partition.CellFile(cell).generic_string());
                    m_Failed = true;
                    return;
                }
                partition.Cells[cell] = WorldCellInfo{ boundsMin, boundsMax, count };
            }
        }

        // What stays in the scene file: the camera over the middle (with the test, when named), the sun and the sky.
        const float middle = grid * cellSize * 0.5f;
        m_Middle = glm::vec3(middle, WorldUnits::FromMeters(20.0f), middle);
        Entity camera = world->CreateEntity("Camera");
        camera.GetComponent<TransformComponent>().Translation = glm::vec3(middle, WorldUnits::FromMeters(60.0f), middle);
        camera.GetComponent<TransformComponent>().Rotation = glm::vec3(-0.2f, 0.0f, 0.0f);
        CameraComponent& cameraComponent = camera.AddComponent<CameraComponent>();
        cameraComponent.Primary = true;
        // (A new camera is orthographic: a 10 unit box, nothing of a 3D world in it.)
        cameraComponent.Camera.SetPerspective(glm::radians(60.0f), WorldUnits::FromMeters(0.1f), WorldUnits::FromMeters(1000.0f));
        if (!m_Settings.CameraScript.empty())
            camera.AddComponent<ScriptComponent>().ClassNames.push_back(m_Settings.CameraScript);
        Entity sun = world->CreateEntity("Sun");
        sun.GetComponent<TransformComponent>().Rotation = glm::vec3(glm::radians(-40.0f), glm::radians(30.0f), 0.0f);
        sun.AddComponent<DirectionalLightComponent>().Intensity = 8.0f;
        Entity sky = world->CreateEntity("Sky");
        sky.AddComponent<EnvironmentLightComponent>().Source = EnvironmentSource::ProceduralSky;

        SceneSerializer serializer(world);
        serializer.Serialize(m_AssetDirectory / scenePath);
        m_Scene = registered(AssetType::Scene, scenePath);
        assets.SerializeAssetRegistry();
        NOX_CORE_INFO("Streaming test world: {} x {} cells of {} m ({:.1f} km across), {:.1f} GB, scene '{}'", grid, grid, m_Settings.CellSizeMeters,
                      grid * m_Settings.CellSizeMeters / 1000.0f, static_cast<double>(m_Bytes.load()) / (1024.0 * 1024.0 * 1024.0),
                      scenePath.generic_string());
    }
}
