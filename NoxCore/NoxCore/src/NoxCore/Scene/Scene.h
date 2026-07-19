#pragma once
#include <entt/entt.hpp>
#include <string>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/UUID.h"
#include "NoxCore/Core/Timestep.h"
#include "NoxCore/Renderer/EditorCamera.h"

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
        void OnUpdateSimulation(Timestep ts/*, EditorCamera& camera*/);
        void OnUpdateEditor(Timestep ts/*, EditorCamera& camera*/);
        
        Entity GetEntityByUUID(UUID uuid);

        // Runs your systems (physics, rendering, etc.)
        void OnUpdate();
        
    private:
        entt::registry m_Registry;

        std::unordered_map<UUID, entt::entity> m_EntityMap;
        
        // Allow the Entity class to access m_Registry to add/get components
        friend class Entity;
        friend class SceneSerializer;
        friend class SceneHierarchyPanel;
    };
}