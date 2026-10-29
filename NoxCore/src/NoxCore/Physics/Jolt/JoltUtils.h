#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Body/MotionQuality.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Nox {

    namespace JoltUtils {

        inline JPH::Vec3 ToJolt(const glm::vec3& v)
        {
            return JPH::Vec3(v.x, v.y, v.z);
        }

        inline JPH::Quat ToJolt(const glm::quat& q)
        {
            return JPH::Quat(q.x, q.y, q.z, q.w);
        }

        inline glm::vec3 ToGLM(const JPH::Vec3& v)
        {
            return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
        }

        inline glm::quat ToGLM(const JPH::Quat& q)
        {
            return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
        }

        // Positions (large world coordinates): Jolt's RVec3 is double precision (JPH_DOUBLE_PRECISION), like the engine's.
        inline JPH::RVec3 ToJoltPosition(const glm::dvec3& v)
        {
            return JPH::RVec3(static_cast<JPH::Real>(v.x), static_cast<JPH::Real>(v.y), static_cast<JPH::Real>(v.z));
        }

        inline glm::dvec3 ToGLMPosition(JPH::RVec3Arg v)
        {
            return glm::dvec3(v.GetX(), v.GetY(), v.GetZ());
        }

    } // namespace JoltUtils

} // namespace Nox
