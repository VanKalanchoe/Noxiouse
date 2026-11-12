#pragma once
#include <set>
#include <entt/entt.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <chrono>
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
        void OnSimulationStart(const glm::dvec3* streamingSource = nullptr);
        void OnSimulationStop();

        void OnUpdateRuntime(Timestep ts);
        void OnUpdateSimulation(Timestep ts, EditorCamera& camera);
        void OnUpdateEditor(Timestep ts, EditorCamera& camera);
        void OnViewportResize(uint32_t width, uint32_t height);

        Entity DuplicateEntity(Entity entity);

        Entity FindEntityByName(std::string_view name);
        Entity GetEntityByUUID(UUID uuid);

        Entity GetPrimaryCameraEntity();
        // Unity's activeInHierarchy: false when the entity or one above it is switched off (InactiveComponent).
        bool IsActiveInHierarchy(entt::entity entity) const { return !m_Registry.all_of<InactiveInHierarchyComponent>(entity); }

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
        Entity Instantiate(AssetHandle prefab, const glm::dvec3& position);
        // Destroys the entity at the end of the script update (a script may destroy itself: nothing in the middle of a frame's
        // script loop is removed).
        void QueueDestroy(Entity entity);

        // Level streaming (Unreal's): a level is another scene file (a scene asset) loaded into this running scene, its entities
        // marked with LevelComponent, and unloaded again (its entities destroyed). It is read on a thread and its entities created a
        // slice a frame (SceneLoader); its scripts, particles, timelines and physics start once the whole level is in, as if the scene
        // had begun with them, and it is drawn once its meshes are all in too (StreamedLevel::Shown). Both happen at the start of the next frame (UpdateLevelStreaming), so a script may call them. Play and
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

        // Sub-levels (Unreal's Levels window, Backlog L1): other scenes (scene assets) edited together with this one, the persistent
        // level. While editing, their entities are in this scene, each root marked with LevelComponent (an entity belongs to its
        // root's level: a tree is never split between files), and a save writes every sub-level's entities into its own file; this
        // scene's file lists the sub-levels only. In Play an Always Loaded sub-level is in from the start, the others are loaded by
        // a volume or a script (LoadLevel) -- both as edited, unsaved changes included. Not in a world (its cells split it already).
        struct SubLevel
        {
            AssetHandle Level = 0;
            bool AlwaysLoaded = true; // else streamed: Level Streaming Volumes or Scene.LoadLevel bring it in
            bool Visible = true;      // editor: hidden, its entities are kept aside (nothing draws, lights, picks or simulates them)
            bool Locked = false;      // editor: its entities cannot be selected
            bool Loaded = false;      // editor: its entities were read (only then does a save write its file)
            // Level transform (Unreal's: translation and yaw): where the level's own space sits in this scene. Applied to its root
            // entities when it is read, taken back out when it is saved -- its file keeps the level where it was built.
            glm::dvec3 Offset{ 0.0 }; // world units
            float Yaw = 0.0f;         // radians, around the up axis
            glm::dmat4 GetTransform() const;
            bool HasTransform() const { return Offset != glm::dvec3(0.0) || Yaw != 0.0f; }
        };
        const std::vector<SubLevel>& GetSubLevels() const { return m_SubLevels; }
        const SubLevel* FindSubLevel(AssetHandle level) const;
        // Editor: lists a scene asset as a sub-level and reads its entities into this scene. False (logged) when it cannot be: not a
        // scene, this scene itself, listed already, a world, or its entity IDs are in this scene already (a copy of it).
        bool AddSubLevel(AssetHandle level, bool alwaysLoaded = true);
        // Editor: unlisted, its entities leave this scene (what was not saved into its file is lost).
        void RemoveSubLevel(AssetHandle level);
        void SetSubLevelAlwaysLoaded(AssetHandle level, bool alwaysLoaded);
        void SetSubLevelVisible(AssetHandle level, bool visible);
        void SetSubLevelLocked(AssetHandle level, bool locked);
        // Editor: moves the level's entities by the change from its old transform to this one (a locked level is left alone).
        void SetSubLevelTransform(AssetHandle level, const glm::dvec3& offset, float yaw);
        // Editor: where new root entities go (0: this scene). Unreal's current level.
        AssetHandle GetCurrentLevel() const { return m_CurrentLevel; }
        void SetCurrentLevel(AssetHandle level);
        // The level an entity is saved into: its root's (0: this scene).
        AssetHandle GetEntityLevel(Entity entity) const;
        bool IsEntityLocked(Entity entity) const;
        // Editor: the entities' roots, with everything under them, into a level (0: this scene). Locked levels are left alone.
        void MoveToLevel(const std::vector<Entity>& entities, AssetHandle level);
        // Editor, once the scene file is read: every listed sub-level's entities read into this scene (hidden ones kept aside).
        void LoadSubLevelsForEditing();
        // The root entities of a sub-level, in this scene or kept aside while it is hidden (the scene they are in: outScene).
        std::vector<Entity> GetSubLevelRoots(AssetHandle level, Scene*& outScene);

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
        // Loaded here: in the world (activated); Loading: any stage before.
        LevelState GetWorldCellState(const WorldCellCoord& cell) const;
        // Unreal's runtime cell states (§5.13.3): Loaded -- built in a scene of its own (nothing draws or simulates it) and its assets
        // requested; Activating -- its entities going into the world, a slice a frame; Activated -- in the world.
        enum class WorldCellStage { Unloaded, Loading, Loaded, Activating, Activated };
        WorldCellStage GetWorldCellStage(const WorldCellCoord& cell) const;
        // Unreal's IsStreamingCompleted: every world cell whose content is within `range` (world units) of a point has reached `stage`
        // (Loaded: also those activating or activated; Activated: in the world). E.g. a teleport waits for its target to be Loaded.
        bool IsWorldStreamingCompleted(const glm::dvec3& point, float range, WorldCellStage stage) const;
        // What the cells activated since Play / Simulate began cost (§5.13.4; Unreal's stat streaming): reading and parsing their
        // files on workers, building them and copying them into the world on the main thread, waiting for their assets, and the
        // time from a cell being wanted to it being in the world.
        struct WorldStreamingCounters
        {
            uint64_t CellsActivated = 0;
            uint64_t EntitiesActivated = 0;
            uint64_t CellsCooked = 0;      // of them, read from their cooked files (§5.13.4)
            double ReadMs = 0.0;           // workers: read, parsed, built
            double BuildMs = 0.0;          // main thread: a worker's result taken, or a session snapshot copied
            double ActivateMs = 0.0;       // main thread
            double AssetWaitSeconds = 0.0;
            double LatencySeconds = 0.0;   // summed: divide by CellsActivated
            double MaxLatencySeconds = 0.0;
        };
        const WorldStreamingCounters& GetWorldStreamingCounters() const { return m_WorldStreamingCounters; }
        // includeLoading: also the cells on their way in (and the loaded ones not activated).
        std::vector<WorldCellCoord> GetLoadedWorldCells(bool includeLoading, int32_t grid = WorldGrid::WorldGridIndex) const;
        bool IsWorldStreamingBusy() const; // a cell is loading, activating or about to unload
        // The indexed cells whose content is within `range` (world units) of a point, horizontally.
        std::vector<WorldCellCoord> WorldCellsAround(const glm::dvec3& point, float range, int32_t grid = WorldGrid::WorldGridIndex) const;
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
        // Folders the outliner shows folded (Unreal's actor folder bIsInitiallyExpanded): saved with the scene, so they stay folded
        // when it is opened again, after an undo, or in another session.
        bool IsFolderCollapsed(const std::string& path) const { return m_CollapsedFolders.contains(path); }
        void SetFolderCollapsed(const std::string& path, bool collapsed);
        const std::set<std::string>& GetCollapsedFolders() const { return m_CollapsedFolders; }

        // Audio sources (AudioSourceComponent) in Play / Simulate: plays the entity's clip (from the start; once loaded, if it is not
        // yet), or stops it with a fade. No-ops outside Play / Simulate and on switched-off entities.
        void PlayAudioSource(Entity entity);
        void StopAudioSource(Entity entity, float fadeOutSeconds = 0.0f);
        bool IsAudioSourcePlaying(Entity entity) const;
        bool IsAudioRunning() const { return m_AudioRunning; } // Play or Simulate
        // A sound (a clip or an audio graph) played once (Audio.PlayOneShot): at a point (3D, the default Min / Max Distance) or as it is
        // (2D); played as soon as it is loaded. Stopped with the rest when Play ends.
        void PlayOneShot(AssetHandle sound, const glm::dvec3& position, float volume, bool spatial, uint64_t bus);
        // The same with every voice setting (distance curve, pitch, ...): Impact Sound and particle sounds.
        void PlayOneShot(AssetHandle sound, const AudioPlayDesc& desc);
        // An Audio Source's audio graph parameter (AudioSourceComponent.SetFloat / SetInt / SetBool): kept for its next Play this
        // session, and sent to its sound if it plays; a trigger parameter (Trigger) fires on the playing sound.
        void SetAudioSourceParameter(Entity entity, const std::string& name, const NodeGraphValue& value);
        void FireAudioSourceParameter(Entity entity, const std::string& name);
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
        // Active / inactive (InactiveComponent): an entity switched, or moved under another parent, is queued; ApplyActiveChanges
        // brings InactiveInHierarchyComponent up to date below it and re-registers what changed (meshes, physics).
        void OnActiveChanged(entt::registry& registry, entt::entity entity);
        void ApplyActiveChanges();
        void RefreshActiveTree(entt::entity root);
        // EnTT signals of the 3D physics components: added or removed while the physics scene runs (Duplicate, a script's
        // AddComponent, the properties panel), the entity's body / character is made again after the update systems.
        void OnPhysics3DComponentChanged(entt::registry& registry, entt::entity entity);
        void SyncPendingPhysics3D();

        // Level streaming, at the start of a running frame: the volumes against the streaming source, the unloads, then the loads for
        // LevelStreamingBudgetMs.
        void UpdateLevelStreaming(const glm::dvec3* source, float deltaTime);
        struct StreamedLevel;
        void FinishLevelLoad(StreamedLevel& streamed, const std::vector<UUID>& created, const std::vector<UUID>& heldPhysics);
        void DestroyLevelEntities(AssetHandle level);
        // World partition: the copy of an edited world turned into in-memory cells and the cells around the sources loaded (Play /
        // Simulate start), then every frame the cells near a source wanted, the ones every source has left unloaded.
        void BeginWorldStreaming(const glm::dvec3* source);
        // `composedSources`: the sources' places from their transforms up the parents (before the first update: no world transforms).
        void UpdateWorldStreaming(const glm::dvec3* source, bool composedSources = false);
        // A streaming source: where, how far (times the grid's loading range), and whether the cells it reaches are activated or
        // only loaded (WorldStreamingSourceComponent; the camera activates).
        struct StreamingSource
        {
            glm::dvec3 Position{ 0.0 };
            float RangeScale = 1.0f;
            bool Activate = true;
        };
        void CollectStreamingSources(const glm::dvec3* source, std::vector<StreamingSource>& outSources, bool composed);
        // The cells of every grid (onlyGrid -1) or of one near the sources wanted -- activated where an activating source reaches them,
        // else loaded -- the ones all of them have left unloaded.
        void StreamWorldCells(const std::vector<StreamingSource>& sources, int32_t onlyGrid);
        void UpdateEditorHLODs(const glm::dvec3& camera);
        // HLODs whose cell is loaded and drawn are hidden (left out of the GPU scene), the others shown (SyncGpuScene).
        void UpdateHLODVisibility();
        // Streamed levels complete and ready are shown (SyncGpuScene, before the HLODs: no frame shows neither a cell nor its HLOD).
        void UpdateStreamedLevelVisibility();
        // The levels (asset handles, world cell IDs) with a mesh entity not registered with all its assets yet.
        void CollectLevelsNotReady(std::unordered_set<uint64_t>& outLevels) const;
        glm::dvec3 ComposedWorldPosition(Entity entity);
        void QueueWorldCellLoad(const WorldCellCoord& cell, bool activate = true);
        // A cell built from its file (or a Play session's text) into a scene of its own: the loader, a slice a frame.
        void StageWorldCell(StreamedLevel& streamed);
        // One cell's next step: building it (staged), then -- its target activated, its assets in or waited for long enough -- its
        // entities copied into this scene. Activated: complete (the caller finishes it, FinishLevelLoad).
        enum class CellStep { Busy, Activated, Failed };
        CellStep StepWorldCell(StreamedLevel& streamed, double budgetMs, float deltaTime, bool waitForAssets);
        bool ActivateWorldCellEntities(StreamedLevel& streamed, double budgetMs); // true once every staged entity is in
        // Out of the world: a cell activated (or being activated) whose target became Loaded, or that is unloaded.
        void RemoveWorldCellFromWorld(StreamedLevel& streamed);
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
        // `depth`: how deep in Sub-Timeline tracks (a timeline nesting itself stops at 8).
        void ApplyTimeline(Entity player, const Timeline& timeline, float time, bool cameraCuts = false, int depth = 0);
        // The event names the playhead passed from `from` to `to`, the Sub-Timelines' included.
        void CollectTimelineEvents(const Timeline& timeline, float from, float to, bool wrapped, std::vector<std::string>& out, int depth = 0);
        void SubmitLights();
        void Submit2D();
        // Particle emitters (ParticleSystem.h): simulated every frame the scene steps (the editor's too), restarted by Play / Simulate.
        void StartParticles();
        void UpdateParticles();
        void SubmitParticles();
        // Audio (docs/Audio_Architecture_Plan_2026.md): Play / Simulate start the Play On Start sources and stop every game voice at
        // the end; each frame the listener (an AudioListenerComponent, else the camera) and the playing sources' positions and
        // settings go to the AudioEngine, after the late update.
        void StartAudio();
        void StopAudio();
        void UpdateAudio(const glm::dmat4& cameraWorld);
        // Sounds from the frame's events: Impact Sounds from the contacts physics began, particle burst sounds.
        void PlayEventSounds();
        uint32_t m_EventSoundRandom = 0x2545F491u;
        void OnAudioSourceDestroyed(entt::registry& registry, entt::entity entity);

        const std::vector<glm::mat4>* GetBoneTransforms(entt::entity entity, const glm::dmat4& meshWorld);

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
        std::set<std::string> m_CollapsedFolders;
        bool m_IsRunning = false;
        bool m_TimelinesRunning = false; // Play or Simulate
        bool m_AudioRunning = false;     // Play or Simulate
        // One-shots asked for while their clip was loading (UpdateAudio plays them once it is in).
        struct PendingOneShot
        {
            AssetHandle Sound = 0;
            AudioPlayDesc Desc;
        };
        std::vector<PendingOneShot> m_PendingOneShots;
        bool StartOneShot(PendingOneShot& shot); // false: its sound is still loading
        // 3D one-shots playing: Audio Volumes keep filtering them while they play.
        struct OneShotVoice
        {
            AudioVoiceHandle Voice = 0;
            glm::dvec3 Position{ 0.0 };
            AudioVoiceEnvironment Environment;
        };
        std::vector<OneShotVoice> m_OneShotVoices;
        // Where the Audio Volumes want a 3D sound at `position` heard by the listener now, and how fast it gets there.
        struct AudioEnvironment
        {
            AudioVoiceEnvironment Target;
            float FadeTime = 0.5f;
        };
        AudioEnvironment AudioEnvironmentAt(const glm::dvec3& position) const;
        glm::dvec3 m_AudioListenerPosition{ 0.0 };
        // The acoustic scene (Steam Audio, SpatialAudio.h): the static meshes placed in it, by entity id; re-placed when one moves,
        // its mesh or materials change, removed when it goes.
        struct AcousticEntry
        {
            AssetHandle Mesh = 0;
            uint32_t FirstSubmesh = 0;
            uint32_t SubmeshCount = 0;
            glm::dmat4 World{ 1.0 };
            std::vector<AcousticMaterial> Materials;
            bool Placed = false;
            uint64_t Seen = 0;
        };
        std::unordered_map<uint64_t, AcousticEntry> m_AcousticEntries;
        std::unordered_map<uint64_t, AssetHandle> m_AcousticBatches; // Acoustic Probe Volumes in the simulation: entity -> its baked data
        uint64_t m_AcousticFrame = 0;
        uint32_t m_AcousticMaterialVersion = 0;
        void UpdateAcousticScene();
    public:
        // The static meshes as the acoustic scene takes them (Acoustic Probe Volumes are baked against them). Returns how many were
        // left out because their mesh or materials are still loading.
        uint32_t GatherAcousticInstances(std::vector<AcousticInstance>& out);
    private:
        bool ResolveAcousticMaterials(entt::entity entity, std::vector<AcousticMaterial>& outMaterials) const;
        std::chrono::steady_clock::time_point m_AudioLastUpdate{}; // (the volumes' blending runs on real time: also while paused)
        std::vector<UUID> m_PendingDestroy;
        std::vector<UUID> m_PendingScriptStart;
        std::unordered_set<UUID> m_PendingPhysics3D;
        std::vector<entt::entity> m_PendingActiveChanges;
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
            bool Loaded = false;                // in the scene (a cell: activated)
            bool Failed = false;                // refused or unreadable: volumes leave it until the source has left them
            bool UnloadQueued = false;
            float OutsideTime = 0.0f;           // seconds no volume naming it has held the streaming source
            // Drawn (Unreal's ULevel::bRequireFullVisibilityToRender): until then its meshes are registered hidden, and once it is complete
            // and every one of them is registered with its assets (or MaxLevelShowWaitSeconds after completion), all show in one frame.
            bool Shown = false;
            std::chrono::steady_clock::time_point CompletedAt{};

            // World cells (§5.13.3): built into a scene of their own first (Loaded), then copied into this one (Activated).
            bool Activate = true;                 // the target: in the world, or only loaded
            bool Empty = false;                   // nothing to load (no file yet, or emptied in the editor): Loaded at once
            Ref<Scene> Staged;                    // loaded, not (yet) activated: nothing draws or simulates it
            std::vector<UUID> StagedEntities;     // in file order
            std::vector<AssetHandle> Assets;      // what the staged cell references, requested once it is built
            size_t AssetsReady = 0;               // how many of them have finished (in order)
            float AssetWait = 0.0f;               // seconds its activation has waited for them
            size_t ActivateNext = 0;              // the next staged entity copied into the world
            std::vector<UUID> ActivatedEntities;  // copied so far
            // Its costs (WorldStreamingCounters), from the request that staged it.
            std::chrono::steady_clock::time_point Requested{};
            double ReadMs = 0.0, BuildMs = 0.0, ActivateMs = 0.0;
            uint32_t BuildSteps = 0, ActivateSteps = 0;
            bool Cooked = false;
        };
        std::vector<StreamedLevel> m_Levels;
        WorldPartition m_World;
        // Play / Simulate of an edited world: the cells the editor had loaded stay in the world as the session begins, and this is
        // each as it was then (its entities, every component, in a scene of their own): streamed out and back, it is built from
        // here -- unsaved changes included -- with a copy, not a text round trip.
        struct WorldCellSnapshot
        {
            Ref<Scene> Entities;
            std::vector<UUID> Order; // roots, each before its children
        };
        std::map<WorldCellCoord, WorldCellSnapshot> m_WorldSnapshots;
        // These entities of another scene copied into this one (same IDs, every component).
        void CopyEntitiesFrom(Scene& source, const std::vector<UUID>& ids);

        // Sub-levels (L1).
        std::vector<SubLevel> m_SubLevels;
        AssetHandle m_CurrentLevel = 0;
        bool m_SubLevelEditing = false; // the editor's scene: new roots go into the current level, a detached entity keeps its level
        // A hidden sub-level's entities, kept aside in a scene of their own (Order: roots, each before its children).
        struct SubLevelStash
        {
            Ref<Scene> Entities;
            std::vector<UUID> Order;
            std::vector<UUID> Roots;
        };
        std::map<AssetHandle, SubLevelStash> m_HiddenSubLevels;
        // Play / Simulate: a streamed sub-level as edited (its entities, every component), loaded from here by LoadLevel.
        std::map<AssetHandle, SubLevelStash> m_LevelSnapshots;
        bool ReadSubLevelEntities(AssetHandle level); // the level's file read into this scene, its roots tagged
        void StartLevelLoad(StreamedLevel& streamed); // from the editor's copy (m_LevelSnapshots) or its file
        void LoadAlwaysLoadedSubLevels();
        void HideSubLevel(AssetHandle level);
        void ShowSubLevel(AssetHandle level);
        void TagLevelTree(Entity root, AssetHandle level); // LevelComponent on the root and everything under it (0: removed)
        // Roots (no parent) moved by `transform` in world space (their trees follow).
        static void TransformRoots(const std::vector<Entity>& roots, const glm::dmat4& transform);
        // The trees of the given roots as IDs, each before its children.
        static std::vector<UUID> TreeOrder(Scene& scene, const std::vector<Entity>& roots);
        // A built cell's references requested (Loaded: in when it activates).
        void RequestWorldCellAssets(StreamedLevel& streamed);
        bool m_WorldStreaming = false; // Play / Simulate of a world: cells follow the streaming sources
        WorldStreamingCounters m_WorldStreamingCounters;
        bool m_ShowHLODs = true;       // editing: the HLOD grid streams around the editor camera
        std::vector<StreamingSource> m_StreamingSources;

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
            bool Morphed = false; // an instance has morph targets: its weights are handed over every frame
            bool Parameterized = false; // material parameters (MaterialComponent::Parameters): its records are refreshed every frame
            bool Hidden = false; // its streamed level is not shown yet (StreamedLevel::Shown)
        };
        uint64_t m_SceneID = 0;
        std::unordered_map<entt::entity, MeshRegistration> m_MeshRegistrations;
        std::vector<entt::entity> m_SkinnedMeshEntities;
        std::vector<entt::entity> m_MorphedMeshEntities;
        std::vector<entt::entity> m_ParameterizedMeshEntities;
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
