#pragma once

#include "Components.h"
#include "Scene.h"

namespace Nox
{
    class Entity
    {
    public:
        Entity() = default;
        Entity(entt::entity handle, Scene* scene)
            : m_EntityHandle(handle), m_Scene(scene) {}

        // Add a component: player.AddComponent<TransformComponent>();
        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            if (HasComponent<T>())
                throw std::runtime_error("Entity already has component!");
                
            return m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
        }

        // Get a component: auto& transform = player.GetComponent<TransformComponent>();
        template<typename T>
        T& GetComponent()
        {
            if (!HasComponent<T>())
                throw std::runtime_error("Entity does not have component!");
                
            return m_Scene->m_Registry.get<T>(m_EntityHandle);
        }

        // Check if component exists
        template<typename T>
        bool HasComponent()
        {
            return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
        }

        // Get the UUID of this specific entity
        UUID GetUUID() { return GetComponent<IDComponent>().ID; }

        // Allow if (entity) checks
        operator bool() const { return m_EntityHandle != entt::null; }
        operator entt::entity() const { return m_EntityHandle; }

    private:
        entt::entity m_EntityHandle{ entt::null };
        Scene* m_Scene = nullptr;
    };
}