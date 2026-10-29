#pragma once

#include <glm/glm.hpp>

namespace Nox::Math
{
    bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::vec3& outRotation, glm::vec3& outScale);
    // A world transform (large world coordinates: the translation in double precision); rotation and scale need no more than
    // float.
    bool DecomposeTransform(const glm::dmat4& transform, glm::dvec3& outTranslation, glm::vec3& outRotation, glm::vec3& outScale);

    // Large world coordinates (Engine_Architecture_Plan_2026.md §5.14a): world positions are doubles on the CPU; the GPU works in
    // single precision relative to the renderer's origin, which follows the camera. A world transform there: the difference of
    // the translations is taken in double precision, so it is exact however far from the world's origin the two are.
    inline glm::mat4 RelativeTo(const glm::dmat4& world, const glm::dvec3& origin)
    {
        glm::mat4 relative(world);
        relative[3] = glm::vec4(glm::vec3(glm::dvec3(world[3]) - origin), static_cast<float>(world[3][3]));
        return relative;
    }
    inline glm::vec3 RelativeTo(const glm::dvec3& position, const glm::dvec3& origin) { return glm::vec3(position - origin); }
    // A world transform's rotation and scale (they need no more than float): directions, axes, sizes.
    inline glm::mat3 Basis(const glm::dmat4& world) { return glm::mat3(glm::dmat3(world)); }
    // A transform whose translation is small (a child's local offset, a bone): in float.
    inline glm::mat4 ToFloat(const glm::dmat4& transform) { return glm::mat4(transform); }
}
