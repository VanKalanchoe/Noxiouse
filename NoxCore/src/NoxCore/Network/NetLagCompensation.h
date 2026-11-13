#pragma once
#include "NetProtocol.h"
#include "NetSnapshot.h"

#include <cstdint>
#include <deque>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Lag compensation (docs/Networking_Plan_2026-11.md, NET5; Valve's Source engine and Overwatch's "favor the shooter"): a client sees
// other entities a little in the past (interpolation) and its shot reaches the server later still, so the server checks a shot against
// where the targets were on that client's screen -- the tick the client drew them at, rewound in a history of their hitboxes the server
// keeps for the last second. The world itself is not moved back: only the replicated entities' hitboxes, tested here analytically
// (boxes, spheres, capsules); NetReplication adds the static level (walls hide what is behind them) from the physics scene. Nothing here
// knows the scene (tested alone, Tests/NetTest.cpp).
namespace Nox::Net
{
    // A hitbox in world space.
    struct HitShape
    {
        enum class Type : uint8_t { Box, Sphere, Capsule };
        Type Kind = Type::Box;
        glm::dvec3 Center{ 0.0 };
        glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 HalfExtents{ 0.5f }; // Box
        float Radius = 0.5f;           // Sphere, Capsule
        float HalfHeight = 0.5f;       // Capsule: half the distance between its two sphere centres, along its local Y
    };

    struct Hitbox
    {
        NetId Entity = 0;
        ClientId Owner = InvalidClientId; // (the shooter's own entities are skipped)
        HitShape Shape;
    };

    struct RewoundHit
    {
        NetId Entity = 0;
        double Distance = 0.0;
        glm::dvec3 Point{ 0.0 };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
    };

    // The first hit of a ray (direction normalized) against one shape within maxDistance.
    bool RayHitShape(const glm::dvec3& origin, const glm::dvec3& direction, double maxDistance, const HitShape& shape, double& distance,
                     glm::vec3& normal);

    class LagCompensationHistory
    {
    public:
        // The replicated entities' hitboxes after server tick `tick` (once a tick, in order). Older than `seconds` are forgotten.
        void Record(uint64_t tick, std::vector<Hitbox> hitboxes, double ticksToKeep);
        // The oldest and newest ticks it can rewind to (0, 0 while empty).
        uint64_t GetOldestTick() const { return m_Frames.empty() ? 0 : m_Frames.front().Tick; }
        uint64_t GetNewestTick() const { return m_Frames.empty() ? 0 : m_Frames.back().Tick; }
        // The first hitbox the ray hits as the entities stood at `tick` (fractional: between two recorded ticks, each entity's hitbox
        // in between; clamped to the history), skipping the entities `shooter` owns. False: nothing within maxDistance.
        bool Raycast(double tick, const glm::dvec3& origin, const glm::dvec3& direction, double maxDistance, ClientId shooter,
                     RewoundHit& hit) const;
        // The hitboxes as they stood at `tick` (the same interpolation; tests and debugging).
        void HitboxesAt(double tick, std::vector<Hitbox>& out) const;
        void Clear() { m_Frames.clear(); }

    private:
        struct Frame
        {
            uint64_t Tick = 0;
            std::vector<Hitbox> Hitboxes; // sorted by entity (two frames are matched entity by entity)
        };
        std::deque<Frame> m_Frames;
    };

    // Where the server allows a client's claimed view tick to be (Overwatch's validation of the shooter's time): within `tolerance`
    // ticks of what the server expects from that client's latency, and never more than `maxRewind` ticks back or in the future.
    double ClampViewTick(double claimed, double serverTick, double expectedDelayTicks, double toleranceTicks, double maxRewindTicks);
}
