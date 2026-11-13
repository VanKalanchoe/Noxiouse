#include "NetPhysicsReplication.h"

#include <algorithm>
#include <cmath>

namespace Nox::Net
{
    namespace
    {
        // The rotation from `from` to `to` as an axis times an angle (the short way round).
        glm::vec3 RotationError(const glm::quat& from, const glm::quat& to)
        {
            glm::quat delta = to * glm::conjugate(from);
            if (delta.w < 0.0f)
                delta = -delta;
            const float sine = glm::length(glm::vec3(delta.x, delta.y, delta.z));
            if (sine < 1e-7f)
                return glm::vec3(0.0f);
            const float angle = 2.0f * std::atan2(sine, delta.w);
            return glm::vec3(delta.x, delta.y, delta.z) / sine * angle;
        }
    }

    BodyState ExtrapolateBody(const BodyState& state, double seconds, double maxSeconds)
    {
        BodyState carried = state;
        if (state.Sleeping)
            return carried;
        const double t = std::clamp(seconds, 0.0, maxSeconds);
        carried.Position += glm::dvec3(state.LinearVelocity) * t;
        const glm::vec3 spin = state.AngularVelocity * static_cast<float>(t);
        const float angle = glm::length(spin);
        if (angle > 1e-7f)
            carried.Rotation = glm::normalize(glm::angleAxis(angle, spin / angle) * state.Rotation);
        return carried;
    }

    BodyCorrection CorrectBody(BodyState& current, const BodyState& target, float dt, const BodyCorrectionSettings& settings)
    {
        const glm::dvec3 error = target.Position - current.Position;
        const double distance = glm::length(error);
        const glm::vec3 rotationError = RotationError(current.Rotation, target.Rotation);
        const float angle = glm::length(rotationError);
        if (!std::isfinite(distance) || distance > settings.HardSnapDistance)
        {
            current = target; // too far to pull (a teleport, a respawn, a long loss): placed
            return BodyCorrection::Snapped;
        }
        if (target.Sleeping && distance <= settings.RestDistance && angle <= settings.RestAngle)
        {
            current.LinearVelocity = glm::vec3(0.0f);
            current.AngularVelocity = glm::vec3(0.0f);
            current.Sleeping = true;
            return BodyCorrection::AtRest;
        }
        (void)dt;
        current.Position += error * static_cast<double>(settings.PositionLerp);
        current.LinearVelocity = target.LinearVelocity + glm::vec3(error) * settings.LinearVelocityCoefficient;
        current.Rotation = glm::normalize(glm::slerp(current.Rotation, target.Rotation, settings.AngleLerp));
        current.AngularVelocity = target.AngularVelocity + rotationError * settings.AngularVelocityCoefficient;
        current.Sleeping = false;
        return BodyCorrection::Corrected;
    }
}
