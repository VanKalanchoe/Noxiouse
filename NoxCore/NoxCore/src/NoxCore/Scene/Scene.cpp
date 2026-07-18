#include "Scene.h"

#include "Entity.h"
#include "Components.h"

namespace Nox
{
    Entity Scene::CreateEntity(const std::string& name)
    {
        return CreateEntityWithUUID(UUID(), name); // Generate a new random UUID
    }
    
    Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name)
    {
        // 1. Create the raw EnTT ID
        entt::entity entityHandle = m_Registry.create();
        
        // 2. Wrap it in our Handle class
        Entity entity = { entityHandle, this };
        
        // 3. Add the mandatory components
        entity.AddComponent<IDComponent>(uuid);
        entity.AddComponent<TagComponent>(name.empty() ? "Entity" : name);
        entity.AddComponent<TransformComponent>(); // Most engines assume everything has a transform
        
        m_EntityMap[uuid] = entityHandle;
        
        return entity;
    }
    
    Entity Scene::GetEntityByUUID(UUID uuid)
    {
        // Check if it exists in the map
        if (m_EntityMap.find(uuid) != m_EntityMap.end())
        {
            // Return the reconstructed Entity wrapper
            return { m_EntityMap.at(uuid), this }; 
        }

        // Return a null entity if the UUID wasn't found
        return { entt::null, this }; 
    }

    void Scene::OnUpdate()
    {
        
    }
}
