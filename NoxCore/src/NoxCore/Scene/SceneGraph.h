#pragma once

#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <unordered_map>
#include "Components.h"
#include "EntityCommandBuffer.h"
#include "NoxCore/Tasks/WorkerLocal.h"

namespace Nox
{
    class SceneGraph
    {
    public:
        // Runs as a task (Scene's "Transform Propagation" system): the top levels serially until there are enough
        // subtrees to balance across the workers, then those subtrees in parallel. Subtrees are disjoint, so their
        // WorldTransform/DirtyTransform writes never overlap; the only structural change (a missing
        // WorldTransformComponent) is deferred into the command buffers.
        static void UpdateWorldTransforms(entt::registry& registry, const std::unordered_map<UUID, entt::entity>& entityMap,
                                          WorkerLocal<EntityCommandBuffer>& commandBuffers);

    private:
        // Updates one entity's world transform; returns it (null if the entity has no WorldTransformComponent yet).
        static const glm::mat4* UpdateNode(entt::registry& registry, WorkerLocal<EntityCommandBuffer>& commandBuffers, entt::entity entity,
                                           const glm::mat4& parentWorldMatrix, bool parentWasDirty, bool& outWasDirty);
        static void UpdateSubtree(entt::registry& registry, const std::unordered_map<UUID, entt::entity>& entityMap,
                                  WorkerLocal<EntityCommandBuffer>& commandBuffers, entt::entity entity,
                                  const glm::mat4& parentWorldMatrix, bool parentWasDirty);
    };
}
