#pragma once

#include "NoxCore/Physics/Physics3DScene.h"
#include "NoxCore/Physics/Jolt/JoltContactEvents.h"
#include "NoxCore/Physics/Jolt/JoltRollingResistance.h"
#include "NoxCore/Physics/Jolt/NoxJoltJobSystem.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Constraints/Constraint.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Nox 
{

    class Scene;

    class JoltPhysics3DScene final : public IPhysics3DScene
    {
    public:
        explicit JoltPhysics3DScene(Scene* scene);
        virtual ~JoltPhysics3DScene() override;

        virtual float NativeUnitsPerMeter() const override { return 1.0f; } // Jolt works in meters

        virtual void Init() override;
        virtual void StepTicks(uint32_t ticks, float alpha) override;
        virtual void OptimizeBroadPhase() override;

        virtual void CreateBody(Entity entity) override;
        virtual void InvalidateConstraints() override { m_ConstraintsDirty = true; }
        virtual void DestroyBody(Entity entity) override;

        virtual void CreateCharacter(Entity entity) override;
        virtual void DestroyCharacter(Entity entity) override;

        virtual void SetMeshCollider(Entity entity, std::span<const glm::vec3> positions, std::span<const uint32_t> indices,
                                     float friction, float restitution) override;
        virtual void RemoveMeshCollider(Entity entity) override;

        virtual bool RayCast(const glm::dvec3& origin, const glm::vec3& direction, float maxDistance,
                             RayCastHit& outHit, uint16_t layerMask = PhysicsLayers::MASK_ALL) override;

        virtual bool SphereCast(const glm::dvec3& origin, float radius, const glm::vec3& direction,
                                float maxDistance, ShapeCastHit& outHit, uint16_t layerMask = PhysicsLayers::MASK_ALL) override;

        virtual std::vector<Entity> OverlapSphere(const glm::dvec3& center, float radius,
                                                  uint16_t layerMask = PhysicsLayers::MASK_ALL) override;

        virtual void AddForce(Entity entity, const glm::vec3& force) override;
        virtual void AddImpulse(Entity entity, const glm::vec3& impulse) override;
        virtual void SetLinearVelocity(Entity entity, const glm::vec3& velocity) override;
        virtual glm::vec3 GetLinearVelocity(Entity entity) const override;
        virtual void SetAngularVelocity(Entity entity, const glm::vec3& velocity) override;
        virtual glm::vec3 GetAngularVelocity(Entity entity) const override;

        virtual void TakeContacts(std::vector<PhysicsContact3D>& out) override;

        JPH::PhysicsSystem& GetPhysicsSystem() { return *m_PhysicsSystem; }
        const JPH::PhysicsSystem& GetPhysicsSystem() const { return *m_PhysicsSystem; }

    private:
        // A mesh collider's shape in native units, unscaled (triangles, or their convex hull): read once from the cooked mesh, then
        // shared by every body using that mesh this session.
        JPH::Ref<JPH::Shape> MeshColliderShape(const struct MeshCollider3DComponent& collider, bool convex);
        std::unordered_map<std::string, JPH::Ref<JPH::Shape>> m_MeshShapes;
        // A mesh collider's triangles carry their submesh as a Jolt physics material (one per submesh index, shared), so a ray hit
        // tells which submesh -- which material, which physical surface -- it struck (Unreal's per-face physical material).
        const JPH::PhysicsMaterial* SubmeshMaterial(uint32_t submesh);
        std::vector<JPH::Ref<JPH::PhysicsMaterial>> m_SubmeshMaterials;
        std::unordered_map<const JPH::PhysicsMaterial*, uint32_t> m_SubmeshOfMaterial;

        // Joints (PhysicsJoint3DComponent) as Jolt constraints, all made again when anything they hold on to changed (a body is
        // destroyed: they go first, Jolt keeps pointers to their bodies).
        void RebuildConstraints();
        void RemoveConstraints();
        JPH::BodyID BodyAtOrAbove(Entity entity) const;
        std::vector<JPH::Ref<JPH::Constraint>> m_Constraints;
        bool m_ConstraintsDirty = true;
        // Collision filters (CollisionFilter3DComponent) and the body pairs a joint keeps apart: every body's collision group points
        // at this filter, its group the filter it uses (0: none), its sub group the body itself (m_BodySerials).
        JPH::Ref<JPH::GroupFilter> m_GroupFilter;
        std::unordered_map<uint32_t, uint32_t> m_BodySerials; // entity -> sub group
        uint32_t m_NextBodySerial = 1;

        void StepCharacters(float dt);
        void SyncCharactersToTransforms();
        void CaptureBodyPoses();

        Scene* m_Scene = nullptr;

        std::unique_ptr<JPH::TempAllocatorImpl> m_TempAllocator;
        std::unique_ptr<NoxJoltJobSystem> m_JobSystem;
        // The physics system's contact listener: declared before it, so it outlives it.
        std::unique_ptr<JoltRollingResistance> m_RollingResistance;
        std::unique_ptr<JoltContactEvents> m_ContactEvents; // the listener: rolling resistance behind it
        std::vector<JoltContactEvents::Contact> m_TakenContacts;
        std::unique_ptr<JPH::PhysicsSystem> m_PhysicsSystem;

        struct JoltFilterData;
        std::unique_ptr<JoltFilterData> m_FilterData;

        std::unordered_map<uint32_t, JPH::BodyID> m_EntityToBodyMap;
        // Bodies that follow their entity's transform instead of moving it: the kinematic ones (a script or the network moves the
        // entity -- a network client's proxies of the server's entities are made kinematic, Scene::IsNetProxy), swept to it before
        // each step so they push dynamic bodies.
        std::unordered_set<uint32_t> m_FollowTransform;
        void MoveFollowers(uint32_t ticks);
        std::unordered_map<uint32_t, Entity> m_BodyToEntityMap;
        std::unordered_map<uint32_t, JPH::BodyID> m_MeshColliders; // SetMeshCollider: static bodies, apart from the rigid bodies
        // A character and its positions at the last two fixed steps: the entity is drawn interpolated between them, so
        // motion is smooth however many steps a frame ran (0, 1 or 2 -- unevenly stepped movement made a follow camera flicker).
        struct CharacterState
        {
            JPH::Ref<JPH::CharacterVirtual> Character;
            glm::dvec3 Previous{ 0.0 }; // world positions (double precision)
            glm::dvec3 Current{ 0.0 };
            glm::vec3 Horizontal{ 0.0f }; // the horizontal velocity the controller has accelerated to (x, z)
            // Root motion on the ground (native units): carried on as momentum when the character leaves it (Unreal keeps the
            // root motion velocity as the character's velocity when it starts falling); a jump or fall clip moves it no further.
            glm::vec3 GroundRootMotion{ 0.0f };
            bool WasOnGround = false;
            // Unreal's floor test, what IsGrounded reports: on walkable ground, or (resting on a stair's edge, which counts as steep)
            // a walkable floor within step height under the capsule while not rising.
            bool Grounded = false;
            // Stair smoothing (world units, vertical): what a step up or down moved the capsule by beyond following the ground's
            // slope, taken off the drawn position and eased back to 0, so the mesh glides up a stair instead of popping. Drawn
            // interpolated between the last two fixed steps like the position.
            float StepOffset = 0.0f;
            float PreviousStepOffset = 0.0f;
        };
        std::unordered_map<uint32_t, CharacterState> m_EntityToCharacterMap;

        // A moving body and its pose after the last two fixed steps: drawn interpolated between them, like a character, so it
        // moves smoothly at any frame rate instead of in 60 Hz jumps.
        struct BodyPose
        {
            glm::dvec3 Previous{ 0.0 }; // world positions (double precision)
            glm::dvec3 Current{ 0.0 };
            glm::quat PreviousRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::quat CurrentRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            bool NeedsSync = false; // the entity does not show the final pose yet
        };
        std::unordered_map<uint32_t, BodyPose> m_BodyPoses;


        float m_Accumulator = 0.0f; // how far into the next tick (alpha x c_FixedDeltaTime): the draw interpolation
        static constexpr float c_FixedDeltaTime = 1.0f / 60.0f;
    };

} // namespace Nox
