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
        bool GetCharacterNetState(Entity entity, CharacterNetState& state) const override;
        void SetCharacterNetState(Entity entity, const CharacterNetState& state) override;
        void StepCharacter(Entity entity, float dt) override;
        bool GetBodyNetState(Entity entity, BodyNetState& state) const override;
        void SetBodyReplicationTarget(Entity entity, const BodyNetState& target, double extrapolateSeconds, bool snap) override;
        glm::dvec3 GetCharacterDrawnPosition(Entity entity) const override;
        void SetCharacterExternallyStepped(Entity entity, bool external) override;
        void AddCharacterVisualOffset(Entity entity, const glm::dvec3& offset) override;
        bool GetCharacterDebugState(Entity entity, CharacterDebugState& state) const override;
        void SetTickCallbacks(std::function<void()> beforeTick, std::function<void()> afterTick) override
        {
            m_BeforeTick = std::move(beforeTick);
            m_AfterTick = std::move(afterTick);
        }
        float GetFixedDeltaTime() const override { return c_FixedDeltaTime; }

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
        struct CharacterState;
        void StepCharacterState(Entity entity, CharacterState& state, float dt);
        void FollowGroundRotation(Entity entity, CharacterState& state, float dt);
        // Its velocity relative to the ground it stands on (native units): Unreal's CharacterMovement Velocity.
        static JPH::Vec3 OwnVelocity(const CharacterState& state);
        std::function<void()> m_BeforeTick;
        std::function<void()> m_AfterTick;
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
        // A network client's copies of dynamic bodies the server simulates: simulated here and pulled toward the server's state each
        // tick (NET5, Unreal's physics replication: SetBodyReplicationTarget, ApplyReplicationTargets).
        struct ReplicationTarget
        {
            BodyNetState State;
            double Extrapolate = 0.0; // seconds the server's state is carried forward (the latency)
            uint32_t TicksSince = 0;  // ticks since it arrived (carried forward by these too)
            bool Snap = false;
        };
        std::unordered_map<uint32_t, ReplicationTarget> m_ReplicationTargets; // by entity
        void ApplyReplicationTargets(float dt);
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
            bool ExternallyStepped = false;       // (networking) stepped by StepCharacter only
            glm::dvec3 VisualOffset{ 0.0 };       // (networking) a correction's offset, drawn and eased away
            float PreviousStepOffset = 0.0f;
            // (native) the ground's velocity its last step carried it with (walking on it); zero when that step did not (in the air,
            // landing, moving away from it) -- what Velocity takes off to be its own
            JPH::Vec3 GroundVelocity = JPH::Vec3::sZero();
        };
        std::unordered_map<uint32_t, CharacterState> m_EntityToCharacterMap;

        // The bodies characters stand on, their motion as the last physics update left it -- sampled at the start of each fixed tick,
        // before any character steps. A character presses its ground with its weight as impulses inside its own step, which the
        // solver only cancels in the next physics update: Jolt's ground velocity read after one step of a character and before that
        // update (a server stepping a client's character once per input, two in one tick; a client's replay) holds that push -- a
        // character standing on a 1 kg box read 5 m/s (Unreal takes its base's velocity from the last physics update the same way).
        struct GroundSample
        {
            JPH::Vec3 Linear = JPH::Vec3::sZero();
            JPH::Vec3 Angular = JPH::Vec3::sZero();
            JPH::RVec3 CenterOfMass = JPH::RVec3::sZero();
        };
        // Each character's ground and every body it touches (its active contacts: the box under it while it is a hair above it, a
        // step it walks onto), so a body it lands on within the tick is known as it was before it pushed it.
        std::unordered_map<uint32_t, GroundSample> m_GroundSamples; // by body id
        uint32_t m_CharacterStepsSinceSample = 0;
        void SampleCharacterGrounds();
        // The velocity of the point a character stands on (its ground body's sample; native units). Zero on static ground or none, and
        // for a body the tick's sampling did not see once a character has stepped this tick (it may hold that step's push: a
        // character landing on a 1 kg box read 12 m/s down and 3 m/s sideways, its walk animation played standing still).
        JPH::Vec3 CharacterGroundVelocity(const JPH::CharacterVirtual& character);

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
