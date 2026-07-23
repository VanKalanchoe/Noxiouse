#pragma once

#include "Components.h"
#include "Scene.h"

namespace Nox
{
    class Entity
    {
    public:
        Entity() = default;
        Entity(entt::entity handle, Scene* scene);
        Entity(const Entity& other) = default;

        // Add a component: player.AddComponent<TransformComponent>();
        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            if (HasComponent<T>())
                throw std::runtime_error("Entity already has component!");
                
            return m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
        }

        template<typename T, typename... Args>
        T& AddOrReplaceComponent(Args&&... args)
        {
            T& component = m_Scene->m_Registry.emplace_or_replace<T>(m_EntityHandle, std::forward<Args>(args)...);
            m_Scene->OnComponentAdded<T>(*this, component);
            return component;
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
        
        template<typename T>
        void RemoveComponent()
        {
            /*VK_CORE_ASSERT(HasComponent<T>(), "Entity does not have Component");*/
            
            m_Scene->m_Registry.remove<T>(m_EntityHandle);
        }
        
        // Allow if (entity) checks
        operator bool() const { return m_EntityHandle != entt::null; }
        operator entt::entity() const { return m_EntityHandle; }
        operator uint32_t() const { return (uint32_t)m_EntityHandle; }
        
        // Get the UUID of this specific entity
        UUID GetUUID() { return GetComponent<IDComponent>().ID; };
        const std::string& GetName() { return GetComponent<TagComponent>().Tag; };
        
        bool operator==(const Entity& other) const
        {
            return m_EntityHandle == other.m_EntityHandle && m_Scene == other.m_Scene;
        }
        bool operator!=(const Entity& other) const
        {
            return !(*this == other);
        }

    private:
        entt::entity m_EntityHandle{ entt::null };
        Scene* m_Scene = nullptr;
    };
}