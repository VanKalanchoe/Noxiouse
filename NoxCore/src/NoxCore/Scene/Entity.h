#pragma once

#include "Components.h"
#include "Scene.h"
#include "NoxCore/Math/Math.h"

namespace Nox
{
    class Entity
    {
    public:
        Entity() = default;
        Entity(entt::entity handle, Scene* scene);
        Entity(const Entity& other) = default;
        
        void SetParent(Entity parent)
    {
        if (*this == parent)
            return;

        Entity currentParent = GetParent();
        if (currentParent == parent)
            return;

        // 1. Calculate and preserve current World Transform before changing hierarchy
        glm::mat4 worldTransform(1.0f);
        if (HasComponent<WorldTransformComponent>())
        {
            worldTransform = GetComponent<WorldTransformComponent>().WorldMatrix;
        }
        else if (HasComponent<TransformComponent>())
        {
            worldTransform = GetComponent<TransformComponent>().GetTransform();
        }

        // 2. Remove from old parent's children list
        if (currentParent)
        {
            auto& oldChildren = currentParent.GetComponent<RelationshipComponent>().Children;
            oldChildren.erase(std::remove(oldChildren.begin(), oldChildren.end(), GetUUID()), oldChildren.end());
        }

        // 3. Set new parent
        if (!HasComponent<RelationshipComponent>())
            AddComponent<RelationshipComponent>();

        auto& rel = GetComponent<RelationshipComponent>();
        if (parent)
        {
            rel.Parent = parent.GetUUID();
            if (!parent.HasComponent<RelationshipComponent>())
                parent.AddComponent<RelationshipComponent>();
            parent.GetComponent<RelationshipComponent>().Children.push_back(GetUUID());
        }
        else
        {
            rel.Parent = 0;
        }

        // 4. Convert the preserved World Transform into the *new* local space
        if (HasComponent<TransformComponent>())
        {
            glm::mat4 localTransform = worldTransform;

            if (parent)
            {
                // If the new parent has a world matrix, invert it to find relative local space
                if (parent.HasComponent<WorldTransformComponent>())
                {
                    glm::mat4 parentWorld = parent.GetComponent<WorldTransformComponent>().WorldMatrix;
                    localTransform = glm::inverse(parentWorld) * worldTransform;
                }
            }

            // Decompose the matrix back into Translation, Rotation, and Scale components
            glm::vec3 translation, rotation, scale;
            Math::DecomposeTransform(localTransform, translation, rotation, scale);

            auto& tc = GetComponent<TransformComponent>();
            tc.Translation = translation;
            tc.Rotation = rotation;
            tc.Scale = scale;

            // Mark transform as dirty so the SceneGraph updates immediately
            if (!HasComponent<DirtyTransformComponent>())
            {
                AddComponent<DirtyTransformComponent>();
            }
        }
    }
        Entity GetParent()
        {
            if (!HasComponent<RelationshipComponent>()) return {};
            UUID parentUUID = GetComponent<RelationshipComponent>().Parent;
            if (parentUUID == 0) return {};
            return m_Scene->GetEntityByUUID(parentUUID);
        }
        
        void SetWorldTransform(const glm::mat4& newWorldTransform)
        {
            auto& tc = GetComponent<TransformComponent>();
            glm::mat4 localTransform = newWorldTransform;

            // 1. If we have a parent, convert the World transform to a Local transform
            if (HasComponent<RelationshipComponent>())
            {
                UUID parentUUID = GetComponent<RelationshipComponent>().Parent;
                if (parentUUID != 0)
                {
                    // Assuming your Entity class has a reference to the scene (e.g., m_Scene)
                    Entity parent = m_Scene->GetEntityByUUID(parentUUID);
                    if (parent)
                    {
                        glm::mat4 parentWorld = parent.GetComponent<WorldTransformComponent>().WorldMatrix;
                        localTransform = glm::inverse(parentWorld) * newWorldTransform;
                    }
                }
            }

            // 2. Decompose the final local transform
            glm::vec3 translation, rotation, scale;
            Math::DecomposeTransform(localTransform, translation, rotation, scale);

            // 3. Apply it to the component (Handle rotation wrap-around smoothly)
            glm::vec3 deltaRotation = rotation - tc.Rotation;
            tc.Translation = translation;
            tc.Rotation += deltaRotation;
            tc.Scale = scale;

            // 4. Mark dirty for the SceneGraph!
            if (!HasComponent<DirtyTransformComponent>())
            {
                AddComponent<DirtyTransformComponent>();
            }
        }

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
            if (!*this) return false; // added myself
            return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
        }
        
        template<typename T>
        void RemoveComponent()
        {
            /*VK_CORE_ASSERT(HasComponent<T>(), "Entity does not have Component");*/
            
            m_Scene->m_Registry.remove<T>(m_EntityHandle);
        }
        
        // Allow if (entity) checks
        operator bool() const
        {
            return m_EntityHandle != entt::null && 
                m_Scene != nullptr && // added myself
            m_Scene->m_Registry.valid(m_EntityHandle);// added myself
        }
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