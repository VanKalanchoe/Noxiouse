#include "WorldPartition.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>

#include "Components.h"
#include "Entity.h"
#include "EntityBounds.h"
#include "Scene.h"
#include "SceneLoader.h"
#include "SceneSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    // --- The grid ---

    UUID WorldGrid::CellID(const WorldCellCoord& cell)
    {
        // splitmix64 over the packed coordinates and a tag: stable across sessions, spread over the 64 bits like random entity IDs.
        uint64_t value = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(static_cast<uint32_t>(cell.Level)) << 56) ^
                         (static_cast<uint64_t>(static_cast<uint32_t>(cell.X)) << 28) ^ static_cast<uint64_t>(static_cast<uint32_t>(cell.Z)) ^
                         0x4E4F5843454C4CULL; // "NOXCELL"
        value += 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        value ^= value >> 31;
        return UUID(value == 0 ? 1 : value);
    }

    std::string WorldGrid::CellName(const WorldCellCoord& cell)
    {
        return "L" + std::to_string(cell.Level) + "_X" + std::to_string(cell.X) + "_Z" + std::to_string(cell.Z);
    }

    WorldCellCoord WorldGrid::CellFor(const glm::vec3& boundsMin, const glm::vec3& boundsMax, float cellSizeWorld)
    {
        const float extent = std::max(boundsMax.x - boundsMin.x, boundsMax.z - boundsMin.z);
        WorldCellCoord cell;
        float size = cellSizeWorld;
        while (extent > size && cell.Level < MaxLevel)
        {
            ++cell.Level;
            size *= 2.0f;
        }
        const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
        cell.X = static_cast<int32_t>(std::floor(center.x / size));
        cell.Z = static_cast<int32_t>(std::floor(center.z / size));
        return cell;
    }

    void WorldGrid::CellRect(const WorldCellCoord& cell, float cellSizeWorld, glm::vec2& outMin, glm::vec2& outMax)
    {
        const float size = cellSizeWorld * static_cast<float>(1u << std::clamp(cell.Level, 0, MaxLevel));
        outMin = glm::vec2(static_cast<float>(cell.X) * size, static_cast<float>(cell.Z) * size);
        outMax = outMin + glm::vec2(size);
    }

    float WorldGrid::HorizontalDistance(const glm::vec3& point, const glm::vec3& boundsMin, const glm::vec3& boundsMax)
    {
        const float dx = std::max({ boundsMin.x - point.x, 0.0f, point.x - boundsMax.x });
        const float dz = std::max({ boundsMin.z - point.z, 0.0f, point.z - boundsMax.z });
        return std::sqrt(dx * dx + dz * dz);
    }

    std::filesystem::path WorldPartition::CellFile(const WorldCellCoord& cell) const
    {
        return CellDirectory / (WorldGrid::CellName(cell) + WorldGrid::CellExtension);
    }

    // --- Scene: which cell ---

    namespace
    {
        // Something in the world: what a spatially loaded root's tree has.
        bool HasWorldPresence(Entity entity)
        {
            return entity.HasComponent<MeshComponent>() || entity.HasComponent<ModelInstanceComponent>() ||
                   entity.HasComponent<PrefabInstanceComponent>() || entity.HasComponent<PointLightComponent>() ||
                   entity.HasComponent<SpotLightComponent>() || entity.HasComponent<RectLightComponent>() ||
                   entity.HasComponent<DiscLightComponent>() || entity.HasComponent<SpriteRendererComponent>() ||
                   entity.HasComponent<CircleRendererComponent>() || entity.HasComponent<ParticleEmitterComponent>() ||
                   entity.HasComponent<TextComponent>() || entity.HasComponent<RigidBody3DComponent>() ||
                   entity.HasComponent<RigidBody2DComponent>() || entity.HasComponent<BoxCollider3DComponent>() ||
                   entity.HasComponent<SphereCollider3DComponent>() || entity.HasComponent<CapsuleCollider3DComponent>();
        }

        // What the whole world needs whatever the streaming source's place: the sky, the sun, cameras, the player, streaming itself.
        bool IsGlobal(Entity entity)
        {
            return entity.HasComponent<CameraComponent>() || entity.HasComponent<DirectionalLightComponent>() ||
                   entity.HasComponent<EnvironmentLightComponent>() || entity.HasComponent<CharacterController3DComponent>() ||
                   entity.HasComponent<LevelStreamingVolumeComponent>() || entity.HasComponent<WorldStreamingSourceComponent>() ||
                   entity.HasComponent<TimelinePlayerComponent>();
        }

        bool IsRoot(Entity entity)
        {
            return !entity.HasComponent<RelationshipComponent>() || entity.GetComponent<RelationshipComponent>().Parent == 0;
        }

        void Visit(Scene& scene, Entity entity, const std::function<void(Entity)>& visitor)
        {
            visitor(entity);
            if (!entity.HasComponent<RelationshipComponent>())
                return;
            for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
            {
                if (Entity child = scene.GetEntityByUUID(childID))
                    Visit(scene, child, visitor);
            }
        }
    }

    bool Scene::IsSpatiallyLoaded(Entity root)
    {
        if (!root || !IsRoot(root))
            return false;
        if (root.HasComponent<WorldPartitionComponent>())
        {
            const auto mode = root.GetComponent<WorldPartitionComponent>().Mode;
            if (mode != WorldPartitionComponent::LoadingMode::Auto)
                return mode == WorldPartitionComponent::LoadingMode::Spatial;
        }
        bool presence = false;
        bool global = false;
        Visit(*this, root, [&](Entity entity)
        {
            presence |= HasWorldPresence(entity);
            global |= IsGlobal(entity);
        });
        return presence && !global;
    }

    bool Scene::GetWorldCell(Entity root, WorldCellCoord& outCell)
    {
        if (!m_World.Settings.Enabled || !IsSpatiallyLoaded(root))
            return false;
        const float cellSize = WorldUnits::FromMeters(m_World.Settings.CellSize);
        const WorldBounds bounds = ComputeWorldBounds(*this, root, true);
        if (bounds.Valid)
        {
            outCell = WorldGrid::CellFor(bounds.Min, bounds.Max, cellSize);
            return true;
        }
        // Nothing with bounds yet (its mesh is not loaded): the cell it came from, else its place.
        if (auto origin = m_World.Origins.find(root.GetUUID()); origin != m_World.Origins.end())
        {
            outCell = origin->second;
            return true;
        }
        const glm::vec3 position = root.HasComponent<WorldTransformComponent>()
            ? glm::vec3(root.GetComponent<WorldTransformComponent>().WorldMatrix[3])
            : root.GetComponent<TransformComponent>().Translation;
        outCell = WorldGrid::CellFor(position, position, cellSize);
        return true;
    }

    std::map<WorldCellCoord, std::vector<Entity>> Scene::CollectWorldCellRoots()
    {
        std::map<WorldCellCoord, std::vector<Entity>> cells;
        if (!m_World.Settings.Enabled)
            return cells;
        for (auto handle : m_Registry.view<IDComponent>())
        {
            Entity entity(handle, this);
            WorldCellCoord cell;
            if (IsRoot(entity) && GetWorldCell(entity, cell))
                cells[cell].push_back(entity);
        }
        return cells;
    }

    // The bounds of a cell's roots (their positions where they have none), for the index.
    static void RootsBounds(Scene& scene, const std::vector<Entity>& roots, glm::vec3& outMin, glm::vec3& outMax)
    {
        outMin = glm::vec3(FLT_MAX);
        outMax = glm::vec3(-FLT_MAX);
        for (Entity root : roots)
        {
            const WorldBounds bounds = ComputeWorldBounds(scene, root, true);
            if (bounds.Valid)
            {
                outMin = glm::min(outMin, bounds.Min);
                outMax = glm::max(outMax, bounds.Max);
                continue;
            }
            const glm::vec3 position = root.HasComponent<WorldTransformComponent>()
                ? glm::vec3(root.GetComponent<WorldTransformComponent>().WorldMatrix[3])
                : root.GetComponent<TransformComponent>().Translation;
            outMin = glm::min(outMin, position);
            outMax = glm::max(outMax, position);
        }
    }

    void Scene::EnableWorldPartition(const WorldPartitionSettings& settings)
    {
        m_World.Settings = settings;
        m_World.Settings.Enabled = true;
        // Every cell with entities in memory is loaded (its file is written on the next save). Cells already loaded stay loaded: after
        // a new cell size their old files are written empty, that is removed.
        for (const auto& [cell, roots] : CollectWorldCellRoots())
        {
            for (Entity root : roots)
                m_World.Origins[root.GetUUID()] = cell;
            if (GetWorldCellState(cell) != LevelState::Unloaded)
                continue;
            StreamedLevel& streamed = m_Levels.emplace_back();
            streamed.Level = WorldGrid::CellID(cell);
            streamed.IsCell = true;
            streamed.Cell = cell;
            streamed.Name = WorldGrid::CellName(cell);
            streamed.Loaded = true;
        }
    }

    // --- Scene: loading and unloading cells ---

    Scene::LevelState Scene::GetWorldCellState(const WorldCellCoord& cell) const
    {
        for (const StreamedLevel& streamed : m_Levels)
        {
            if (streamed.IsCell && streamed.Cell == cell && !streamed.UnloadQueued)
                return streamed.Failed ? LevelState::Unloaded : streamed.Loaded ? LevelState::Loaded : LevelState::Loading;
        }
        return LevelState::Unloaded;
    }

    std::vector<WorldCellCoord> Scene::GetLoadedWorldCells(bool includeLoading) const
    {
        std::vector<WorldCellCoord> cells;
        for (const StreamedLevel& streamed : m_Levels)
        {
            if (streamed.IsCell && !streamed.UnloadQueued && !streamed.Failed && (streamed.Loaded || includeLoading))
                cells.push_back(streamed.Cell);
        }
        return cells;
    }

    bool Scene::IsWorldStreamingBusy() const
    {
        for (const StreamedLevel& streamed : m_Levels)
        {
            if (streamed.IsCell && (streamed.Loader || streamed.UnloadQueued))
                return true;
        }
        return false;
    }

    void Scene::QueueWorldCellLoad(const WorldCellCoord& cell)
    {
        auto existing = std::find_if(m_Levels.begin(), m_Levels.end(), [&](const StreamedLevel& s) { return s.IsCell && s.Cell == cell; });
        if (existing != m_Levels.end())
        {
            existing->UnloadQueued = false; // wanted again before its unload ran: it stays
            if (!existing->Failed)
                return;
            // Failed before (unreadable): asked for again, it is tried again.
            existing->Failed = false;
            existing->Loaded = false;
        }
        StreamedLevel& streamed = existing != m_Levels.end() ? *existing : m_Levels.emplace_back();
        streamed.Level = WorldGrid::CellID(cell);
        streamed.IsCell = true;
        streamed.Cell = cell;
        streamed.Name = WorldGrid::CellName(cell);
        if (auto memory = m_World.Overrides.find(cell); memory != m_World.Overrides.end())
        {
            if (memory->second.empty())
                streamed.Loaded = true; // emptied in the editor: nothing to load
            else
                streamed.Loader = std::make_unique<SceneLoader>(streamed.Name, memory->second, *this, streamed.Level, true);
            return;
        }
        if (!m_World.Cells.contains(cell) || m_World.CellDirectory.empty())
        {
            streamed.Loaded = true; // no file (yet): a new cell, empty
            return;
        }
        streamed.Loader = std::make_unique<SceneLoader>(Project::GetActiveAssetDirectory() / m_World.CellFile(cell), *this, streamed.Level, true);
    }

    void Scene::LoadWorldCells(const std::vector<WorldCellCoord>& cells)
    {
        if (!m_World.Settings.Enabled)
            return;
        for (const WorldCellCoord& cell : cells)
            QueueWorldCellLoad(cell);
    }

    void Scene::UnloadWorldCells(const std::vector<WorldCellCoord>& cells)
    {
        for (StreamedLevel& streamed : m_Levels)
        {
            if (streamed.IsCell && std::find(cells.begin(), cells.end(), streamed.Cell) != cells.end())
                streamed.UnloadQueued = true;
        }
    }

    void Scene::DestroyWorldCellEntities(const WorldCellCoord& cell)
    {
        // The entities the cell's load made (their LevelComponent), and the roots that came from it or were saved to it (an undo step
        // restored after the load made them anew).
        const UUID id = WorldGrid::CellID(cell);
        std::vector<UUID> entities;
        for (auto handle : m_Registry.view<LevelComponent, IDComponent>())
        {
            if (m_Registry.get<LevelComponent>(handle).Level == id)
                entities.push_back(m_Registry.get<IDComponent>(handle).ID);
        }
        for (auto origin = m_World.Origins.begin(); origin != m_World.Origins.end();)
        {
            if (origin->second == cell)
            {
                entities.push_back(origin->first);
                origin = m_World.Origins.erase(origin);
            }
            else
                ++origin;
        }
        for (UUID entityID : entities)
        {
            if (Entity entity = GetEntityByUUID(entityID))
                DestroyEntity(entity);
        }
    }

    std::vector<WorldCellCoord> Scene::WorldCellsAround(const glm::vec3& point, float range) const
    {
        std::vector<WorldCellCoord> cells;
        for (const auto& [cell, info] : m_World.Cells)
        {
            if (WorldGrid::HorizontalDistance(point, info.BoundsMin, info.BoundsMax) <= range)
                cells.push_back(cell);
        }
        return cells;
    }

    // --- Scene: streaming in Play and Simulate ---

    glm::vec3 Scene::ComposedWorldPosition(Entity entity)
    {
        // Up the parents (a scene not updated yet has no world transforms: one just loaded by the runtime).
        glm::mat4 world = entity.GetComponent<TransformComponent>().GetTransform();
        UUID parentID = entity.HasComponent<RelationshipComponent>() ? entity.GetComponent<RelationshipComponent>().Parent : UUID(0);
        for (int depth = 0; parentID != 0 && depth < 1024; ++depth)
        {
            Entity parent = GetEntityByUUID(parentID);
            if (!parent)
                break;
            world = parent.GetComponent<TransformComponent>().GetTransform() * world;
            parentID = parent.HasComponent<RelationshipComponent>() ? parent.GetComponent<RelationshipComponent>().Parent : UUID(0);
        }
        return glm::vec3(world[3]);
    }

    void Scene::CollectStreamingSources(const glm::vec3* source, std::vector<glm::vec3>& outSources, bool composed)
    {
        outSources.clear();
        if (source)
            outSources.push_back(*source);
        for (auto handle : m_Registry.view<WorldStreamingSourceComponent, WorldTransformComponent>())
        {
            if (!m_Registry.get<WorldStreamingSourceComponent>(handle).Enabled)
                continue;
            outSources.push_back(composed ? ComposedWorldPosition(Entity(handle, this))
                                          : glm::vec3(m_Registry.get<WorldTransformComponent>(handle).WorldMatrix[3]));
        }
    }

    void Scene::UpdateWorldStreaming(const glm::vec3* source, bool composedSources)
    {
        if (!m_WorldStreaming || !m_World.Settings.Enabled)
            return;
        std::vector<glm::vec3>& sources = m_StreamingSources;
        CollectStreamingSources(source, sources, composedSources);
        if (sources.empty())
            return; // no source this frame (no primary camera): the cells stay as they are

        const float range = WorldUnits::FromMeters(m_World.Settings.LoadingRange);
        const float unloadRange = range * (1.0f + WorldGrid::UnloadHysteresis);
        std::map<WorldCellCoord, StreamedLevel*> present;
        for (StreamedLevel& streamed : m_Levels)
        {
            if (streamed.IsCell)
                present[streamed.Cell] = &streamed;
        }

        // Wanted cells nearest first; the ones every source has left by the hysteresis go.
        std::vector<std::pair<float, WorldCellCoord>> wanted;
        for (const auto& [cell, info] : m_World.Cells)
        {
            float distance = FLT_MAX;
            for (const glm::vec3& point : sources)
                distance = std::min(distance, WorldGrid::HorizontalDistance(point, info.BoundsMin, info.BoundsMax));
            auto found = present.find(cell);
            StreamedLevel* streamed = found != present.end() ? found->second : nullptr;
            if (distance <= range)
            {
                if (!streamed || streamed->UnloadQueued)
                    wanted.emplace_back(distance, cell);
            }
            else if (streamed && distance > unloadRange)
                streamed->UnloadQueued = true;
        }
        std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [distance, cell] : wanted)
            QueueWorldCellLoad(cell);
    }

    void Scene::BeginWorldStreaming(const glm::vec3* source)
    {
        m_WorldStreaming = false;
        if (!m_World.Settings.Enabled)
            return;
        m_WorldStreaming = true;
        NOX_PROFILE_SCOPE("World Streaming Start");

        // 1. A copy of an edited world: the cells' entities in it become in-memory cells (unsaved changes included), streamed like
        //    the files. A cell the editor had loaded is its entities as they are now (none: nothing); entities moved into a cell it had
        //    not loaded join that cell's file.
        const std::map<WorldCellCoord, std::vector<Entity>> groups = CollectWorldCellRoots();
        std::set<WorldCellCoord> cells = m_World.EditorLoaded;
        for (const auto& [cell, roots] : groups)
            cells.insert(cell);
        static const std::vector<Entity> none;
        for (const WorldCellCoord& cell : cells)
        {
            auto group = groups.find(cell);
            const std::vector<Entity>& roots = group != groups.end() ? group->second : none;
            const std::string name = WorldGrid::CellName(cell);
            uint32_t count = 0;
            std::string text;
            glm::vec3 boundsMin, boundsMax;
            RootsBounds(*this, roots, boundsMin, boundsMax);
            if (m_World.EditorLoaded.contains(cell))
            {
                if (!roots.empty())
                    text = SceneSerializer::WorldCellText(*this, roots, name, &count);
            }
            else
            {
                std::string fileText;
                if (m_World.Cells.contains(cell) && !m_World.CellDirectory.empty())
                    VFS::ReadText(Project::GetActiveAssetDirectory() / m_World.CellFile(cell), fileText);
                text = SceneSerializer::MergeWorldCellText(*this, fileText, roots, name, count);
                if (auto indexed = m_World.Cells.find(cell); indexed != m_World.Cells.end())
                {
                    boundsMin = glm::min(boundsMin, indexed->second.BoundsMin);
                    boundsMax = glm::max(boundsMax, indexed->second.BoundsMax);
                }
            }
            if (count == 0)
            {
                m_World.Overrides[cell] = std::string();
                m_World.Cells.erase(cell);
                continue;
            }
            m_World.Overrides[cell] = std::move(text);
            m_World.Cells[cell] = WorldCellInfo{ boundsMin, boundsMax, count };
        }
        for (const auto& [cell, roots] : groups)
        {
            for (Entity root : roots)
            {
                m_World.Origins.erase(root.GetUUID());
                DestroyEntity(root);
            }
        }

        // 2. The cells around the sources, in before the first frame.
        UpdateWorldStreaming(source, true);
        for (StreamedLevel& streamed : m_Levels)
        {
            if (!streamed.IsCell || !streamed.Loader)
                continue;
            streamed.Loader->WaitForRead();
            streamed.Loader->Step(DBL_MAX);
            if (streamed.Loader->Failed())
            {
                DestroyWorldCellEntities(streamed.Cell);
                streamed.Failed = true;
            }
            else
            {
                FinishLevelLoad(streamed, streamed.Loader->GetCreatedEntities(), {});
                streamed.Loaded = true;
            }
            streamed.Loader.reset();
        }
    }

    // --- Scene: saving ---

    bool Scene::IsWorldCellModified(const WorldCellCoord& cell)
    {
        const auto groups = CollectWorldCellRoots();
        auto group = groups.find(cell);
        std::string text;
        if (group != groups.end())
            text = SceneSerializer::WorldCellText(*this, group->second, WorldGrid::CellName(cell), nullptr);
        std::string fileText;
        if (m_World.Cells.contains(cell) && !m_World.CellDirectory.empty())
            VFS::ReadText(Project::GetActiveAssetDirectory() / m_World.CellFile(cell), fileText);
        return text != fileText;
    }

    static bool WriteTextFile(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out)
        {
            NOX_CORE_ERROR("World partition: could not write '{}'", path.string());
            return false;
        }
        return true;
    }

    static std::filesystem::path DefaultCellDirectory(const std::filesystem::path& scenePath)
    {
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        std::filesystem::path relative = scenePath.lexically_normal().lexically_relative(assetDirectory.lexically_normal());
        if (relative.empty() || *relative.begin() == "..")
            relative = scenePath; // outside the asset directory: next to the file
        return relative.parent_path() / (scenePath.stem().string() + "_Cells");
    }

    bool Scene::SaveWorldCells(const std::filesystem::path& scenePath, std::unordered_set<UUID>& outSavedInCells)
    {
        if (!m_World.Settings.Enabled)
            return true;
        NOX_PROFILE_SCOPE("World Partition Save");
        if (m_World.CellDirectory.empty())
            m_World.CellDirectory = DefaultCellDirectory(scenePath);
        const std::filesystem::path directory = Project::GetActiveAssetDirectory() / m_World.CellDirectory;
        std::error_code error;
        std::filesystem::create_directories(directory, error);

        const auto groups = CollectWorldCellRoots();
        const std::vector<WorldCellCoord> loadedList = GetLoadedWorldCells(false);
        const std::set<WorldCellCoord> loaded(loadedList.begin(), loadedList.end());
        static const std::vector<Entity> none;
        bool ok = true;
        size_t written = 0, removed = 0;
        std::vector<WorldCellCoord> joined; // cells not loaded that entities moved into: loaded after the save

        // Loaded cells: their files are what is in memory (no entities: no file).
        for (const WorldCellCoord& cell : loaded)
        {
            auto group = groups.find(cell);
            const std::vector<Entity>& roots = group != groups.end() ? group->second : none;
            const std::filesystem::path file = directory / (WorldGrid::CellName(cell) + WorldGrid::CellExtension);
            if (roots.empty())
            {
                if (std::filesystem::remove(file, error))
                    ++removed;
                m_World.Cells.erase(cell);
                continue;
            }
            uint32_t count = 0;
            const std::string text = SceneSerializer::WorldCellText(*this, roots, WorldGrid::CellName(cell), &count);
            ok &= WriteTextFile(file, text);
            WorldCellInfo& info = m_World.Cells[cell];
            RootsBounds(*this, roots, info.BoundsMin, info.BoundsMax);
            info.Entities = count;
            ++written;
        }
        // Cells not (fully) loaded that entities were moved or placed into: merged into their files (an entity already there is
        // replaced, the others kept).
        for (const auto& [cell, roots] : groups)
        {
            if (loaded.contains(cell))
                continue;
            const std::filesystem::path file = directory / (WorldGrid::CellName(cell) + WorldGrid::CellExtension);
            std::string fileText;
            VFS::ReadText(file, fileText);
            uint32_t count = 0;
            const std::string text = SceneSerializer::MergeWorldCellText(*this, fileText, roots, WorldGrid::CellName(cell), count);
            ok &= WriteTextFile(file, text);
            glm::vec3 boundsMin, boundsMax;
            RootsBounds(*this, roots, boundsMin, boundsMax);
            auto [entry, added] = m_World.Cells.try_emplace(cell, WorldCellInfo{ boundsMin, boundsMax, count });
            if (!added)
            {
                entry->second.BoundsMin = glm::min(entry->second.BoundsMin, boundsMin);
                entry->second.BoundsMax = glm::max(entry->second.BoundsMax, boundsMax);
                entry->second.Entities = count;
            }
            joined.push_back(cell);
            ++written;
        }

        // What went into the cells stays out of the scene file; each root now belongs to the cell it was saved to.
        for (const auto& [cell, roots] : groups)
        {
            for (Entity root : roots)
            {
                m_World.Origins[root.GetUUID()] = cell;
                Visit(*this, root, [&](Entity entity) { outSavedInCells.insert(entity.GetUUID()); });
            }
        }
        LoadWorldCells(joined);
        NOX_CORE_INFO("World partition: {} cell file(s) written, {} removed, {} cells in '{}'", written, removed, m_World.Cells.size(),
                      m_World.CellDirectory.generic_string());
        return ok;
    }

    bool Scene::CopyWorldCellsTo(const std::filesystem::path& scenePath)
    {
        if (!m_World.Settings.Enabled || m_World.CellDirectory.empty())
            return true;
        const std::filesystem::path target = DefaultCellDirectory(scenePath);
        if (target.lexically_normal() == m_World.CellDirectory.lexically_normal())
            return true;
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        std::error_code error;
        std::filesystem::create_directories(assetDirectory / target, error);
        bool ok = true;
        for (const auto& [cell, info] : m_World.Cells)
        {
            const std::string file = WorldGrid::CellName(cell) + WorldGrid::CellExtension;
            std::filesystem::copy_file(assetDirectory / m_World.CellDirectory / file, assetDirectory / target / file,
                                       std::filesystem::copy_options::overwrite_existing, error);
            if (error)
            {
                NOX_CORE_ERROR("World partition: could not copy cell '{}' for Save As: {}", file, error.message());
                ok = false;
            }
        }
        m_World.CellDirectory = target;
        return ok;
    }

    void Scene::AdoptWorldState(Scene& from)
    {
        // The world as the editor has it (settings, index, which cells are loaded, where roots came from): none of it is an undo step
        // (undoing past a conversion does not turn the world back into a scene whose entities are in cell files).
        m_World = from.m_World;
        for (const WorldCellCoord& cell : from.GetLoadedWorldCells(false))
        {
            StreamedLevel& streamed = m_Levels.emplace_back();
            streamed.Level = WorldGrid::CellID(cell);
            streamed.IsCell = true;
            streamed.Cell = cell;
            streamed.Name = WorldGrid::CellName(cell);
            streamed.Loaded = true;
        }
    }
}
