#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Physics replication (docs/Networking_Plan_2026-11.md, NET5; Unreal's FPhysicsReplication, "predictive interpolation" in UE 5.4): a
// client simulates the server's dynamic rigid bodies itself -- what its own character pushes moves at once -- and every tick pulls each
// one toward the server's state, extrapolated to the client's present by the latency: the velocity is set to the server's plus the
// position error times a rate, the position and rotation are nudged a little, and a body more than a few metres off is placed there
// (a teleport, a respawn). A body the server has at rest is left to sleep once it is close. Nothing here knows the scene.
namespace Nox::Net
{
    struct BodyState
    {
        glm::dvec3 Position{ 0.0 };   // world units
        glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 LinearVelocity{ 0.0f };  // world units a second
        glm::vec3 AngularVelocity{ 0.0f }; // radians a second
        bool Sleeping = false;
    };

    struct BodyCorrectionSettings
    {
        double HardSnapDistance = 400.0;       // world units (4 m when a unit is a centimetre): placed, not pulled (Unreal: 400 cm)
        float PositionLerp = 0.05f;            // of the position error removed directly each tick
        float LinearVelocityCoefficient = 8.0f; // 1/s: the velocity added per unit of position error (the error halves in ~80 ms)
        float AngleLerp = 0.1f;                // of the rotation error removed directly each tick
        float AngularVelocityCoefficient = 8.0f;
        double RestDistance = 1.0;             // world units: a body the server has asleep and this close is left alone
        float RestAngle = 0.02f;               // radians
        double MaxExtrapolation = 0.3;         // seconds the server's state is carried forward at most
    };

    enum class BodyCorrection : unsigned char { Corrected, Snapped, AtRest };

    // The server's state carried forward by `seconds` along its velocities (a body at rest stays).
    BodyState ExtrapolateBody(const BodyState& state, double seconds, double maxSeconds);
    // One tick's pull of the client's body toward the (extrapolated) server state.
    BodyCorrection CorrectBody(BodyState& current, const BodyState& target, float dt, const BodyCorrectionSettings& settings);
}
