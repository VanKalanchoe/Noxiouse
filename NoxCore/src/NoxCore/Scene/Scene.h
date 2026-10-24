#pragma once
#include <entt/entt.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <box2d/id.h>

#include "EntityCommandBuffer.h"
#include "WorldPartition.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/UUID.h"
#include "NoxCore/Core/Timestep.h"
#include "NoxCore/Renderer/EditorCamera.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Tasks/SystemGraph.h"
#include "NoxCore/Tasks/WorkerLocal.h"

namespace Nox
{
    class Entity; // Forward declaration
    struct AnimatorComponent;
    class IPhysics3DScene;
    class SceneLoader;
    class Timeline;

    class Scene : public Asset
    {
    public:
        Scene();
        ~Scene();

        static Ref<Scene> Copy(Ref<Scene> other);

        virtual AssetType GetType() const { return AssetType::Scene;};

        // Creates an entity, auto-generates a UUID, and assigns a name
        Entity CreateEntity(const std::string& name = std::string());
        // Creates an entity with a specific UUID (crucial for loading saved games!)
        Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
        void DestroyEntity(Entity entity);

        void OnRuntimeStart();
        void OnRuntimeStop();

        // `streamingSource`: where the world partition's cells are streamed around (the editor camera); Play uses the primary camera.
        void OnSimulationStart(const glm::vec3* streamingSource = nullptr);
        void OnSimulationStop();

        void OnUpdateRuntime(Timestep ts);
        void OnUpdateSimulation(Timestep ts, EditorCamera& camera);
        void OnUpdateEditor(Timestep ts, EditorCamera& camera);
        void OnViewportResize(uint32_t width, uint32_t height);

        Entity DuplicateEntity(Entity entity);

        Entity FindEntityByName(std::string_view name);
        Entity GetEntityByUUID(UUID uuid);

        Entity GetPrimaryCameraEntity();

        bool IsRunning() const { return m_IsRunning; }
        bool IsPaused() const { return m_IsPaused; }
        void SetPaused(bool paused) { m_IsPaused = paused; }
        void Step(int frames = 1);

        template<typename... Components>
        auto GetAllEntitiesWith()
        {
            return m_Registry.view<Components...>();
        }

        entt::registry& GetRegistry() { return m_Registry; }
        const entt::registry& GetRegistry() const { return m_Registry; }

        void SetRenderer(Renderer* renderer);
        void SetRenderer2D(Renderer2D* renderer) { m_renderer2D = renderer; }

        // Every asset handle this scene's components reference - the roots for unloading unused assets.
        void CollectAssetReferences(std::unordered_set<AssetHandle>& outHandles);
        // The Timeline window's preview while editing: the player's timeline posed at `time` seconds on its targets (written into
        // their components, like a Play frame would).
        void PreviewTimeline(Entity player, float time);
        // A timeline track's target: the entity reached from the player by a path of child names ("" = the player itself).
        Entity FindTimelineTarget(Entity player, const std::string& path);

        // True once after entities were destroyed since the last call, i.e. assets may have become unused.
        bool ConsumeAssetReferencesChanged() { return std::exchange(m_AssetReferencesChanged, false); }

        // Writes the scene's system graphs as GraphViz DOT files (SceneUpdate.dot, SceneSubmit.dot) into directory.
        bool DumpSystemGraphs(const std::filesystem::path& directory);

        IPhysics3DScene* GetPhysics3DScene() { return m_Physics3DScene.get(); }

        // Runtime: places a prefab in the scene and spawns it at once (see PrefabInstance::Instantiate); physics bodies and scripts
        // of its entities start as if the scene had begun with them. Returns the instance root (invalid when it fails).
        Entity Instantiate(AssetHandle prefab, const glm::vec3& position);
        // Destroys the entity at the end of the script update (a script may destroy itself: nothing in the middle of a frame's
        // script loop is removed).
        void QueueDestroy(Entity entity);

        // Level streaming (Unreal's): a level is another scene file (a scene asset) loaded into this running scene, its entities
        // marked with LevelComponent, and unloaded again (its entities destroyed). It is read on a thread and its entities created a
        // slice a frame (SceneLoader); its scripts, particles, timelines and physics start once the whole level is in, as if the scene
        // had begun with them. Both happen at the start of the next frame (UpdateLevelStreaming), so a script may call them. Play and
        // Simulate; Level Streaming Volumes call them too. A level whose entity IDs are already in the scene (a scene saved as a copy
        // of another) is refused.
        enum class LevelState : uint8_t
        {
            Unloaded,
            Loading,
            Loaded
        };
        bool LoadLevel(AssetHandle level);
        void UnloadLevel(AssetHandle level);
        LevelState GetLevelState(AssetHandle level) const;

        // World partition (WorldPartition.h, docs/Engine_Architecture_Plan_2026.md §5.13.1): the spatially loaded root entities are
        // kept in grid cells, each in its own file next to the scene, and streamed like levels -- around the streaming sources in Play
        // and Simulate (WorldStreamingSourceComponent, the camera), by hand in the editor.
        WorldPartition& GetWorldPartition() { return m_World; }
        const WorldPartition& GetWorldPartition() const { return m_World; }
        bool IsWorldPartitioned() const { return m_World.Settings.Enabled; }
        // Makes the scene a world (or applies new settings): the cells of the spatially loaded entities in memory count as loaded, their
        // files are written on the next save. A new cell size needs every cell loaded (the next save re-partitions the world).
        void EnableWorldPartition(const WorldPartitionSettings& settings);
        // Whether a root entity is kept in a cell (else in the scene file: sky, sun, cameras, logic, a WorldPartitionComponent's choice).
        bool IsSpatiallyLoaded(Entity root);
        // The cell a root entity is kept in, as a save would place it now; false: always loaded (a child, or no world partition).
        bool GetWorldCell(Entity root, WorldCellCoord& outCell);
        // A model or prefab instance (or any tree) as separate entities, each part with something in the world a root where it stands,
        // filed in a folder named after it: a world partitions per root, and one instance would be one cell (Bistro). Not for an
        // animated tree. Returns how many parts became roots; 0 with `outError` when it cannot.
        int SplitIntoSeparateEntities(Entity root, std::string& outError);
        // The spatially loaded root entities in memory by cell.
        std::map<WorldCellCoord, std::vector<Entity>> CollectWorldCellRoots();
        // By hand (the editor): read on a thread and created a slice a frame like levels; unloading destroys the cell's entities.
        void LoadWorldCells(const std::vector<WorldCellCoord>& cells);
        void UnloadWorldCells(const std::vector<WorldCellCoord>& cells);
        LevelState GetWorldCellState(const WorldCellCoord& cell) const;
        std::vector<WorldCellCoord> GetLoadedWorldCells(bool includeLoading, int32_t grid = WorldGrid::WorldGridIndex) const;
        bool IsWorldStreamingBusy() const; // a cell is loading or about to unload
        // The indexed cells whose content is within `range` (world units) of a point, horizontally.
        std::vector<WorldCellCoord> WorldCellsAround(const glm::vec3& point, float range, int32_t grid = WorldGrid::WorldGridIndex) const;
        // HLODs (§5.13.2) while editing: the HLOD grid streams around the editor camera when shown (World Partition panel).
        void SetShowHLODs(bool show) { m_ShowHLODs = show; }
        bool GetShowHLODs() const { return m_ShowHLODs; }
        // Whether a loaded cell's entities differ from its file (unsaved changes).
        bool IsWorldCellModified(const WorldCellCoord& cell);
        // Saving (SceneSerializer::Serialize): writes the cell files of the scene saved as `scenePath` and the index; the entities written
        // into cells are added to `outSavedInCells` (the scene file leaves them out). False on a write error.
        bool SaveWorldCells(const std::filesystem::path& scenePath, std::unordered_set<UUID>& outSavedInCells);
        // Save As: the cell files are copied to the new scene file's cell folder, which the scene uses from then on.
        bool CopyWorldCellsTo(const std::filesystem::path& scenePath);
        // An undo step restored into a new scene: the world's settings, index, loaded cells and where roots came from carry over.
        void AdoptWorldState(Scene& from);

        // Outliner folders (FolderComponent paths). Folders that hold entities exist through them; this list keeps the ones the
        // user made and left empty. Paths use '/' for nesting.
        const std::vector<std::string>& GetFolders() const { return m_Folders; }
        void AddFolder(const std::string& path);
        // Removes the folder and its subfolders; their entities go back to the top level.
        void RemoveFolder(const std::string& path);
        void RenameFolder(const std::string& from, const std::string& to);
    private:
        template<typename T>
        void OnComponentAdded(Entity entity, T& component);

        void OnPhysics2DStart();
        void OnPhysics2DStop();

        void OnPhysics3DStart();
        void OnPhysics3DStop();

        // Frame graph (§5.3): Game Update systems, then (after BeginScene on the main thread) the submission systems.
        void RegisterSystems();
        void FlushPendingDestroy();
        void StartPendingScripts();
        void RunUpdateSystems(Timestep ts, bool stepPhysics, bool stepAnimation);
        void RunSubmitSystems();
        // Main thread after each graph: deferred structural changes, then loads of assets the systems found unloaded.
        void ApplySyncPoint();
        // Main thread after the update systems (§5.5): brings the renderer's GPU scene up to date with what changed.
        void SyncGpuScene();
        // EnTT signals of the components that decide a mesh entity's GPU instances.
        void OnMeshEntityChanged(entt::registry& registry, entt::entity entity);
        // EnTT signals of the 3D physics components: added or removed while the physics scene runs (Duplicate, a script's
        // AddComponent, the properties panel), the entity's body / character is made again after the update systems.
        void OnPhysics3DComponentChanged(entt::registry& registry, entt::entity entity);
        void SyncPendingPhysics3D();

        // Level streaming, at the start of a running frame: the volumes against the streaming source, the unloads, then the loads for
        // LevelStreamingBudgetMs.
        void UpdateLevelStreaming(const glm::vec3* source, float deltaTime);
        struct StreamedLevel;
        void FinishLevelLoad(StreamedLevel& streamed, const std::vector<UUID>& created, const std::vector<UUID>& heldPhysics);
        void DestroyLevelEntities(AssetHandle level);
        // World partition: the copy of an edited world turned into in-memory cells and the cells around the sources loaded (Play /
        // Simulate start), then every frame the cells near a source wanted, the ones every source has left unloaded.
        void BeginWorldStreaming(const glm::vec3* source);
        // `composedSources`: the sources' places from their transforms up the parents (before the first update: no world transforms).
        void UpdateWorldStreaming(const glm::vec3* source, bool composedSources = false);
        void CollectStreamingSources(const glm::vec3* source, std::vector<glm::vec3>& outSources, bool composed);
        // The cells of every grid (onlyGrid -1) or of one near the sources wanted, the ones all of them have left unloaded.
        void StreamWorldCells(const std::vector<glm::vec3>& sources, int32_t onlyGrid);
        void UpdateEditorHLODs(const glm::vec3& camera);
        // HLODs whose cell is loaded and drawn are hidden (left out of the GPU scene), the others shown (SyncGpuScene).
        void UpdateHLODVisibility();
        glm::vec3 ComposedWorldPosition(Entity entity);
        void QueueWorldCellLoad(const WorldCellCoord& cell);
        void DestroyWorldCellEntities(const WorldCellCoord& cell);
        // A Box2D body for one entity with a RigidBody2DComponent (Play / Simulate start, and entities that come in later).
        void CreatePhysics2DBody(Entity entity);

        // Systems (run as tasks).
        void UpdatePhysics2D();
        void UpdatePhysics3D();
        void UpdateAnimators();
        // One animator's step (a task of UpdateAnimators).
        void UpdateAnimator(entt::entity entity, AnimatorComponent& animator, float ts);
        // Places the entities with a BoneSocketComponent on their bone (after the animators).
        void UpdateBoneSockets();
        // Sequencer timelines (TimelinePlayerComponent): played in Play and Simulate from their start.
        void StartTimelines();
        void UpdateTimelines();
        // cameraCuts: Camera Cut tracks switch the primary camera (Play only: the editor previews through its own camera).
        void ApplyTimeline(Entity player, const Timeline& timeline, float time, bool cameraCuts = false);
        void SubmitLights();
        void Submit2D();
        // Particle emitters (ParticleSystem.h): simulated every frame the scene steps (the editor's too), restarted by Play / Simulate.
        void StartParticles();
        void UpdateParticles();
        void SubmitParticles();

        const std::vector<glm::mat4>* GetBoneTransforms(entt::entity entity, const glm::mat4& meshWorld);

        std::string MakeUniqueDuplicateName(const std::string& baseName);
    private:
        struct SystemFrameInput
        {
            float Timestep = 0.0f;
            bool StepPhysics = false;
            bool StepAnimation = false;
        };

    private:
        entt::registry m_Registry;
        uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;
        float nearPlane, farPlane;

        b2WorldId m_PhysicsWorldID = b2_nullWorldId;
        std::unique_ptr<IPhysics3DScene> m_Physics3DScene;
        std::vector<std::string> m_Folders;
        bool m_IsRunning = false;
        bool m_TimelinesRunning = false; // Play or Simulate
        std::vector<UUID> m_PendingDestroy;
        std::vector<UUID> m_PendingScriptStart;
        std::unordered_set<UUID> m_PendingPhysics3D;
        bool m_IsPaused = false;
        int m_StepFrames = 0;
        float m_FixedTimeAccumulator = 0.0f; // seconds owed to OnFixedUpdate
        bool m_AssetReferencesChanged = false;

        std::unordered_map<UUID, entt::entity> m_EntityMap;

        // Level streaming: the levels loaded or on their way, and the volume timers (UpdateLevelStreaming).
        struct StreamedLevel
        {
            AssetHandle Level = 0;              // the level asset, or a world cell's ID (WorldGrid::CellID)
            bool IsCell = false;
            WorldCellCoord Cell;
            std::string Name;                   // for the log
            std::unique_ptr<SceneLoader> Loader; // while loading
            std::vector<UUID> HeldPhysics;      // 3D physics bodies made once the level is complete, not while its floor is missing
            bool Loaded = false;
            bool Failed = false;                // refused or unreadable: volumes leave it until the source has left them
            bool UnloadQueued = false;
            float OutsideTime = 0.0f;           // seconds no volume naming it has held the streaming source
        };
        std::vector<StreamedLevel> m_Levels;
        WorldPartition m_World;
        bool m_WorldStreaming = false; // Play / Simulate of a world: cells follow the streaming sources
        bool m_ShowHLODs = true;       // editing: the HLOD grid streams around the editor camera
        std::vector<glm::vec3> m_StreamingSources;

        // Frame graph
        SystemGraph m_UpdateSystems{ "Scene Update" };
        SystemGraph m_SubmitSystems{ "Scene Submit" };
        SystemFrameInput m_FrameInput;
        WorkerLocal<EntityCommandBuffer> m_CommandBuffers;
        WorkerLocal<std::vector<AssetHandle>> m_MissingAssets;

        // GPU scene registration: the renderer's instances of each mesh entity (one per drawn submesh).
        struct MeshRegistration
        {
            std::vector<uint32_t> Instances;
            bool Skinned = false;
        };
        uint64_t m_SceneID = 0;
        std::unordered_map<entt::entity, MeshRegistration> m_MeshRegistrations;
        std::vector<entt::entity> m_SkinnedMeshEntities;
        std::vector<const std::vector<glm::mat4>*> m_SkinnedBones; // SyncGpuScene: each skinned mesh's joint matrices this frame
        static constexpr uint32_t SkinnedMeshesPerTask = 8;
        // UpdateAnimators: the animators by node table (SharesNodeTable, Key: the table's first node), and where each group starts.
        struct AnimatorTask
        {
            bool SharesNodeTable = false;
            uint64_t Key = 0; // the table's first node, else the entity
            entt::entity Entity = entt::null;
        };
        static constexpr uint32_t AnimatorGroupsPerTask = 2;
        std::vector<AnimatorTask> m_AnimatorOrder;
        std::vector<uint32_t> m_AnimatorGroups;
        std::vector<entt::entity> m_PendingMeshEntities; // (re)registered at the next sync
        WorkerLocal<std::vector<entt::entity>> m_MovedEntities; // world transform changed (transform propagation)

        // Allow the Entity class to access m_Registry to add/get components
        friend class Entity;
        friend class SceneSerializer;
        friend class SceneHierarchyPanel;

        Renderer* m_renderer = nullptr;
        Renderer2D* m_renderer2D = nullptr;
    };
}
