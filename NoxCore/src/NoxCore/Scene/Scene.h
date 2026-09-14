#pragma once
#include <entt/entt.hpp>
#include <string>
#include <unordered_set>
#include <utility>
#include <box2d/id.h>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/UUID.h"
#include "NoxCore/Core/Timestep.h"
#include "NoxCore/Renderer/EditorCamera.h"
#include "NoxCore/Renderer/Renderer.h"

namespace Nox
{
    class Entity; // Forward declaration
    
    class Scene : public Asset
    {
    public:
        Scene() = default;
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
        
        void SetRenderer(Renderer* renderer) { m_renderer = renderer; }
        void SetRenderer2D(Renderer2D* renderer) { m_renderer2D = renderer; }

        // Every asset handle this scene's components reference - the roots for unloading unused assets.
        void CollectAssetReferences(std::unordered_set<AssetHandle>& outHandles);

        // True once after entities were destroyed since the last call, i.e. assets may have become unused.
        bool ConsumeAssetReferencesChanged() { return std::exchange(m_AssetReferencesChanged, false); }
    private:
        template<typename T>
        void OnComponentAdded(Entity entity, T& component);
        
        void OnPhysics2DStart();
        void OnPhysics2DStop();

        void RenderScene(EditorCamera& camera);
        void UpdateAnimators(Timestep ts);
        const std::vector<glm::mat4>* GetBoneTransforms(entt::entity entity, const glm::mat4& meshWorld);

        std::string MakeUniqueDuplicateName(const std::string& baseName);
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
        
        // Allow the Entity class to access m_Registry to add/get components
        friend class Entity;
        friend class SceneSerializer;
        friend class SceneHierarchyPanel;
        
        Renderer* m_renderer = nullptr;
        Renderer2D* m_renderer2D = nullptr;
    };
}
