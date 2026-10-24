#include "WorldPartition.h"

#include <algorithm>
#include <unordered_set>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>

#include "Components.h"
#include "Entity.h"
#include "EntityBounds.h"
#include "PrefabInstance.h"
#include "Scene.h"
#include "SceneLoader.h"
#include "SceneSerializer.h"
#include "NoxCore/Asset/AssetManager.h"
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
        uint64_t value = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(static_cast<uint32_t>(cell.Grid)) << 61) ^
                         (static_cast<uint64_t>(static_cast<uint32_t>(cell.Level)) << 56) ^
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
        return std::string(cell.Grid == HLODGridIndex ? "HLOD_" : "") + "L" + std::to_string(cell.Level) + "_X" + std::to_string(cell.X) + "_Z" +
               std::to_string(cell.Z);
    }

    WorldCellCoord WorldGrid::CellFor(const glm::vec3& boundsMin, const glm::vec3& boundsMax, float cellSizeWorld, int32_t grid)
    {
        const float extent = std::max(boundsMax.x - boundsMin.x, boundsMax.z - boundsMin.z);
        WorldCellCoord cell;
        cell.Grid = grid;
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
        if (!root || !IsRoot(root) || root.HasComponent<HLODComponent>()) // (HLODs are in their own grid's cells)
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

    int Scene::SplitIntoSeparateEntities(Entity root, std::string& outError)
    {
        // Unreal partitions per actor: a model placed as one instance (Bistro: one root, thousands of spawned parts) would be one
        // cell streaming the whole town at once. Its parts become entities of their own here, each a root where it stands.
        if (!root || !IsRoot(root))
        {
            outError = "only a root entity can be split";
            return 0;
        }
        if (root.HasComponent<ModelInstanceComponent>() && !root.GetComponent<ModelInstanceComponent>().Spawned)
        {
            outError = "the model is still loading";
            return 0;
        }
        bool animated = false;
        Visit(*this, root, [&](Entity entity) { animated |= entity.HasComponent<AnimatorComponent>(); });
        if (animated)
        {
            outError = "it is animated (its animator drives its parts as one tree)";
            return 0;
        }

        // Unpacked first: the parts are saved as ordinary entities from now on.
        if (root.HasComponent<PrefabInstanceComponent>())
            PrefabInstance::UnpackCompletely(*this, root);
        Visit(*this, root, [&](Entity entity)
        {
            if (entity.HasComponent<ModelInstanceComponent>())
                entity.RemoveComponent<ModelInstanceComponent>();
            if (entity.HasComponent<ModelNodeComponent>())
                entity.RemoveComponent<ModelNodeComponent>();
        });

        // Every part with something in the world becomes a root (keeping its place, filed in a folder named after the model); what
        // is left -- the groups that only held them -- goes when nothing is under it any more.
        std::vector<Entity> order;
        Visit(*this, root, [&](Entity entity) { order.push_back(entity); });
        const std::string folder = root.GetName();
        int split = 0;
        for (Entity entity : order)
        {
            if (entity == root || !HasWorldPresence(entity))
                continue;
            entity.SetParent(Entity());
            if (!entity.HasComponent<FolderComponent>())
                entity.AddComponent<FolderComponent>(folder);
            ++split;
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            Entity entity = *it;
            if (!entity || HasWorldPresence(entity) || IsGlobal(entity) || entity.HasComponent<ScriptComponent>())
                continue;
            const bool hasChildren = entity.HasComponent<RelationshipComponent>() && !entity.GetComponent<RelationshipComponent>().Children.empty();
            if (!hasChildren)
                DestroyEntity(entity);
        }
        NOX_CORE_INFO("Split '{}' into {} separate entities", folder, split);
        return split;
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

    Scene::WorldCellStage Scene::GetWorldCellStage(const WorldCellCoord& cell) const
    {
        for (const StreamedLevel& streamed : m_Levels)
        {
            if (!streamed.IsCell || streamed.Cell != cell || streamed.UnloadQueued)
                continue;
            if (streamed.Failed)
                return WorldCellStage::Unloaded;
            if (streamed.Loaded)
                return WorldCellStage::Activated;
            if (streamed.Loader)
                return WorldCellStage::Loading;
            if (streamed.Staged)
                return streamed.ActivateNext > 0 ? WorldCellStage::Activating : WorldCellStage::Loaded;
            return WorldCellStage::Loading;
        }
        return WorldCellStage::Unloaded;
    }

    bool Scene::IsWorldStreamingCompleted(const glm::vec3& point, float range, WorldCellStage stage) const
    {
        for (const auto& [cell, info] : m_World.Cells)
        {
            if (cell.Grid != WorldGrid::WorldGridIndex || WorldGrid::HorizontalDistance(point, info.BoundsMin, info.BoundsMax) > range)
                continue;
            const WorldCellStage reached = GetWorldCellStage(cell);
            const bool done = stage == WorldCellStage::Activated ? reached == WorldCellStage::Activated : reached >= stage;
            if (!done)
                return false;
        }
        return true;
    }

    std::vector<WorldCellCoord> Scene::GetLoadedWorldCells(bool includeLoading, int32_t grid) const
    {
        std::vector<WorldCellCoord> cells;
        for (const StreamedLevel& streamed : m_Levels)
        {
            if (streamed.IsCell && streamed.Cell.Grid == grid && !streamed.UnloadQueued && !streamed.Failed && (streamed.Loaded || includeLoading))
                cells.push_back(streamed.Cell);
        }
        return cells;
    }

    bool Scene::IsWorldStreamingBusy() const
    {
        // The world's cells only: HLODs stream on their own around the editor camera and are never edited.
        for (const StreamedLevel& streamed : m_Levels)
        {
            if (streamed.IsCell && streamed.Cell.Grid == WorldGrid::WorldGridIndex &&
                (streamed.Loader || streamed.UnloadQueued || (streamed.Staged && streamed.Activate)))
                return true;
        }
        return false;
    }

    void Scene::QueueWorldCellLoad(const WorldCellCoord& cell, bool activate)
    {
        auto existing = std::find_if(m_Levels.begin(), m_Levels.end(), [&](const StreamedLevel& s) { return s.IsCell && s.Cell == cell; });
        if (existing != m_Levels.end())
        {
            existing->UnloadQueued = false; // wanted again before its unload ran: it stays
            existing->Activate = activate;  // (a cell in the world whose target is Loaded now leaves it: UpdateLevelStreaming)
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
        streamed.Activate = activate;
        StageWorldCell(streamed);
    }

    void Scene::StageWorldCell(StreamedLevel& streamed)
    {
        streamed.Staged = nullptr;
        streamed.StagedEntities.clear();
        streamed.Assets.clear();
        streamed.AssetsReady = 0;
        streamed.AssetWait = 0.0f;
        streamed.ActivateNext = 0;
        streamed.ActivatedEntities.clear();
        streamed.Empty = false;
        if (auto memory = m_World.Overrides.find(streamed.Cell); memory != m_World.Overrides.end())
        {
            if (memory->second.empty())
            {
                streamed.Loaded = streamed.Empty = true; // emptied in the editor: nothing to load
                return;
            }
            streamed.Staged = CreateRef<Scene>();
            streamed.Loader = std::make_unique<SceneLoader>(streamed.Name, memory->second, *streamed.Staged, streamed.Level, true);
            return;
        }
        if (!m_World.Cells.contains(streamed.Cell) || m_World.CellDirectory.empty())
        {
            streamed.Loaded = streamed.Empty = true; // no file (yet): a new cell, empty
            return;
        }
        streamed.Staged = CreateRef<Scene>();
        streamed.Loader = std::make_unique<SceneLoader>(Project::GetActiveAssetDirectory() / m_World.CellFile(streamed.Cell), *streamed.Staged,
                                                        streamed.Level, true);
    }

    // How long a loaded cell's activation waits for its assets (meshes, models, materials) before it goes into the world anyway.
    static constexpr float MaxAssetWaitSeconds = 2.0f;

    Scene::CellStep Scene::StepWorldCell(StreamedLevel& streamed, double budgetMs, float deltaTime, bool waitForAssets)
    {
        const auto start = std::chrono::steady_clock::now();
        if (streamed.Loader)
        {
            if (!streamed.Loader->Step(budgetMs))
                return CellStep::Busy;
            if (streamed.Loader->Failed())
            {
                streamed.Loader.reset();
                streamed.Staged = nullptr;
                streamed.StagedEntities.clear();
                return CellStep::Failed;
            }
            // Loaded: what it references is requested now, to be in when it activates.
            // Every entity of the built cell: the file's in file order, then any its components made while it was built.
            streamed.StagedEntities = streamed.Loader->GetCreatedEntities();
            streamed.Loader.reset();
            {
                const std::unordered_set<UUID> listed(streamed.StagedEntities.begin(), streamed.StagedEntities.end());
                for (auto handle : streamed.Staged->m_Registry.view<IDComponent>())
                {
                    const UUID id = streamed.Staged->m_Registry.get<IDComponent>(handle).ID;
                    if (!listed.contains(id))
                        streamed.StagedEntities.push_back(id);
                }
            }
            std::unordered_set<AssetHandle> handles;
            streamed.Staged->CollectAssetReferences(handles);
            streamed.Assets.assign(handles.begin(), handles.end());
            for (AssetHandle handle : streamed.Assets)
                AssetManager::RequestAsset(handle);
            NOX_CORE_TRACE("World cell `{}` loaded ({} entities, {} assets)", streamed.Name, streamed.StagedEntities.size(), streamed.Assets.size());
        }
        if (!streamed.Staged || !streamed.Activate)
            return CellStep::Busy; // (Loaded is its target)
        if (streamed.ActivateNext == 0 && waitForAssets)
        {
            while (streamed.AssetsReady < streamed.Assets.size() && AssetManager::RequestAsset(streamed.Assets[streamed.AssetsReady]) != AssetState::Loading)
                ++streamed.AssetsReady;
            if (streamed.AssetsReady < streamed.Assets.size() && streamed.AssetWait < MaxAssetWaitSeconds)
            {
                streamed.AssetWait += deltaTime;
                return CellStep::Busy;
            }
        }
        const double spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!ActivateWorldCellEntities(streamed, std::max(budgetMs - spent, 0.0)))
            return CellStep::Busy;
        streamed.Staged = nullptr;
        streamed.StagedEntities = {};
        streamed.Assets = {};
        return CellStep::Activated;
    }

    void Scene::RemoveWorldCellFromWorld(StreamedLevel& streamed)
    {
        // Its roots once activated (Origins), and the entities copied in so far while it is activating (no origins yet).
        DestroyWorldCellEntities(streamed.Cell);
        for (UUID id : streamed.ActivatedEntities)
        {
            if (Entity entity = GetEntityByUUID(id))
                DestroyEntity(entity);
        }
        streamed.ActivatedEntities.clear();
        streamed.ActivateNext = 0;
        streamed.HeldPhysics.clear();
        streamed.Loaded = false;
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
        // The roots that belong to the cell -- loaded from it, or last saved to it -- with their trees. Not by LevelComponent: that
        // is the cell an entity was loaded from, and an entity moved into another cell and saved belongs to that one now (unloading
        // its old cell took it from the editor although its new cell's file holds it).
        std::vector<UUID> roots;
        for (auto origin = m_World.Origins.begin(); origin != m_World.Origins.end();)
        {
            if (origin->second == cell)
            {
                roots.push_back(origin->first);
                origin = m_World.Origins.erase(origin);
            }
            else
                ++origin;
        }
        for (UUID rootID : roots)
        {
            if (Entity root = GetEntityByUUID(rootID))
                DestroyEntity(root);
        }
    }

    std::vector<WorldCellCoord> Scene::WorldCellsAround(const glm::vec3& point, float range, int32_t grid) const
    {
        std::vector<WorldCellCoord> cells;
        for (const auto& [cell, info] : m_World.Cells)
        {
            if (cell.Grid == grid && WorldGrid::HorizontalDistance(point, info.BoundsMin, info.BoundsMax) <= range)
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

    void Scene::CollectStreamingSources(const glm::vec3* source, std::vector<StreamingSource>& outSources, bool composed)
    {
        outSources.clear();
        if (source)
            outSources.push_back({ *source, 1.0f, true }); // the camera activates
        for (auto handle : m_Registry.view<WorldStreamingSourceComponent, WorldTransformComponent>())
        {
            const auto& component = m_Registry.get<WorldStreamingSourceComponent>(handle);
            if (!component.Enabled)
                continue;
            const glm::vec3 position = composed ? ComposedWorldPosition(Entity(handle, this))
                                                : glm::vec3(m_Registry.get<WorldTransformComponent>(handle).WorldMatrix[3]);
            outSources.push_back({ position, component.LoadingRangeScale, component.Target == WorldStreamingSourceComponent::TargetState::Activated });
        }
    }

    void Scene::UpdateWorldStreaming(const glm::vec3* source, bool composedSources)
    {
        if (!m_WorldStreaming || !m_World.Settings.Enabled)
            return;
        std::vector<StreamingSource>& sources = m_StreamingSources;
        CollectStreamingSources(source, sources, composedSources);
        if (sources.empty())
            return; // no source this frame (no primary camera): the cells stay as they are
        StreamWorldCells(sources, -1);
    }

    void Scene::StreamWorldCells(const std::vector<StreamingSource>& sources, int32_t onlyGrid)
    {
        // Each grid by its own range (the world's cells by the loading range, the HLODs by theirs), times each source's scale.
        const float ranges[2] = { WorldUnits::FromMeters(m_World.Settings.LoadingRange), WorldUnits::FromMeters(m_World.Settings.HLODLoadingRange) };
        std::map<WorldCellCoord, StreamedLevel*> present;
        for (StreamedLevel& streamed : m_Levels)
        {
            if (streamed.IsCell)
                present[streamed.Cell] = &streamed;
        }

        // Wanted cells nearest first, activated where an activating source reaches them; the hysteresis keeps a cell (and its being in
        // the world) until every source has left it by that much more.
        struct Want
        {
            float Distance;
            WorldCellCoord Cell;
            bool Activate;
        };
        std::vector<Want> wanted;
        for (const auto& [cell, info] : m_World.Cells)
        {
            if (onlyGrid >= 0 && cell.Grid != onlyGrid)
                continue;
            const float range = ranges[cell.Grid == WorldGrid::HLODGridIndex ? 1 : 0];
            float nearest = FLT_MAX;
            bool load = false, keep = false, activate = false, keepActive = false;
            for (const StreamingSource& source : sources)
            {
                const float distance = WorldGrid::HorizontalDistance(source.Position, info.BoundsMin, info.BoundsMax);
                const float sourceRange = range * source.RangeScale;
                const bool inside = distance <= sourceRange;
                const bool kept = distance <= sourceRange * (1.0f + WorldGrid::UnloadHysteresis);
                nearest = std::min(nearest, distance);
                load |= inside;
                keep |= kept;
                activate |= inside && source.Activate;
                keepActive |= kept && source.Activate;
            }
            auto found = present.find(cell);
            StreamedLevel* streamed = found != present.end() ? found->second : nullptr;
            if (!streamed || streamed->UnloadQueued)
            {
                if (load)
                    wanted.push_back({ nearest, cell, activate });
            }
            else if (!keep)
                streamed->UnloadQueued = true;
            else
                streamed->Activate = streamed->Activate ? keepActive : activate;
        }
        std::sort(wanted.begin(), wanted.end(), [](const Want& a, const Want& b) { return a.Distance < b.Distance; });
        for (const Want& want : wanted)
            QueueWorldCellLoad(want.Cell, want.Activate);
    }

    void Scene::UpdateEditorHLODs(const glm::vec3& camera)
    {
        // While editing, the HLOD grid streams around the editor camera (the world's cells are loaded by hand): what is not loaded
        // shows as its HLODs. Off, they go.
        if (!m_World.Settings.Enabled)
            return;
        if (m_ShowHLODs)
        {
            const std::vector<StreamingSource> sources{ { camera, 1.0f, true } };
            StreamWorldCells(sources, WorldGrid::HLODGridIndex);
        }
        else
            UnloadWorldCells(GetLoadedWorldCells(true, WorldGrid::HLODGridIndex));
    }

    void Scene::UpdateHLODVisibility()
    {
        // An HLOD is drawn until its cell is loaded and drawn: loaded, its model and prefab instances spawned and none of its entities
        // waiting to be registered with the GPU scene -- so no frame shows neither. Hidden HLODs are not registered (nor traced).
        auto hlods = m_Registry.view<HLODComponent>();
        if (hlods.begin() == hlods.end())
            return;
        std::set<WorldCellCoord> drawn;
        for (const WorldCellCoord& cell : GetLoadedWorldCells(false, WorldGrid::WorldGridIndex))
            drawn.insert(cell);
        if (!drawn.empty())
        {
            std::unordered_set<uint64_t> notReady; // cell IDs
            for (entt::entity pending : m_PendingMeshEntities)
            {
                if (const LevelComponent* level = m_Registry.valid(pending) ? m_Registry.try_get<LevelComponent>(pending) : nullptr)
                    notReady.insert(level->Level);
            }
            for (auto handle : m_Registry.view<ModelInstanceComponent, LevelComponent>())
            {
                if (!m_Registry.get<ModelInstanceComponent>(handle).Spawned)
                    notReady.insert(m_Registry.get<LevelComponent>(handle).Level);
            }
            for (auto it = drawn.begin(); it != drawn.end();)
            {
                if (notReady.contains(WorldGrid::CellID(*it)))
                    it = drawn.erase(it);
                else
                    ++it;
            }
        }
        for (auto handle : hlods)
        {
            HLODComponent& hlod = hlods.get<HLODComponent>(handle);
            const bool hidden = drawn.contains(hlod.Source);
            if (hidden == hlod.Hidden)
                continue;
            hlod.Hidden = hidden;
            m_PendingMeshEntities.push_back(handle); // registered again, or not at all
        }
    }

    void Scene::BeginWorldStreaming(const glm::vec3* source)
    {
        m_WorldStreaming = false;
        if (!m_World.Settings.Enabled)
            return;
        m_WorldStreaming = true;
        NOX_PROFILE_SCOPE("World Streaming Start");

        // 0. The HLODs the editor streamed are in the copy: the session streams its own.
        {
            std::vector<UUID> hlods;
            for (auto handle : m_Registry.view<HLODComponent, IDComponent>())
                hlods.push_back(m_Registry.get<IDComponent>(handle).ID);
            for (UUID id : hlods)
            {
                m_World.Origins.erase(id);
                if (Entity entity = GetEntityByUUID(id))
                    DestroyEntity(entity);
            }
        }

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
            }
            // The index's bounds stay in, the editor's loaded cells' too: an entity without bounds yet (a mesh a script makes in Play,
            // a model not loaded) counts as its position only, and its cell's content shrank to that -- streamed later than its
            // file says, or not at all from where it should be (the test world's cells, 45 m away instead of 32).
            if (auto indexed = m_World.Cells.find(cell); indexed != m_World.Cells.end())
            {
                boundsMin = glm::min(boundsMin, indexed->second.BoundsMin);
                boundsMax = glm::max(boundsMax, indexed->second.BoundsMax);
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

        // 2. The cells around the sources, in before the first frame: built and activated now, without waiting for their assets.
        UpdateWorldStreaming(source, true);
        for (StreamedLevel& streamed : m_Levels)
        {
            if (!streamed.IsCell || streamed.Loaded || !streamed.Loader)
                continue;
            streamed.Loader->WaitForRead();
            CellStep step = CellStep::Busy;
            while (step == CellStep::Busy && (streamed.Loader || (streamed.Staged && streamed.Activate)))
                step = StepWorldCell(streamed, DBL_MAX, 0.0f, false);
            if (step == CellStep::Failed)
                streamed.Failed = true;
            else if (step == CellStep::Activated)
            {
                FinishLevelLoad(streamed, streamed.ActivatedEntities, {});
                streamed.ActivatedEntities.clear();
                streamed.Loaded = true;
            }
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
        const std::vector<WorldCellCoord> loadedList = GetLoadedWorldCells(false, WorldGrid::WorldGridIndex); // (the HLOD grid is built, not saved)
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
        m_ShowHLODs = from.m_ShowHLODs;
        for (const WorldCellCoord& cell : from.GetLoadedWorldCells(false, WorldGrid::WorldGridIndex))
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
