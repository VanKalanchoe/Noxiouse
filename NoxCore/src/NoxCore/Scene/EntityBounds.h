#pragma once

#include <glm/glm.hpp>

namespace Nox
{
    class Entity;
    class Scene;

    // The world-space axis-aligned bounds of an entity's meshes and of everything below it (a prefab instance, an imported group).
    struct WorldBounds
    {
        // World space, double precision (large world coordinates, like Unreal's FBox).
        glm::dvec3 Min{ 0.0 };
        glm::dvec3 Max{ 0.0 };
        bool Valid = false;

        glm::vec3 Size() const { return glm::vec3(Max - Min); }
        glm::dvec3 Center() const { return (Min + Max) * 0.5; }
    };

    // `includeChildren` false: only this entity's own mesh (what a floor candidate is, in Snap to Floor).
    WorldBounds ComputeWorldBounds(Scene& scene, Entity entity, bool includeChildren = true);
}
