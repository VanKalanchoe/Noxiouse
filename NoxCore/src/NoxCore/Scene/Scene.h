#pragma once
#include <entt/entt.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <box2d/id.h>

#include "EntityCommandBuffer.h"
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

    class Scene : public Asset
    {
    public:
        Scene();
        ~Scene() = default;

        static Ref<Scene> Copy(Ref<Scene> other);

        virtual AssetType GetType() const { return AssetType::Scene;};

        // Creates an entity, auto-generates a UUID, and assigns a name
        Entity CreateEntity(const std::string& name = std::string());
        // Creates an entity with a specific UUID (crucial for loading saved games!)
        Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
        void DestroyEntity(Entity entity);

        void OnRuntimeStart();
        void OnRuntimeStop();

        void OnSimulationStart();
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

        void SetRenderer(Renderer* renderer);
        void SetRenderer2D(Renderer2D* renderer) { m_renderer2D = renderer; }

        // Every asset handle this scene's components reference - the roots for unloading unused assets.
        void CollectAssetReferences(std::unordered_set<AssetHandle>& outHandles);

        // True once after entities were destroyed since the last call, i.e. assets may have become unused.
        bool ConsumeAssetReferencesChanged() { return std::exchange(m_AssetReferencesChanged, false); }

        // Writes the scene's system graphs as GraphViz DOT files (SceneUpdate.dot, SceneSubmit.dot) into directory.
        bool DumpSystemGraphs(const std::filesystem::path& directory);
    private:
        template<typename T>
        void OnComponentAdded(Entity entity, T& component);

        void OnPhysics2DStart();
        void OnPhysics2DStop();

        // Frame graph (§5.3): Game Update systems, then (after BeginScene on the main thread) the submission systems.
        void RegisterSystems();
        void RunUpdateSystems(Timestep ts, bool stepPhysics, bool stepAnimation);
        void RunSubmitSystems();
        // Main thread after each graph: deferred structural changes, then loads of assets the systems found unloaded.
        void ApplySyncPoint();
        // Main thread after the update systems (§5.5): brings the renderer's GPU scene up to date with what changed.
        void SyncGpuScene();
        // EnTT signals of the components that decide a mesh entity's GPU instances.
        void OnMeshEntityChanged(entt::registry& registry, entt::entity entity);

        // Systems (run as tasks).
        void UpdatePhysics2D();
        void UpdateAnimators();
        void SubmitLights();
        void Submit2D();

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

        b2WorldId m_PhysicsWorldID;
        bool m_IsRunning = false;
        bool m_IsPaused = false;
        int m_StepFrames = 0;
        bool m_AssetReferencesChanged = false;

        std::unordered_map<UUID, entt::entity> m_EntityMap;

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
