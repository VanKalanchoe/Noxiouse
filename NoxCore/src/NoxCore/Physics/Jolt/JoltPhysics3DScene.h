#pragma once

#include "NoxCore/Physics/Physics3DScene.h"
#include "NoxCore/Physics/Jolt/NoxJoltJobSystem.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <memory>
#include <unordered_map>

namespace Nox 
{

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

        virtual void CreateCharacter(Entity entity) override;
        virtual void DestroyCharacter(Entity entity) override;

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
        void StepCharacters(float dt);
        void SyncCharactersToTransforms();
        void CaptureBodyPoses();

        Scene* m_Scene = nullptr;

        std::unique_ptr<JPH::TempAllocatorImpl> m_TempAllocator;
        std::unique_ptr<NoxJoltJobSystem> m_JobSystem;
        std::unique_ptr<JPH::PhysicsSystem> m_PhysicsSystem;

        struct JoltFilterData;
        std::unique_ptr<JoltFilterData> m_FilterData;

        std::unordered_map<uint32_t, JPH::BodyID> m_EntityToBodyMap;
        std::unordered_map<uint32_t, Entity> m_BodyToEntityMap;
        // A character and its positions at the last two fixed steps: the entity is drawn interpolated between them, so
        // motion is smooth however many steps a frame ran (0, 1 or 2 -- unevenly stepped movement made a follow camera flicker).
        struct CharacterState
        {
            JPH::Ref<JPH::CharacterVirtual> Character;
            glm::vec3 Previous{ 0.0f };
            glm::vec3 Current{ 0.0f };
            glm::vec3 Horizontal{ 0.0f }; // the horizontal velocity the controller has accelerated to (x, z)
        };
        std::unordered_map<uint32_t, CharacterState> m_EntityToCharacterMap;

        // A moving body and its pose after the last two fixed steps: drawn interpolated between them, like a character, so it
        // moves smoothly at any frame rate instead of in 60 Hz jumps.
        struct BodyPose
        {
            glm::vec3 Previous{ 0.0f };
            glm::vec3 Current{ 0.0f };
            glm::quat PreviousRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::quat CurrentRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            bool NeedsSync = false; // the entity does not show the final pose yet
        };
        std::unordered_map<uint32_t, BodyPose> m_BodyPoses;


        float m_Accumulator = 0.0f;
        static constexpr float c_FixedDeltaTime = 1.0f / 60.0f;
    };

} // namespace Nox
