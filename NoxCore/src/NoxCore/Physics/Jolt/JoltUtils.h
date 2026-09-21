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

    } // namespace JoltUtils

} // namespace Nox
