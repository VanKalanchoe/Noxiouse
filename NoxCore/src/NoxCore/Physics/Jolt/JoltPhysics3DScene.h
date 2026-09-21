#pragma once

#include "NoxCore/Physics/Physics3DScene.h"
#include "NoxCore/Physics/Jolt/NoxJoltJobSystem.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Core/TempAllocator.h>

#include <memory>
#include <unordered_map>

namespace Nox {

    class Scene;

    class JoltPhysics3DScene final : public IPhysics3DScene
    {
    public:
        explicit JoltPhysics3DScene(Scene* scene);
        virtual ~JoltPhysics3DScene() override;

        virtual void Init() override;
        virtual void Step(float dt) override;
        virtual void OptimizeBroadPhase() override;

        virtual void CreateBody(Entity entity) override;
        virtual void DestroyBody(Entity entity) override;

        virtual bool RayCast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                             RayCastHit& outHit, uint16_t layerMask = PhysicsLayers::MASK_ALL) override;

        virtual bool SphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction,
                                float maxDistance, ShapeCastHit& outHit, uint16_t layerMask = PhysicsLayers::MASK_ALL) override;

        virtual std::vector<Entity> OverlapSphere(const glm::vec3& center, float radius,
                                                  uint16_t layerMask = PhysicsLayers::MASK_ALL) override;

        virtual void AddForce(Entity entity, const glm::vec3& force) override;
        virtual void AddImpulse(Entity entity, const glm::vec3& impulse) override;
        virtual void SetLinearVelocity(Entity entity, const glm::vec3& velocity) override;
        virtual glm::vec3 GetLinearVelocity(Entity entity) const override;
        virtual void SetAngularVelocity(Entity entity, const glm::vec3& velocity) override;
        virtual glm::vec3 GetAngularVelocity(Entity entity) const override;

        JPH::PhysicsSystem& GetPhysicsSystem() { return *m_PhysicsSystem; }
        const JPH::PhysicsSystem& GetPhysicsSystem() const { return *m_PhysicsSystem; }

    private:
        Scene* m_Scene = nullptr;

        std::unique_ptr<JPH::TempAllocatorImpl> m_TempAllocator;
        std::unique_ptr<NoxJoltJobSystem> m_JobSystem;
        std::unique_ptr<JPH::PhysicsSystem> m_PhysicsSystem;

        struct JoltFilterData;
        std::unique_ptr<JoltFilterData> m_FilterData;

        std::unordered_map<uint32_t, JPH::BodyID> m_EntityToBodyMap;
        std::unordered_map<uint32_t, Entity> m_BodyToEntityMap;

        float m_Accumulator = 0.0f;
        static constexpr float c_FixedDeltaTime = 1.0f / 60.0f;
    };

} // namespace Nox
