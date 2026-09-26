#pragma once

#include <glm/glm.hpp>

namespace Nox
{
    class Entity;
    class Scene;

    // The world-space axis-aligned bounds of an entity's meshes and of everything below it (a prefab instance, an imported group).
    struct WorldBounds
    {
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };
        bool Valid = false;

        glm::vec3 Size() const { return Max - Min; }
    };

    // `includeChildren` false: only this entity's own mesh (what a floor candidate is, in Snap to Floor).
    WorldBounds ComputeWorldBounds(Scene& scene, Entity entity, bool includeChildren = true);
}
