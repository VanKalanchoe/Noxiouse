#pragma once

#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <unordered_map>
#include "Components.h"

namespace Nox
{
    class SceneGraph
    {
    public:
        // Pass both registry and the UUID lookup map to keep it decoupled from Scene
        static void UpdateWorldTransforms(entt::registry& registry, const std::unordered_map<UUID, entt::entity>& entityMap)
        {
            auto view = registry.view<TransformComponent, WorldTransformComponent>();

            for (auto entity : view)
            {
                auto* rel = registry.try_get<RelationshipComponent>(entity);

                // Process Roots (Entities without a parent). In your struct, 0 means no parent.
                if (!rel || rel->Parent == 0)
                {
                    bool isSelfDirty = registry.any_of<DirtyTransformComponent>(entity);
                    auto& localTransform = view.get<TransformComponent>(entity);
                    auto& worldTransform = view.get<WorldTransformComponent>(entity);

                    if (isSelfDirty)
                    {
                        worldTransform.WorldMatrix = localTransform.GetTransform();
                        registry.remove<DirtyTransformComponent>(entity); // Clear dirty flag
                    }

                    // Traverse down hierarchy if entity has children
                    if (rel && !rel->Children.empty())
                    {
                        UpdateChildren(registry, entityMap, rel->Children, worldTransform.WorldMatrix, isSelfDirty);
                    }
                }
            }
        }

    private:
        static void UpdateChildren(entt::registry& registry, 
                                   const std::unordered_map<UUID, entt::entity>& entityMap, 
                                   const std::vector<UUID>& children, 
                                   const glm::mat4& parentWorldMatrix, 
                                   bool parentWasDirty)
        {
            for (UUID childUUID : children)
            {
                // Fast translation from UUID to entt::entity
                auto it = entityMap.find(childUUID);
                if (it == entityMap.end()) continue;
                
                entt::entity child = it->second;
                if (!registry.valid(child)) continue;

                bool isChildLocallyDirty = registry.any_of<DirtyTransformComponent>(child);
                bool needsUpdate = parentWasDirty || isChildLocallyDirty;

                auto& childWorld = registry.get_or_emplace<WorldTransformComponent>(child);

                if (needsUpdate)
                {
                    auto& childLocal = registry.get<TransformComponent>(child);
                    
                    // World = ParentWorld * Local
                    childWorld.WorldMatrix = parentWorldMatrix * childLocal.GetTransform();

                    if (isChildLocallyDirty)
                        registry.remove<DirtyTransformComponent>(child); // Clear dirty flag
                }

                auto* childRel = registry.try_get<RelationshipComponent>(child);
                if (childRel && !childRel->Children.empty())
                {
                    UpdateChildren(registry, entityMap, childRel->Children, childWorld.WorldMatrix, needsUpdate);
                }
            }
        }
    };
}