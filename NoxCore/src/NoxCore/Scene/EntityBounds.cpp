#include "EntityBounds.h"

#include <algorithm>
#include <cfloat>
#include <functional>

#include "Components.h"
#include "Entity.h"
#include "Scene.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Renderer/Mesh.h"

namespace Nox
{
    bool ComputeLocalMeshBounds(Entity entity, glm::vec3& outMin, glm::vec3& outMax)
    {
        if (!entity || !entity.HasComponent<MeshComponent>())
            return false;
        const MeshComponent& meshComponent = entity.GetComponent<MeshComponent>();
        glm::vec3 lowest(FLT_MAX), highest(-FLT_MAX);
        bool any = false;
        auto addSubmeshes = [&](const auto& mesh)
        {
            const size_t total = mesh.GetSubMeshCount();
            const size_t first = std::min<size_t>(meshComponent.SubmeshIndex, total);
            const size_t count = meshComponent.SubmeshCount == UINT32_MAX ? total : std::max(meshComponent.SubmeshCount, 1u);
            const size_t last = std::min(first + count, total);
            for (size_t index = first; index < last; ++index)
            {
                const MeshHandle& submesh = mesh.GetSubMesh(index);
                if (!submesh.hasBounds)
                    continue;
                lowest = glm::min(lowest, submesh.boundsMin);
                highest = glm::max(highest, submesh.boundsMax);
                any = true;
            }
        };
        if (AssetManager::GetAssetType(meshComponent.Mesh) == AssetType::StaticMesh)
        {
            if (const StaticMesh* mesh = AssetManager::FindLoadedAsset<StaticMesh>(meshComponent.Mesh))
                addSubmeshes(*mesh);
        }
        else if (const Mesh* mesh = AssetManager::FindLoadedAsset<Mesh>(meshComponent.Mesh))
        {
            addSubmeshes(*mesh);
        }
        if (!any)
            return false;
        outMin = lowest;
        outMax = highest;
        return true;
    }

    WorldBounds ComputeWorldBounds(Scene& scene, Entity entity, bool includeChildren)
    {
        WorldBounds result;
        glm::dvec3 lowest(DBL_MAX), highest(-DBL_MAX);

        // The 8 corners of a submesh's local box in world space.
        auto addBox = [&](const MeshHandle& submesh, const glm::dmat4& world)
        {
            if (!submesh.hasBounds)
                return;
            for (int corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 local((corner & 1) ? submesh.boundsMax.x : submesh.boundsMin.x,
                                      (corner & 2) ? submesh.boundsMax.y : submesh.boundsMin.y,
                                      (corner & 4) ? submesh.boundsMax.z : submesh.boundsMin.z);
                const glm::dvec3 point = glm::dvec3(world * glm::dvec4(glm::dvec3(local), 1.0));
                lowest = glm::min(lowest, point);
                highest = glm::max(highest, point);
                result.Valid = true;
            }
        };

        std::function<void(Entity)> visit = [&](Entity current)
        {
            if (current.HasComponent<MeshComponent>() && current.HasComponent<WorldTransformComponent>())
            {
                const MeshComponent& meshComponent = current.GetComponent<MeshComponent>();
                const glm::dmat4& world = current.GetComponent<WorldTransformComponent>().WorldMatrix;

                auto addSubmeshes = [&](const auto& mesh)
                {
                    const size_t total = mesh.GetSubMeshCount();
                    const size_t first = std::min<size_t>(meshComponent.SubmeshIndex, total);
                    const size_t count = meshComponent.SubmeshCount == UINT32_MAX ? total : std::max(meshComponent.SubmeshCount, 1u);
                    const size_t last = std::min(first + count, total);
                    for (size_t index = first; index < last; ++index)
                        addBox(mesh.GetSubMesh(index), world);
                };

                if (AssetManager::GetAssetType(meshComponent.Mesh) == AssetType::StaticMesh)
                {
                    if (const StaticMesh* mesh = AssetManager::FindLoadedAsset<StaticMesh>(meshComponent.Mesh))
                        addSubmeshes(*mesh);
                }
                else if (const Mesh* mesh = AssetManager::FindLoadedAsset<Mesh>(meshComponent.Mesh))
                {
                    addSubmeshes(*mesh);
                }
            }

            if (!includeChildren || !current.HasComponent<RelationshipComponent>())
                return;
            for (UUID childID : current.GetComponent<RelationshipComponent>().Children)
            {
                if (Entity child = scene.GetEntityByUUID(childID))
                    visit(child);
            }
        };
        visit(entity);

        if (result.Valid)
        {
            result.Min = lowest;
            result.Max = highest;
        }
        return result;
    }
}
