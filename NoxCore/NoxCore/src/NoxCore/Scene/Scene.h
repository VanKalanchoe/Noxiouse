#pragma once
#include <entt/entt.hpp>
#include <string>

#include "NoxCore/Core/UUID.h"

namespace Nox
{
    class Entity; // Forward declaration
    
    class Scene
    {
    public:
        Scene() = default;
        ~Scene() = default;
        
        // Creates an entity, auto-generates a UUID, and assigns a name
        Entity CreateEntity(const std::string& name = std::string());
        
        // Creates an entity with a specific UUID (crucial for loading saved games!)
        Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
        
        Entity GetEntityByUUID(UUID uuid);

        // Runs your systems (physics, rendering, etc.)
        void OnUpdate();
        
    private:
        entt::registry m_Registry;

        std::unordered_map<UUID, entt::entity> m_EntityMap;
        
        // Allow the Entity class to access m_Registry to add/get components
        friend class Entity;
    };
}