#include "SceneGraph.h"

#include <utility>

#include "Entity.h" // EntityCommandBuffer commands resolve against the complete Scene/Entity
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    namespace
    {
        // Subtrees per parallel-for chunk; a chunk's cost depends on subtree sizes, so keep chunks small.
        constexpr uint32_t SubtreesPerChunk = 16;

        struct SubtreeRoot
        {
            entt::entity Entity = entt::null;
            const glm::mat4* ParentWorldMatrix = nullptr;
            bool ParentWasDirty = false;
        };
    }

    void SceneGraph::UpdateWorldTransforms(entt::registry& registry, const std::unordered_map<UUID, entt::entity>& entityMap,
                                           WorkerLocal<EntityCommandBuffer>& commandBuffers)
    {
        // Children are found through entityMap, so its size bounds any level's item count. Pointers into component
        // storage stay valid: nothing makes structural changes while the frame graph runs.
        JobSystem& jobSystem = JobSystem::Get();
        FrameArena& arena = jobSystem.GetFrameArena();
        std::span<SubtreeRoot> frontier = arena.NewArray<SubtreeRoot>(entityMap.size());
        std::span<SubtreeRoot> nextFrontier = arena.NewArray<SubtreeRoot>(entityMap.size());
        uint32_t frontierCount = 0;

        auto appendChildren = [&](std::span<SubtreeRoot> target, uint32_t& count, const RelationshipComponent& relationship,
                                  const glm::mat4& worldMatrix, bool wasDirty)
        {
            for (UUID childUUID : relationship.Children)
            {
                auto found = entityMap.find(childUUID);
                if (found != entityMap.end() && registry.valid(found->second))
                    target[count++] = { found->second, &worldMatrix, wasDirty };
            }
        };

        {
            NOX_PROFILE_SCOPE("Update Top Levels");

            // 1. Roots (no parent; 0 means no parent). Their children form the first level.
            auto view = registry.view<TransformComponent, WorldTransformComponent>();
            for (auto entity : view)
            {
                const RelationshipComponent* relationship = registry.try_get<RelationshipComponent>(entity);
                if (relationship && relationship->Parent != 0)
                    continue;

                DirtyTransformComponent* dirty = registry.try_get<DirtyTransformComponent>(entity);
                const bool rootWasDirty = dirty && dirty->isDirty;
                auto& world = view.get<WorldTransformComponent>(entity);
                if (rootWasDirty)
                {
                    world.WorldMatrix = view.get<TransformComponent>(entity).GetTransform();
                    dirty->isDirty = false;
                }

                if (relationship)
                    appendChildren(frontier, frontierCount, *relationship, world.WorldMatrix, rootWasDirty);
            }

            // 2. Hierarchies are often lopsided (an import root holding the whole model), so splitting at the first
            // level can leave one subtree with nearly all the work. Update level by level until there are enough
            // subtrees for every worker to get balanced chunks.
            const uint32_t targetSubtrees = jobSystem.GetWorkerCount() * SubtreesPerChunk;
            while (frontierCount > 0 && frontierCount < targetSubtrees)
            {
                uint32_t nextCount = 0;
                for (uint32_t index = 0; index < frontierCount; ++index)
                {
                    const SubtreeRoot& node = frontier[index];
                    bool wasDirty = false;
                    const glm::mat4* worldMatrix = UpdateNode(registry, commandBuffers, node.Entity, *node.ParentWorldMatrix, node.ParentWasDirty, wasDirty);
                    if (!worldMatrix)
                        continue;
                    if (const RelationshipComponent* relationship = registry.try_get<RelationshipComponent>(node.Entity))
                        appendChildren(nextFrontier, nextCount, *relationship, *worldMatrix, wasDirty);
                }

                std::swap(frontier, nextFrontier);
                frontierCount = nextCount;
            }
        }

        // 3. Remaining subtrees in parallel.
        jobSystem.ParallelFor("Propagate Subtrees", frontierCount, SubtreesPerChunk,
            [&](uint32_t, uint32_t begin, uint32_t end)
            {
                for (uint32_t index = begin; index < end; ++index)
                {
                    const SubtreeRoot& subtree = frontier[index];
                    UpdateSubtree(registry, entityMap, commandBuffers, subtree.Entity, *subtree.ParentWorldMatrix, subtree.ParentWasDirty);
                }
            });
    }

    const glm::mat4* SceneGraph::UpdateNode(entt::registry& registry, WorkerLocal<EntityCommandBuffer>& commandBuffers, entt::entity entity,
                                            const glm::mat4& parentWorldMatrix, bool parentWasDirty, bool& outWasDirty)
    {
        DirtyTransformComponent* dirty = registry.try_get<DirtyTransformComponent>(entity);
        outWasDirty = parentWasDirty || (dirty && dirty->isDirty);

        WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(entity);
        if (!world)
        {
            // Added at the sync point; the entity stays dirty so its subtree is computed next frame.
            commandBuffers.Local().AddOrReplaceComponent<WorldTransformComponent>(registry.get<IDComponent>(entity).ID, WorldTransformComponent{});
            if (dirty)
                dirty->isDirty = true;
            return nullptr;
        }

        if (outWasDirty)
        {
            if (const TransformComponent* local = registry.try_get<TransformComponent>(entity))
                world->WorldMatrix = parentWorldMatrix * local->GetTransform();
        }
        if (dirty)
            dirty->isDirty = false;

        return &world->WorldMatrix;
    }

    void SceneGraph::UpdateSubtree(entt::registry& registry, const std::unordered_map<UUID, entt::entity>& entityMap,
                                   WorkerLocal<EntityCommandBuffer>& commandBuffers, entt::entity entity,
                                   const glm::mat4& parentWorldMatrix, bool parentWasDirty)
    {
        bool wasDirty = false;
        const glm::mat4* worldMatrix = UpdateNode(registry, commandBuffers, entity, parentWorldMatrix, parentWasDirty, wasDirty);
        if (!worldMatrix)
            return;

        const RelationshipComponent* relationship = registry.try_get<RelationshipComponent>(entity);
        if (!relationship)
            return;

        for (UUID childUUID : relationship->Children)
        {
            auto found = entityMap.find(childUUID);
            if (found == entityMap.end() || !registry.valid(found->second))
                continue;
            UpdateSubtree(registry, entityMap, commandBuffers, found->second, *worldMatrix, wasDirty);
        }
    }
}
