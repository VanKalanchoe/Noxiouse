#pragma once

#include "Physics3DTypes.h"
#include "NoxCore/Core/WorldUnits.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Nox {

    class Scene;

    class IPhysics3DScene
    {
    public:
        virtual ~IPhysics3DScene() = default;

        // Factory method to instantiate active backend (default: Jolt)
        static std::unique_ptr<IPhysics3DScene> Create(Scene* scene);

        // The backend's own simulation unit, in world units per meter of it (docs/Units_And_World_Tools_Plan_2026.md section 4.2):
        // Jolt works in meters, so its backend returns 1. A backend crosses this boundary wherever it hands its physics library a
        // length that came from world-unit data -- positions, collider half extents / radii / offsets, linear velocities, forces,
        // impulses, ray / cast distances and hit points -- through ToNative() below, and converts back with ToWorld() when it reads
        // one out. Angles, angular velocities and dimensionless quantities (friction, damping, restitution, gravity factor) are never
        // touched this way; a backend's own internal tolerances (e.g. Jolt's mCharacterPadding) are written directly in its native
        // unit and stay untouched too.
        virtual float NativeUnitsPerMeter() const = 0;

        // Lifecycle & Simulation
        virtual void Init() = 0;
        // The scene's fixed ticks this frame (Scene::TickSeconds each: scripts' OnFixedUpdate and physics in step, the network tick),
        // and how far time is into the next one (0..1): bodies are drawn between their last two poses by it.
        virtual void StepTicks(uint32_t ticks, float alpha) = 0;
        virtual void OptimizeBroadPhase() = 0;

        // Body Management
        virtual void CreateBody(Entity entity) = 0;
        virtual void DestroyBody(Entity entity) = 0;

        // Character controllers (CharacterController3DComponent). Created for every such entity in Init(); driven
        // inside the fixed step from the component's input fields, results written back to the component + transform.
        // The joints (PhysicsJoint3DComponent) are made again before the next step: one was added or removed, or a body changed.
        virtual void InvalidateConstraints() {}

        virtual void CreateCharacter(Entity entity) = 0;
        virtual void DestroyCharacter(Entity entity) = 0;

        // Networking (docs/Networking_Plan_2026-11.md, NET3): a character's movement state, so a client can predict its own character
        // and correct it to the server's (world units).
        struct CharacterNetState
        {
            glm::dvec3 Position{ 0.0 };
            glm::vec3 Velocity{ 0.0f };   // the capsule's whole velocity (falling, jumping)
            glm::vec3 Horizontal{ 0.0f }; // the walking velocity it has accelerated to
            bool Grounded = false;
            bool WasOnGround = false;
        };
        virtual bool GetCharacterNetState(Entity entity, CharacterNetState& state) const { return false; }
        virtual void SetCharacterNetState(Entity entity, const CharacterNetState& state) {}
        // One character stepped by itself by dt with its controller's current inputs: a client replaying its inputs after a correction,
        // or the server applying a client's input when it arrives.
        virtual void StepCharacter(Entity entity, float dt) {}
        // Stepped only by StepCharacter, not by the fixed tick (the server moves a client's character when that client's inputs come).
        virtual void SetCharacterExternallyStepped(Entity entity, bool external) {}
        // Where it is drawn this frame: between its last two steps, plus a correction still gliding out (world units).
        virtual glm::dvec3 GetCharacterDrawnPosition(Entity entity) const { return glm::dvec3(0.0); }
        // Drawn this much away from where it is, eased away over about 0.1 s (a prediction correction is not a visible snap).
        virtual void AddCharacterVisualOffset(Entity entity, const glm::dvec3& offset) {}
        // Networking (NET5, Unreal's physics replication): a rigid body's state (world units; Sleeping: not moving, Jolt's deactivated).
        struct BodyNetState
        {
            glm::dvec3 Position{ 0.0 };
            glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::vec3 LinearVelocity{ 0.0f };
            glm::vec3 AngularVelocity{ 0.0f };
            bool Sleeping = false;
        };
        virtual bool GetBodyNetState(Entity entity, BodyNetState& state) const { return false; }
        // A network client's copy of a dynamic body the server simulates: simulated here too (what this client's character pushes moves
        // at once), and pulled every tick toward `target` -- the server's state, carried forward by `extrapolateSeconds` (the latency)
        // plus the time since (Net::CorrectBody). `snap`: placed there at once (it just appeared, or came back into relevancy).
        virtual void SetBodyReplicationTarget(Entity entity, const BodyNetState& target, double extrapolateSeconds, bool snap) {}
        // Called in every fixed tick: before the characters step (inputs are set or recorded there) and after the tick.
        virtual void SetTickCallbacks(std::function<void()> beforeTick, std::function<void()> afterTick) {}
        // The fixed tick's length (seconds).
        virtual float GetFixedDeltaTime() const { return 1.0f / 60.0f; }

        // A static triangle-mesh collider at the entity's world transform (terrain, a voxel chunk, level geometry a script
        // built: DynamicMeshes), replacing the one the entity had; empty arrays remove it. Counter-clockwise front faces. It
        // does not follow the entity: set it again after moving it.
        virtual void SetMeshCollider(Entity entity, std::span<const glm::vec3> positions, std::span<const uint32_t> indices,
                                     float friction = 0.5f, float restitution = 0.0f) = 0;
        virtual void RemoveMeshCollider(Entity entity) = 0;

        // Queries
        virtual bool RayCast(const glm::dvec3& origin, const glm::vec3& direction, float maxDistance,
                             RayCastHit& outHit, uint16_t layerMask = PhysicsLayers::MASK_ALL) = 0;

        virtual bool SphereCast(const glm::dvec3& origin, float radius, const glm::vec3& direction,
                                float maxDistance, ShapeCastHit& outHit, uint16_t layerMask = PhysicsLayers::MASK_ALL) = 0;

        virtual std::vector<Entity> OverlapSphere(const glm::dvec3& center, float radius,
                                                  uint16_t layerMask = PhysicsLayers::MASK_ALL) = 0;

        // The contacts begun since the last call by bodies whose entity has an Impact Sound (ImpactSoundComponent), after Step.
        virtual void TakeContacts(std::vector<PhysicsContact3D>& out) { out.clear(); }

        // Forces & Velocities
        virtual void AddForce(Entity entity, const glm::vec3& force) = 0;
        virtual void AddImpulse(Entity entity, const glm::vec3& impulse) = 0;
        virtual void SetLinearVelocity(Entity entity, const glm::vec3& velocity) = 0;
        virtual glm::vec3 GetLinearVelocity(Entity entity) const = 0;
        virtual void SetAngularVelocity(Entity entity, const glm::vec3& velocity) = 0;
        virtual glm::vec3 GetAngularVelocity(Entity entity) const = 0;

    protected:
        // Lengths only: never applied to a rotation, an angle, or a dimensionless factor.
        float PhysicsScale() const { return NativeUnitsPerMeter() / std::max(WorldUnits::PerMeter(), 1e-6f); }
        float ToNative(float worldLength) const { return worldLength * PhysicsScale(); }
        float ToWorld(float nativeLength) const { return nativeLength / PhysicsScale(); }
        glm::vec3 ToNative(const glm::vec3& worldLength) const { return worldLength * PhysicsScale(); }
        glm::vec3 ToWorld(const glm::vec3& nativeLength) const { return nativeLength / PhysicsScale(); }
        // Positions (large world coordinates: double precision).
        glm::dvec3 ToNative(const glm::dvec3& worldPosition) const { return worldPosition * static_cast<double>(PhysicsScale()); }
        glm::dvec3 ToWorld(const glm::dvec3& nativePosition) const { return nativePosition / static_cast<double>(PhysicsScale()); }
    };

} // namespace Nox
