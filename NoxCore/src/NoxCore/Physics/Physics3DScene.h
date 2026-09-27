#pragma once

#include "Physics3DTypes.h"
#include "NoxCore/Core/WorldUnits.h"
#include <algorithm>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

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
        virtual void Step(float dt) = 0;
        virtual void OptimizeBroadPhase() = 0;

        // Body Management
        virtual void CreateBody(Entity entity) = 0;
        virtual void DestroyBody(Entity entity) = 0;

        // Character controllers (CharacterController3DComponent). Created for every such entity in Init(); driven
        // inside the fixed step from the component's input fields, results written back to the component + transform.
        virtual void CreateCharacter(Entity entity) = 0;
        virtual void DestroyCharacter(Entity entity) = 0;

        // Queries
        virtual bool RayCast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                             RayCastHit& outHit, uint16_t layerMask = PhysicsLayers::MASK_ALL) = 0;

        virtual bool SphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction,
                                float maxDistance, ShapeCastHit& outHit, uint16_t layerMask = PhysicsLayers::MASK_ALL) = 0;

        virtual std::vector<Entity> OverlapSphere(const glm::vec3& center, float radius,
                                                  uint16_t layerMask = PhysicsLayers::MASK_ALL) = 0;

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
    };

} // namespace Nox
