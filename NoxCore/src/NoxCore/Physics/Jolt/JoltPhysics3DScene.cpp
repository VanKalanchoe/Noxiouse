#include "JoltPhysics3DScene.h"
#include "JoltUtils.h"
#include "NoxCore/Physics/PhysicsEngine.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Components.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Body/BodyFilter.h>

#include <spdlog/spdlog.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Nox {

    // Factory method implementation
    std::unique_ptr<IPhysics3DScene> IPhysics3DScene::Create(Scene* scene)
    {
        return std::make_unique<JoltPhysics3DScene>(scene);
    }

    namespace JoltBroadPhaseLayers {
        static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
        static constexpr JPH::BroadPhaseLayer MOVING(1);
        static constexpr JPH::uint NUM_LAYERS = 2;
    }

    class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
    {
    public:
        BPLayerInterfaceImpl()
        {
            m_ObjectToBroadPhase[PhysicsLayers::NON_MOVING] = JoltBroadPhaseLayers::NON_MOVING;
            m_ObjectToBroadPhase[PhysicsLayers::MOVING]     = JoltBroadPhaseLayers::MOVING;
            m_ObjectToBroadPhase[PhysicsLayers::TRIGGER]    = JoltBroadPhaseLayers::MOVING;
            m_ObjectToBroadPhase[PhysicsLayers::CHARACTER]  = JoltBroadPhaseLayers::MOVING;
        }

        virtual JPH::uint GetNumBroadPhaseLayers() const override
        {
            return JoltBroadPhaseLayers::NUM_LAYERS;
        }

        virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
        {
            JPH_ASSERT(inLayer < PhysicsLayers::NUM_LAYERS);
            return m_ObjectToBroadPhase[inLayer];
        }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
        {
            switch ((JPH::BroadPhaseLayer::Type)inLayer)
            {
            case (JPH::BroadPhaseLayer::Type)JoltBroadPhaseLayers::NON_MOVING: return "NON_MOVING";
            case (JPH::BroadPhaseLayer::Type)JoltBroadPhaseLayers::MOVING:     return "MOVING";
            default: return "INVALID";
            }
        }
#endif

    private:
        JPH::BroadPhaseLayer m_ObjectToBroadPhase[PhysicsLayers::NUM_LAYERS];
    };

    class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
        {
            switch (inLayer1)
            {
            case PhysicsLayers::NON_MOVING:
                return inLayer2 == JoltBroadPhaseLayers::MOVING;
            case PhysicsLayers::MOVING:
            case PhysicsLayers::TRIGGER:
            case PhysicsLayers::CHARACTER:
                return true;
            default:
                return false;
            }
        }
    };

    class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
    {
    public:
        virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override
        {
            switch (inLayer1)
            {
            case PhysicsLayers::NON_MOVING:
                return inLayer2 == PhysicsLayers::MOVING || inLayer2 == PhysicsLayers::CHARACTER;
            case PhysicsLayers::MOVING:
                return true;
            case PhysicsLayers::TRIGGER:
                return inLayer2 == PhysicsLayers::MOVING || inLayer2 == PhysicsLayers::CHARACTER;
            case PhysicsLayers::CHARACTER:
                return inLayer2 != PhysicsLayers::TRIGGER; // Characters trigger sensors via trigger queries
            default:
                return false;
            }
        }
    };

    struct JoltPhysics3DScene::JoltFilterData
    {
        BPLayerInterfaceImpl BPLayerInterface;
        ObjectVsBroadPhaseLayerFilterImpl ObjectVsBroadphaseFilter;
        ObjectLayerPairFilterImpl ObjectPairFilter;
    };

    JoltPhysics3DScene::JoltPhysics3DScene(Scene* scene)
        : m_Scene(scene)
    {
    }

    JoltPhysics3DScene::~JoltPhysics3DScene()
    {
        if (m_PhysicsSystem)
        {
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
            for (auto& [entityId, bodyId] : m_EntityToBodyMap)
            {
                if (!bodyId.IsInvalid())
                {
                    bodyInterface.RemoveBody(bodyId);
                    bodyInterface.DestroyBody(bodyId);
                }
            }
            m_EntityToBodyMap.clear();
            m_BodyPoses.clear();
            m_EntityToCharacterMap.clear();
            m_BodyToEntityMap.clear();
        }
    }

    void JoltPhysics3DScene::Init()
    {
        PhysicsEngine::Init();

        m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
        m_JobSystem = std::make_unique<NoxJoltJobSystem>(1024, 128);
        m_FilterData = std::make_unique<JoltFilterData>();

        constexpr JPH::uint cMaxBodies = 10240;
        constexpr JPH::uint cNumBodyMutexes = 0; // default auto-detect
        constexpr JPH::uint cMaxBodyPairs = 10240;
        constexpr JPH::uint cMaxContactConstraints = 10240;

        m_PhysicsSystem = std::make_unique<JPH::PhysicsSystem>();
        m_PhysicsSystem->Init(
            cMaxBodies,
            cNumBodyMutexes,
            cMaxBodyPairs,
            cMaxContactConstraints,
            m_FilterData->BPLayerInterface,
            m_FilterData->ObjectVsBroadphaseFilter,
            m_FilterData->ObjectPairFilter
        );

        m_PhysicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

        // Create bodies for all entities currently possessing RigidBody3DComponent
        if (m_Scene)
        {
            auto view = m_Scene->GetRegistry().view<RigidBody3DComponent, TransformComponent>();
            for (auto entityID : view)
            {
                Entity entity{ entityID, m_Scene };
                CreateBody(entity);
            }

            for (auto entityID : m_Scene->GetRegistry().view<CharacterController3DComponent, TransformComponent>())
                CreateCharacter(Entity{ entityID, m_Scene });
        }

        OptimizeBroadPhase();
        SPDLOG_INFO("JoltPhysics3DScene initialized successfully.");
    }

    void JoltPhysics3DScene::OptimizeBroadPhase()
    {
        if (m_PhysicsSystem)
            m_PhysicsSystem->OptimizeBroadPhase();
    }

    void JoltPhysics3DScene::CreateBody(Entity entity)
    {
        if (!m_PhysicsSystem || !entity)
            return;

        auto& rb = entity.GetComponent<RigidBody3DComponent>();
        const auto& transform = entity.GetComponent<TransformComponent>();

        glm::vec3 worldPos = transform.Translation;
        glm::quat worldRot = glm::quat(transform.Rotation);
        glm::vec3 worldScale = transform.Scale;

        if (entity.HasComponent<WorldTransformComponent>())
        {
            const auto& world = entity.GetComponent<WorldTransformComponent>();
            worldPos = glm::vec3(world.WorldMatrix[3]);
            worldScale = glm::vec3(
                glm::length(glm::vec3(world.WorldMatrix[0])),
                glm::length(glm::vec3(world.WorldMatrix[1])),
                glm::length(glm::vec3(world.WorldMatrix[2]))
            );
            if (worldScale.x > 1e-5f && worldScale.y > 1e-5f && worldScale.z > 1e-5f)
            {
                glm::mat3 rotMat(
                    glm::vec3(world.WorldMatrix[0]) / worldScale.x,
                    glm::vec3(world.WorldMatrix[1]) / worldScale.y,
                    glm::vec3(world.WorldMatrix[2]) / worldScale.z
                );
                worldRot = glm::quat_cast(rotMat);
            }
        }

        // Create collision shape based on attached collider components
        JPH::Ref<JPH::Shape> shape;

        if (entity.HasComponent<BoxCollider3DComponent>())
        {
            const auto& box = entity.GetComponent<BoxCollider3DComponent>();
            glm::vec3 halfExtents = glm::max(box.HalfExtents * worldScale, glm::vec3(0.001f));
            JPH::BoxShapeSettings shapeSettings(JoltUtils::ToJolt(halfExtents));
            shapeSettings.mDensity = rb.Mass;

            if (box.Offset != glm::vec3(0.0f))
            {
                JPH::RotatedTranslatedShapeSettings offsetShape(JoltUtils::ToJolt(box.Offset), JPH::Quat::sIdentity(), &shapeSettings);
                shape = offsetShape.Create().Get();
            }
            else
            {
                shape = shapeSettings.Create().Get();
            }
        }
        else if (entity.HasComponent<SphereCollider3DComponent>())
        {
            const auto& sphere = entity.GetComponent<SphereCollider3DComponent>();
            float radius = std::max(sphere.Radius * std::max(worldScale.x, std::max(worldScale.y, worldScale.z)), 0.001f);
            JPH::SphereShapeSettings shapeSettings(radius);
            shapeSettings.mDensity = rb.Mass;

            if (sphere.Offset != glm::vec3(0.0f))
            {
                JPH::RotatedTranslatedShapeSettings offsetShape(JoltUtils::ToJolt(sphere.Offset), JPH::Quat::sIdentity(), &shapeSettings);
                shape = offsetShape.Create().Get();
            }
            else
            {
                shape = shapeSettings.Create().Get();
            }
        }
        else if (entity.HasComponent<CapsuleCollider3DComponent>())
        {
            const auto& capsule = entity.GetComponent<CapsuleCollider3DComponent>();
            float halfHeight = std::max(capsule.HalfHeight * worldScale.y, 0.001f);
            float radius = std::max(capsule.Radius * std::max(worldScale.x, worldScale.z), 0.001f);
            JPH::CapsuleShapeSettings shapeSettings(halfHeight, radius);
            shapeSettings.mDensity = rb.Mass;

            if (capsule.Offset != glm::vec3(0.0f))
            {
                JPH::RotatedTranslatedShapeSettings offsetShape(JoltUtils::ToJolt(capsule.Offset), JPH::Quat::sIdentity(), &shapeSettings);
                shape = offsetShape.Create().Get();
            }
            else
            {
                shape = shapeSettings.Create().Get();
            }
        }
        else
        {
            // Default fallback shape: 1x1x1 unit box
            glm::vec3 halfExtents = glm::max(0.5f * worldScale, glm::vec3(0.001f));
            JPH::BoxShapeSettings shapeSettings(JoltUtils::ToJolt(halfExtents));
            shape = shapeSettings.Create().Get();
        }

        // Motion type & Layer resolution
        JPH::EMotionType motionType = JPH::EMotionType::Dynamic;
        JPH::ObjectLayer objectLayer = rb.Layer;

        float friction = 0.5f;
        float restitution = 0.0f;
        if (entity.HasComponent<BoxCollider3DComponent>())
        {
            friction = entity.GetComponent<BoxCollider3DComponent>().Friction;
            restitution = entity.GetComponent<BoxCollider3DComponent>().Restitution;
        }
        else if (entity.HasComponent<SphereCollider3DComponent>())
        {
            friction = entity.GetComponent<SphereCollider3DComponent>().Friction;
            restitution = entity.GetComponent<SphereCollider3DComponent>().Restitution;
        }
        else if (entity.HasComponent<CapsuleCollider3DComponent>())
        {
            friction = entity.GetComponent<CapsuleCollider3DComponent>().Friction;
            restitution = entity.GetComponent<CapsuleCollider3DComponent>().Restitution;
        }

        if (rb.Type == RigidBody3DComponent::BodyType::Static)
        {
            motionType = JPH::EMotionType::Static;
            objectLayer = PhysicsLayers::NON_MOVING;
        }
        else if (rb.Type == RigidBody3DComponent::BodyType::Kinematic)
        {
            motionType = JPH::EMotionType::Kinematic;
            if (objectLayer == PhysicsLayers::NON_MOVING)
                objectLayer = PhysicsLayers::MOVING;
        }
        else
        {
            if (objectLayer == PhysicsLayers::NON_MOVING)
                objectLayer = PhysicsLayers::MOVING;
        }

        JPH::BodyCreationSettings bodySettings(
            shape,
            JoltUtils::ToJolt(worldPos),
            JoltUtils::ToJolt(worldRot),
            motionType,
            objectLayer
        );

        bodySettings.mFriction = friction;
        bodySettings.mRestitution = restitution;
        bodySettings.mLinearDamping = rb.LinearDamping;
        bodySettings.mAngularDamping = rb.AngularDamping;
        bodySettings.mGravityFactor = rb.GravityFactor;
        bodySettings.mAllowSleeping = rb.AllowSleeping;
        bodySettings.mIsSensor = rb.IsSensor;
        bodySettings.mMotionQuality = (rb.Quality == RigidBody3DComponent::MotionQuality::LinearCast)
            ? JPH::EMotionQuality::LinearCast
            : JPH::EMotionQuality::Discrete;

        bodySettings.mUserData = static_cast<uint64_t>(static_cast<uint32_t>(entity));

        JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
        JPH::Body* body = bodyInterface.CreateBody(bodySettings);
        if (!body)
        {
            SPDLOG_ERROR("Failed to create Jolt body for entity {}", static_cast<uint32_t>(entity));
            return;
        }

        bodyInterface.AddBody(body->GetID(), (motionType == JPH::EMotionType::Static) ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);

        uint32_t bodyIdVal = body->GetID().GetIndexAndSequenceNumber();
        rb.RuntimeBodyID = bodyIdVal;

        uint32_t entIdVal = static_cast<uint32_t>(entity);
        m_EntityToBodyMap[entIdVal] = body->GetID();
        m_BodyToEntityMap[bodyIdVal] = entity;
    }

    void JoltPhysics3DScene::DestroyBody(Entity entity)
    {
        if (!m_PhysicsSystem || !entity)
            return;

        uint32_t entIdVal = static_cast<uint32_t>(entity);
        auto it = m_EntityToBodyMap.find(entIdVal);
        if (it != m_EntityToBodyMap.end())
        {
            JPH::BodyID bodyId = it->second;
            if (!bodyId.IsInvalid())
            {
                JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
                bodyInterface.RemoveBody(bodyId);
                bodyInterface.DestroyBody(bodyId);

                m_BodyToEntityMap.erase(bodyId.GetIndexAndSequenceNumber());
            }
            m_EntityToBodyMap.erase(it);
            m_BodyPoses.erase(entIdVal);
        }

        if (entity.HasComponent<RigidBody3DComponent>())
            entity.GetComponent<RigidBody3DComponent>().RuntimeBodyID = 0xFFFFFFFF;
    }

    // Ported from Jolt's Samples/Tests/Character/CharacterVirtualTest.cpp: the capsule shape is offset so the
    // character position is at the feet, the supporting volume is the lower sphere cap.
    void JoltPhysics3DScene::CreateCharacter(Entity entity)
    {
        if (!m_PhysicsSystem || !entity)
            return;

        const auto& controller = entity.GetComponent<CharacterController3DComponent>();
        const auto& transform = entity.GetComponent<TransformComponent>();

        glm::vec3 worldPos = transform.Translation;
        if (entity.HasComponent<WorldTransformComponent>())
            worldPos = glm::vec3(entity.GetComponent<WorldTransformComponent>().WorldMatrix[3]);

        const float radius = std::max(controller.Radius, 0.01f);
        const float halfHeight = std::max(controller.Height, 0.0f) * 0.5f;

        JPH::RefConst<JPH::Shape> shape = JPH::RotatedTranslatedShapeSettings(
            JPH::Vec3(0.0f, halfHeight + radius, 0.0f), JPH::Quat::sIdentity(), new JPH::CapsuleShape(halfHeight, radius)).Create().Get();

        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        settings->mMaxSlopeAngle = glm::radians(controller.MaxSlopeDegrees);
        settings->mMaxStrength = 100.0f;
        settings->mShape = shape;
        settings->mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        settings->mCharacterPadding = 0.02f;
        settings->mPenetrationRecoverySpeed = 1.0f;
        settings->mPredictiveContactDistance = 0.1f;
        settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
        settings->mEnhancedInternalEdgeRemoval = false;

        JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
            settings, JoltUtils::ToJolt(worldPos), JPH::Quat::sIdentity(), static_cast<uint64_t>(static_cast<uint32_t>(entity)), m_PhysicsSystem.get());

        CharacterState& state = m_EntityToCharacterMap[static_cast<uint32_t>(entity)];
        state.Character = character;
        state.Previous = state.Current = worldPos;
    }

    void JoltPhysics3DScene::DestroyCharacter(Entity entity)
    {
        if (entity)
            m_EntityToCharacterMap.erase(static_cast<uint32_t>(entity));
    }

    void JoltPhysics3DScene::StepCharacters(float dt)
    {
        if (!m_Scene)
            return;

        const JPH::Vec3 gravity = m_PhysicsSystem->GetGravity();
        const JPH::Vec3 up = JPH::Vec3::sAxisY();

        for (auto& [entityId, state] : m_EntityToCharacterMap)
        {
            Entity entity{ static_cast<entt::entity>(entityId), m_Scene };
            if (!entity.IsValid() || !entity.HasComponent<CharacterController3DComponent>())
                continue;

            auto& controller = entity.GetComponent<CharacterController3DComponent>();
            JPH::CharacterVirtual* character = state.Character.GetPtr();

            state.Previous = state.Current;
            character->UpdateGroundVelocity();
            const JPH::Vec3 groundVelocity = character->GetGroundVelocity();
            const JPH::Vec3 currentVelocity = character->GetLinearVelocity();
            const bool onGround = character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
            const bool movingTowardsGround = (currentVelocity.GetY() - groundVelocity.GetY()) <= 0.1f;

            JPH::Vec3 velocity;
            if (onGround && movingTowardsGround)
            {
                velocity = groundVelocity;
                if (controller.JumpSpeed > 0.0f)
                    velocity += up * controller.JumpSpeed;
            }
            else
            {
                velocity = JPH::Vec3(0.0f, currentVelocity.GetY(), 0.0f);
            }
            controller.JumpSpeed = 0.0f;

            velocity += gravity * controller.GravityScale * dt;
            // Horizontal velocity ramps towards the wanted one (Unreal's MaxAcceleration / BrakingDeceleration) instead of
            // snapping to it -- most of what makes its character start and stop smoothly. Less acceleration in the air.
            {
                const glm::vec3 wanted(controller.MoveVelocity.x, 0.0f, controller.MoveVelocity.z);
                const glm::vec3 toWanted = wanted - state.Horizontal;
                const float distance = glm::length(toWanted);
                const bool wantsToMove = glm::length(wanted) > 0.001f;
                const float rate = (wantsToMove ? controller.MaxAcceleration : controller.BrakingDeceleration) *
                                   (character->IsSupported() ? 1.0f : std::max(controller.AirControl, 0.0f));
                const float step = rate * dt;
                state.Horizontal = distance <= step || distance < 1e-5f ? wanted : state.Horizontal + toWanted * (step / distance);
            }
            velocity += JPH::Vec3(state.Horizontal.x, 0.0f, state.Horizontal.z);

            character->SetLinearVelocity(velocity);

            JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
            updateSettings.mStickToFloorStepDown = -up * 0.5f;
            updateSettings.mWalkStairsStepUp = up * controller.StepHeight;

            character->ExtendedUpdate(dt, -up * gravity.Length(), updateSettings,
                m_PhysicsSystem->GetDefaultBroadPhaseLayerFilter(PhysicsLayers::CHARACTER),
                m_PhysicsSystem->GetDefaultLayerFilter(PhysicsLayers::CHARACTER),
                {}, {}, *m_TempAllocator);
            state.Current = JoltUtils::ToGLM(character->GetPosition());
        }
    }

    // The controller owns the entity's translation (like a dynamic body does); rotation stays with scripts.
    void JoltPhysics3DScene::SyncCharactersToTransforms()
    {
        if (!m_Scene)
            return;

        for (auto& [entityId, state] : m_EntityToCharacterMap)
        {
            Entity entity{ static_cast<entt::entity>(entityId), m_Scene };
            if (!entity.IsValid() || !entity.HasComponent<CharacterController3DComponent>())
                continue;

            auto& controller = entity.GetComponent<CharacterController3DComponent>();
            JPH::CharacterVirtual* character = state.Character.GetPtr();
            controller.IsGrounded = character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
            controller.Velocity = JoltUtils::ToGLM(character->GetLinearVelocity());

            // Drawn between the last two fixed steps by how far the accumulator is into the next one.
            const float alpha = glm::clamp(m_Accumulator / c_FixedDeltaTime, 0.0f, 1.0f);
            glm::vec3 position = glm::mix(state.Previous, state.Current, alpha);
            if (entity.HasComponent<RelationshipComponent>())
            {
                UUID parentUUID = entity.GetComponent<RelationshipComponent>().Parent;
                if (parentUUID != 0)
                {
                    Entity parentEntity = m_Scene->GetEntityByUUID(parentUUID);
                    if (parentEntity && parentEntity.HasComponent<WorldTransformComponent>())
                        position = glm::vec3(glm::inverse(parentEntity.GetComponent<WorldTransformComponent>().WorldMatrix) * glm::vec4(position, 1.0f));
                }
            }

            entity.GetComponent<TransformComponent>().Translation = position;
            entity.GetComponent<DirtyTransformComponent>().isDirty = true;
        }
    }

    // Called after every fixed step: the pose before it is the one the entity is drawn from, the new one the one it moves to.
    void JoltPhysics3DScene::CaptureBodyPoses()
    {
        JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
        for (const auto& [entityId, bodyId] : m_EntityToBodyMap)
        {
            if (bodyId.IsInvalid() || bodyInterface.GetMotionType(bodyId) == JPH::EMotionType::Static)
                continue;

            const glm::vec3 position = JoltUtils::ToGLM(bodyInterface.GetPosition(bodyId));
            const glm::quat rotation = JoltUtils::ToGLM(bodyInterface.GetRotation(bodyId));
            auto [pose, isNew] = m_BodyPoses.try_emplace(entityId);
            if (isNew)
            {
                // Nothing to interpolate from yet: the first step is drawn as it is.
                pose->second.Previous = pose->second.Current = position;
                pose->second.PreviousRotation = pose->second.CurrentRotation = rotation;
                pose->second.NeedsSync = true;
                continue;
            }

            BodyPose& stored = pose->second;
            stored.Previous = stored.Current;
            stored.PreviousRotation = stored.CurrentRotation;
            if (position != stored.Current || rotation != stored.CurrentRotation)
                stored.NeedsSync = true;
            stored.Current = position;
            stored.CurrentRotation = rotation;
        }
    }

    void JoltPhysics3DScene::Step(float dt)
    {
        if (!m_PhysicsSystem)
            return;

        m_Accumulator += dt;
        constexpr int maxSubSteps = 8; // below ~8 fps the simulation runs slower than real time instead of spiralling
        int steps = 0;

        while (m_Accumulator >= c_FixedDeltaTime && steps < maxSubSteps)
        {
            StepCharacters(c_FixedDeltaTime);
            m_PhysicsSystem->Update(c_FixedDeltaTime, 1, m_TempAllocator.get(), m_JobSystem.get());
            CaptureBodyPoses();
            m_Accumulator -= c_FixedDeltaTime;
            steps++;
        }

        // Time the step cap could not simulate is dropped, not carried: a backlog would keep the simulation behind forever.
        m_Accumulator = std::min(m_Accumulator, c_FixedDeltaTime);

        SyncCharactersToTransforms();

        // Synchronize positions and rotations back to ECS TransformComponent
        if (m_Scene)
        {
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();

            for (const auto& [entityId, bodyId] : m_EntityToBodyMap)
            {
                if (bodyId.IsInvalid())
                    continue;

                Entity entity{ static_cast<entt::entity>(entityId), m_Scene };
                if (!entity.IsValid() || !entity.HasComponent<RigidBody3DComponent>())
                    continue;

                const auto& rb = entity.GetComponent<RigidBody3DComponent>();
                if (rb.Type == RigidBody3DComponent::BodyType::Static)
                    continue; // Static bodies do not move

                // A body that fell asleep is synced once more after its last step, so it ends on its final pose.
                auto poseIt = m_BodyPoses.find(entityId);
                BodyPose* pose = poseIt != m_BodyPoses.end() ? &poseIt->second : nullptr;
                const bool active = bodyInterface.IsActive(bodyId);
                if (active || (pose && pose->NeedsSync))
                {
                    auto& transform = entity.GetComponent<TransformComponent>();
                    auto& dirty = entity.GetComponent<DirtyTransformComponent>();

                    glm::vec3 worldPos;
                    glm::quat worldRot;
                    if (pose)
                    {
                        // Drawn between the last two fixed steps by how far the accumulator is into the next one.
                        const float alpha = glm::clamp(m_Accumulator / c_FixedDeltaTime, 0.0f, 1.0f);
                        worldPos = glm::mix(pose->Previous, pose->Current, alpha);
                        worldRot = glm::slerp(pose->PreviousRotation, pose->CurrentRotation, alpha);
                        if (!active && pose->Previous == pose->Current && pose->PreviousRotation == pose->CurrentRotation)
                            pose->NeedsSync = false; // settled: this frame writes the final pose
                    }
                    else
                    {
                        worldPos = JoltUtils::ToGLM(bodyInterface.GetPosition(bodyId));
                        worldRot = JoltUtils::ToGLM(bodyInterface.GetRotation(bodyId));
                    }

                    bool hasParent = false;
                    glm::mat4 parentWorldInv(1.0f);
                    if (entity.HasComponent<RelationshipComponent>())
                    {
                        UUID parentUUID = entity.GetComponent<RelationshipComponent>().Parent;
                        if (parentUUID != 0)
                        {
                            Entity parentEntity = m_Scene->GetEntityByUUID(parentUUID);
                            if (parentEntity && parentEntity.HasComponent<WorldTransformComponent>())
                            {
                                parentWorldInv = glm::inverse(parentEntity.GetComponent<WorldTransformComponent>().WorldMatrix);
                                hasParent = true;
                            }
                        }
                    }

                    if (hasParent)
                    {
                        glm::mat4 worldMat = glm::translate(glm::mat4(1.0f), worldPos) * glm::toMat4(worldRot) * glm::scale(glm::mat4(1.0f), transform.Scale);
                        glm::mat4 localMat = parentWorldInv * worldMat;

                        glm::vec3 localScale, localSkew;
                        glm::vec4 localPerspective;
                        glm::quat localRot;
                        glm::vec3 localPos;
                        glm::decompose(localMat, localScale, localRot, localPos, localSkew, localPerspective);

                        transform.Translation = localPos;
                        transform.Rotation = glm::eulerAngles(glm::conjugate(localRot));
                    }
                    else
                    {
                        transform.Translation = worldPos;
                        transform.Rotation = glm::eulerAngles(worldRot);
                    }
                    dirty.isDirty = true;
                }
            }
        }
    }

    bool JoltPhysics3DScene::RayCast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                     RayCastHit& outHit, uint16_t layerMask)
    {
        if (!m_PhysicsSystem)
            return false;

        JPH::RRayCast ray(JoltUtils::ToJolt(origin), JoltUtils::ToJolt(direction * maxDistance));
        JPH::RayCastResult hit;

        if (m_PhysicsSystem->GetNarrowPhaseQuery().CastRay(ray, hit))
        {
            outHit.Hit = true;
            outHit.Fraction = hit.mFraction;
            outHit.Distance = hit.mFraction * maxDistance;
            outHit.Position = origin + direction * outHit.Distance;

            auto it = m_BodyToEntityMap.find(hit.mBodyID.GetIndexAndSequenceNumber());
            if (it != m_BodyToEntityMap.end())
                outHit.HitEntity = it->second;

            JPH::BodyLockRead lock(m_PhysicsSystem->GetBodyLockInterface(), hit.mBodyID);
            if (lock.Succeeded())
            {
                const JPH::Body& body = lock.GetBody();
                outHit.Normal = JoltUtils::ToGLM(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction)));
            }
            return true;
        }

        outHit.Hit = false;
        return false;
    }

    bool JoltPhysics3DScene::SphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction,
                                        float maxDistance, ShapeCastHit& outHit, uint16_t layerMask)
    {
        if (!m_PhysicsSystem)
            return false;

        JPH::SphereShape sphere(radius);
        JPH::RShapeCast shapeCast(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JoltUtils::ToJolt(origin)), JoltUtils::ToJolt(direction * maxDistance));

        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        m_PhysicsSystem->GetNarrowPhaseQuery().CastShape(shapeCast, JPH::ShapeCastSettings(), JPH::RVec3::sZero(), collector);

        if (collector.HadHit())
        {
            const JPH::ShapeCastResult& hit = collector.mHit;
            outHit.Hit = true;
            outHit.Fraction = hit.mFraction;
            outHit.Distance = hit.mFraction * maxDistance;
            outHit.ContactPosition = JoltUtils::ToGLM(hit.mContactPointOn2);
            outHit.ContactNormal = JoltUtils::ToGLM(-hit.mPenetrationAxis.Normalized());

            auto it = m_BodyToEntityMap.find(hit.mBodyID2.GetIndexAndSequenceNumber());
            if (it != m_BodyToEntityMap.end())
                outHit.HitEntity = it->second;

            return true;
        }

        outHit.Hit = false;
        return false;
    }

    std::vector<Entity> JoltPhysics3DScene::OverlapSphere(const glm::vec3& center, float radius, uint16_t layerMask)
    {
        std::vector<Entity> results;
        if (!m_PhysicsSystem)
            return results;

        JPH::SphereShape sphere(radius);
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
        m_PhysicsSystem->GetNarrowPhaseQuery().CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JoltUtils::ToJolt(center)), JPH::CollideShapeSettings(), JPH::RVec3::sZero(), collector);

        results.reserve(collector.mHits.size());
        for (const JPH::CollideShapeResult& hit : collector.mHits)
        {
            auto it = m_BodyToEntityMap.find(hit.mBodyID2.GetIndexAndSequenceNumber());
            if (it != m_BodyToEntityMap.end())
                results.push_back(it->second);
        }

        return results;
    }

    void JoltPhysics3DScene::AddForce(Entity entity, const glm::vec3& force)
    {
        if (!m_PhysicsSystem || !entity)
            return;

        uint32_t entIdVal = static_cast<uint32_t>(entity);
        auto it = m_EntityToBodyMap.find(entIdVal);
        if (it != m_EntityToBodyMap.end())
        {
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
            bodyInterface.AddForce(it->second, JoltUtils::ToJolt(force));
        }
    }

    void JoltPhysics3DScene::AddImpulse(Entity entity, const glm::vec3& impulse)
    {
        if (!m_PhysicsSystem || !entity)
            return;

        uint32_t entIdVal = static_cast<uint32_t>(entity);
        auto it = m_EntityToBodyMap.find(entIdVal);
        if (it != m_EntityToBodyMap.end())
        {
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
            bodyInterface.AddImpulse(it->second, JoltUtils::ToJolt(impulse));
        }
    }

    void JoltPhysics3DScene::SetLinearVelocity(Entity entity, const glm::vec3& velocity)
    {
        if (!m_PhysicsSystem || !entity)
            return;

        uint32_t entIdVal = static_cast<uint32_t>(entity);
        auto it = m_EntityToBodyMap.find(entIdVal);
        if (it != m_EntityToBodyMap.end())
        {
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
            bodyInterface.SetLinearVelocity(it->second, JoltUtils::ToJolt(velocity));
        }
    }

    glm::vec3 JoltPhysics3DScene::GetLinearVelocity(Entity entity) const
    {
        if (!m_PhysicsSystem || !entity)
            return glm::vec3(0.0f);

        uint32_t entIdVal = static_cast<uint32_t>(entity);
        auto it = m_EntityToBodyMap.find(entIdVal);
        if (it != m_EntityToBodyMap.end())
        {
            const JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
            return JoltUtils::ToGLM(bodyInterface.GetLinearVelocity(it->second));
        }
        return glm::vec3(0.0f);
    }

    void JoltPhysics3DScene::SetAngularVelocity(Entity entity, const glm::vec3& velocity)
    {
        if (!m_PhysicsSystem || !entity)
            return;

        uint32_t entIdVal = static_cast<uint32_t>(entity);
        auto it = m_EntityToBodyMap.find(entIdVal);
        if (it != m_EntityToBodyMap.end())
        {
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
            bodyInterface.SetAngularVelocity(it->second, JoltUtils::ToJolt(velocity));
        }
    }

    glm::vec3 JoltPhysics3DScene::GetAngularVelocity(Entity entity) const
    {
        if (!m_PhysicsSystem || !entity)
            return glm::vec3(0.0f);

        uint32_t entIdVal = static_cast<uint32_t>(entity);
        auto it = m_EntityToBodyMap.find(entIdVal);
        if (it != m_EntityToBodyMap.end())
        {
            const JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
            return JoltUtils::ToGLM(bodyInterface.GetAngularVelocity(it->second));
        }
        return glm::vec3(0.0f);
    }

} // namespace Nox
