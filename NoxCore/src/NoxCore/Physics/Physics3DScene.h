#pragma once

#include "Physics3DTypes.h"
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
    };

} // namespace Nox
