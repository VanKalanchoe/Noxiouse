#include "JoltPhysics3DScene.h"
#include "NoxCore/Math/Math.h"
#include "JoltUtils.h"
#include "NoxCore/Physics/PhysicsEngine.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/MeshImporter.h"
#include "NoxCore/Project/Project.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/TaperedCylinderShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Body/BodyFilter.h>

#include <spdlog/spdlog.h>
#include <functional>
#include <set>
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
            for (auto& [entityId, bodyId] : m_MeshColliders)
            {
                bodyInterface.RemoveBody(bodyId);
                bodyInterface.DestroyBody(bodyId);
            }
            m_MeshColliders.clear();
            m_BodyPoses.clear();
            m_EntityToCharacterMap.clear();
            m_BodyToEntityMap.clear();
        }
    }

    // KHR_physics_rigid_bodies' collision filters and the pairs of jointed bodies that do not collide (CollisionFilter3DComponent,
    // PhysicsJoint3DComponent::EnableCollision), as Jolt's group filter: a body's group is its filter (0: none), its sub group the body.
    class NoxCollisionGroupFilter final : public JPH::GroupFilter
    {
    public:
        struct Filter
        {
            std::vector<std::string> Systems, CollideWith, NotCollideWith;
        };
        std::vector<Filter> Filters; // group g uses Filters[g - 1]
        std::set<std::pair<uint32_t, uint32_t>> Apart; // sub groups (bodies), smaller first

        bool CanCollide(const JPH::CollisionGroup& a, const JPH::CollisionGroup& b) const override
        {
            const uint32_t first = std::min(a.GetSubGroupID(), b.GetSubGroupID()), second = std::max(a.GetSubGroupID(), b.GetSubGroupID());
            if (Apart.contains({ first, second }))
                return false;
            return Allows(a.GetGroupID(), b.GetGroupID()) && Allows(b.GetGroupID(), a.GetGroupID());
        }

        // A collides with B (the spec): A's systems are among B's CollideWith when B has that list (which then lets nothing else in, a
        // body in no system included), and not all among B's NotCollideWith (which never excludes a body in no system).
        bool Allows(uint32_t groupA, uint32_t groupB) const
        {
            if (groupB == 0 || groupB > Filters.size())
                return true;
            static const std::vector<std::string> none;
            const std::vector<std::string>& systems = groupA != 0 && groupA <= Filters.size() ? Filters[groupA - 1].Systems : none;
            const Filter& b = Filters[groupB - 1];
            auto within = [&](const std::vector<std::string>& list)
            {
                return std::all_of(systems.begin(), systems.end(), [&](const std::string& system) { return std::find(list.begin(), list.end(), system) != list.end(); });
            };
            if (!b.CollideWith.empty() && (systems.empty() || !within(b.CollideWith)))
                return false;
            if (!b.NotCollideWith.empty() && !systems.empty() && within(b.NotCollideWith))
                return false;
            return true;
        }
    };

    void JoltPhysics3DScene::Init()
    {
        m_GroupFilter = new NoxCollisionGroupFilter();
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

        m_PhysicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f)); // Jolt's own native unit (meters/s^2): not a world-unit value, untouched
        m_RollingResistance = std::make_unique<JoltRollingResistance>();
        m_PhysicsSystem->SetContactListener(m_RollingResistance.get());

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

    namespace
    {
        // An entity's world position, rotation and scale: its WorldTransformComponent's when it has one, its own transform otherwise.
        struct WorldPose
        {
            glm::dvec3 Position;
            glm::quat Rotation;
            glm::vec3 Scale;
        };
        WorldPose WorldPoseOf(Entity entity)
        {
            const auto& transform = entity.GetComponent<TransformComponent>();
            WorldPose pose{ transform.Translation, glm::quat(transform.Rotation), transform.Scale };
            if (entity.HasComponent<WorldTransformComponent>())
            {
                const auto& world = entity.GetComponent<WorldTransformComponent>();
                pose.Position = world.GetPosition();
                pose.Scale = glm::vec3(
                    glm::length(glm::vec3(world.WorldMatrix[0])),
                    glm::length(glm::vec3(world.WorldMatrix[1])),
                    glm::length(glm::vec3(world.WorldMatrix[2]))
                );
                if (pose.Scale.x > 1e-5f && pose.Scale.y > 1e-5f && pose.Scale.z > 1e-5f)
                {
                    glm::mat3 rotMat(
                        glm::vec3(world.WorldMatrix[0]) / pose.Scale.x,
                        glm::vec3(world.WorldMatrix[1]) / pose.Scale.y,
                        glm::vec3(world.WorldMatrix[2]) / pose.Scale.z
                    );
                    pose.Rotation = glm::quat_cast(rotMat);
                }
            }
            return pose;
        }

        // What a body's collider says about its surface; RollingRadius in Jolt units (the sphere's or capsule's radius, what it rolls on).
        struct ColliderSurface
        {
            float Friction = 0.5f;
            float Restitution = 0.0f;
            float RollingResistance = 0.0f;
            float RollingRadius = 0.0f;
        };

        // The entity's collider component as a Jolt shape: its size times the entity's world scale, moved by its Offset. Collider sizes
        // are world-unit lengths (the component fields); Jolt wants its own native unit (nativePerWorld). Null without a collider.
        // `dynamic`: the body moves under forces (triangle meshes and unbounded planes have no mass: a mesh takes its hull there).
        // `meshShape`: a mesh collider's unscaled native shape (JoltPhysics3DScene::MeshColliderShape, cached per mesh).
        JPH::Ref<JPH::Shape> ColliderShapeOf(Entity entity, const glm::vec3& worldScale, float nativePerWorld, bool dynamic,
                                             const std::function<JPH::Ref<JPH::Shape>(const MeshCollider3DComponent&, bool)>& meshShape,
                                             ColliderSurface& surface)
        {
            JPH::Ref<JPH::Shape> shape;
            glm::vec3 offset(0.0f);
            if (entity.HasComponent<BoxCollider3DComponent>())
            {
                const auto& box = entity.GetComponent<BoxCollider3DComponent>();
                const glm::vec3 halfExtents = glm::max(box.HalfExtents * worldScale, glm::vec3(0.001f)) * nativePerWorld;
                shape = JPH::BoxShapeSettings(JoltUtils::ToJolt(halfExtents)).Create().Get();
                offset = box.Offset;
                surface.Friction = box.Friction;
                surface.Restitution = box.Restitution;
            }
            else if (entity.HasComponent<SphereCollider3DComponent>())
            {
                const auto& sphere = entity.GetComponent<SphereCollider3DComponent>();
                const float radius = std::max(sphere.Radius * std::max(worldScale.x, std::max(worldScale.y, worldScale.z)), 0.001f) * nativePerWorld;
                shape = JPH::SphereShapeSettings(radius).Create().Get();
                offset = sphere.Offset;
                surface.Friction = sphere.Friction;
                surface.Restitution = sphere.Restitution;
                surface.RollingResistance = sphere.RollingResistance;
                surface.RollingRadius = radius;
            }
            else if (entity.HasComponent<CapsuleCollider3DComponent>())
            {
                const auto& capsule = entity.GetComponent<CapsuleCollider3DComponent>();
                const float halfHeight = std::max(capsule.HalfHeight * worldScale.y, 0.001f) * nativePerWorld;
                const float radius = std::max(capsule.Radius * std::max(worldScale.x, worldScale.z), 0.001f) * nativePerWorld;
                const float bottom = std::max(capsule.BottomRadius * std::max(worldScale.x, worldScale.z), 0.001f) * nativePerWorld;
                if (capsule.BottomRadius > 0.0f && std::abs(bottom - radius) > 1e-5f)
                    shape = JPH::TaperedCapsuleShapeSettings(halfHeight, radius, bottom).Create().Get();
                else
                    shape = JPH::CapsuleShapeSettings(halfHeight, radius).Create().Get();
                offset = capsule.Offset;
                surface.Friction = capsule.Friction;
                surface.Restitution = capsule.Restitution;
                surface.RollingResistance = capsule.RollingResistance;
                surface.RollingRadius = radius;
            }
            else if (entity.HasComponent<CylinderCollider3DComponent>())
            {
                const auto& cylinder = entity.GetComponent<CylinderCollider3DComponent>();
                const float halfHeight = std::max(cylinder.HalfHeight * worldScale.y, 0.001f) * nativePerWorld;
                const float horizontal = std::max(worldScale.x, worldScale.z) * nativePerWorld;
                const float top = std::max(cylinder.Radius * horizontal, 0.0f);
                const float bottom = cylinder.BottomRadius > 0.0f ? cylinder.BottomRadius * horizontal : top;
                // The convex radius rounds the edges: no more than the smallest dimension allows.
                const float convexRadius = std::min({ JPH::cDefaultConvexRadius, halfHeight, std::max(std::min(top, bottom), 0.0f) });
                if (std::abs(top - bottom) > 1e-5f)
                    shape = JPH::TaperedCylinderShapeSettings(halfHeight, top, bottom, convexRadius).Create().Get();
                else
                    shape = JPH::CylinderShapeSettings(halfHeight, std::max(top, 0.001f), convexRadius).Create().Get();
                offset = cylinder.Offset;
                surface.Friction = cylinder.Friction;
                surface.Restitution = cylinder.Restitution;
            }
            else if (entity.HasComponent<PlaneCollider3DComponent>())
            {
                const auto& plane = entity.GetComponent<PlaneCollider3DComponent>();
                if (plane.HalfSize.x > 0.0f && plane.HalfSize.y > 0.0f)
                {
                    // A finite plane: a slab one convex radius thick under the surface.
                    const glm::vec3 halfExtents(plane.HalfSize.x * worldScale.x * nativePerWorld, JPH::cDefaultConvexRadius,
                                                plane.HalfSize.y * worldScale.z * nativePerWorld);
                    JPH::BoxShapeSettings slab(JoltUtils::ToJolt(halfExtents), 0.5f * JPH::cDefaultConvexRadius);
                    shape = JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, -JPH::cDefaultConvexRadius, 0.0f), JPH::Quat::sIdentity(), slab.Create().Get()).Create().Get();
                }
                else if (!dynamic)
                {
                    shape = JPH::PlaneShapeSettings(JPH::Plane(JPH::Vec3::sAxisY(), 0.0f)).Create().Get();
                }
                else
                {
                    SPDLOG_WARN("Entity {}: an unbounded plane collider cannot move (give it a Half Size, or a static body)", static_cast<uint32_t>(entity));
                }
                offset = plane.Offset;
                surface.Friction = plane.Friction;
                surface.Restitution = plane.Restitution;
            }
            else if (entity.HasComponent<MeshCollider3DComponent>())
            {
                const auto& mesh = entity.GetComponent<MeshCollider3DComponent>();
                if (JPH::Ref<JPH::Shape> base = meshShape(mesh, mesh.Convex || dynamic))
                {
                    shape = worldScale == glm::vec3(1.0f) ? base : JPH::ScaledShapeSettings(base, JoltUtils::ToJolt(worldScale)).Create().Get();
                    surface.Friction = mesh.Friction;
                    surface.Restitution = mesh.Restitution;
                }
            }
            if (shape && offset != glm::vec3(0.0f))
                shape = JPH::RotatedTranslatedShapeSettings(JoltUtils::ToJolt(offset * nativePerWorld), JPH::Quat::sIdentity(), shape).Create().Get();
            return shape;
        }
    }

    void JoltPhysics3DScene::CreateBody(Entity entity)
    {
        if (!m_PhysicsSystem || !entity)
            return;

        auto& rb = entity.GetComponent<RigidBody3DComponent>();
        const float nativePerWorld = PhysicsScale();
        const WorldPose pose = WorldPoseOf(entity);
        const glm::dvec3 worldPos = pose.Position;
        const glm::quat worldRot = pose.Rotation;
        const glm::vec3 worldScale = pose.Scale;

        // The entity's own collider, then the colliders of descendants without a body of their own: they are part of this body (a
        // compound, as Godot's collision shapes under a body and Unreal's attached shape components), each where it stands relative
        // to the body. KHR_physics_rigid_bodies places shapes the same way (a rotated or offset shape sits on a child node).
        const bool dynamic = rb.Type == RigidBody3DComponent::BodyType::Dynamic;
        auto meshShape = [this](const MeshCollider3DComponent& collider, bool convex) { return MeshColliderShape(collider, convex); };
        ColliderSurface surface;
        JPH::Ref<JPH::Shape> shape = ColliderShapeOf(entity, worldScale, nativePerWorld, dynamic, meshShape, surface);
        struct Part
        {
            JPH::Ref<JPH::Shape> Shape;
            glm::vec3 Position; // native units, in the body's frame
            glm::quat Rotation;
        };
        std::vector<Part> parts;
        if (m_Scene && entity.HasComponent<RelationshipComponent>())
        {
            const glm::quat toBody = glm::inverse(worldRot);
            std::vector<UUID> pending = entity.GetComponent<RelationshipComponent>().Children;
            while (!pending.empty())
            {
                Entity child = m_Scene->GetEntityByUUID(pending.back());
                pending.pop_back();
                if (!child || child.HasComponent<RigidBody3DComponent>())
                    continue; // a body of its own takes its subtree
                const WorldPose childPose = WorldPoseOf(child);
                ColliderSurface childSurface;
                if (JPH::Ref<JPH::Shape> childShape = ColliderShapeOf(child, childPose.Scale, nativePerWorld, dynamic, meshShape, childSurface))
                {
                    if (!shape && parts.empty())
                        surface = childSurface; // the body's surface: its own collider's, else its first part's
                    parts.push_back({ childShape, glm::vec3(toBody * glm::vec3(childPose.Position - worldPos)) * nativePerWorld,
                                      toBody * childPose.Rotation });
                }
                if (child.HasComponent<RelationshipComponent>())
                {
                    const std::vector<UUID>& children = child.GetComponent<RelationshipComponent>().Children;
                    pending.insert(pending.end(), children.begin(), children.end());
                }
            }
        }
        if (!parts.empty())
        {
            JPH::StaticCompoundShapeSettings compound;
            if (shape)
                compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), shape);
            for (const Part& part : parts)
                compound.AddShape(JoltUtils::ToJolt(part.Position), JoltUtils::ToJolt(part.Rotation), part.Shape);
            JPH::Shape::ShapeResult result = compound.Create();
            if (result.HasError())
                SPDLOG_ERROR("Compound collider for entity {}: {}", static_cast<uint32_t>(entity), result.GetError().c_str());
            else
                shape = result.Get();
            surface.RollingRadius = 0.0f; // several shapes: no one radius it rolls on
        }
        if (!shape)
        {
            // Default fallback shape: 1x1x1 unit box
            glm::vec3 halfExtents = ToNative(glm::max(0.5f * worldScale, glm::vec3(0.001f)));
            JPH::BoxShapeSettings shapeSettings(JoltUtils::ToJolt(halfExtents));
            shape = shapeSettings.Create().Get();
        }

        // Motion type & Layer resolution
        JPH::EMotionType motionType = JPH::EMotionType::Dynamic;
        JPH::ObjectLayer objectLayer = rb.Layer;

        const float friction = surface.Friction;
        const float restitution = surface.Restitution;
        const float rollingResistance = surface.RollingResistance;
        const float rollingRadius = surface.RollingRadius;

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
            JoltUtils::ToJoltPosition(ToNative(worldPos)),
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

        // The collision group: the filter of the body (its own, else one of its colliders'), and the body itself (joints keep pairs apart).
        uint32_t filterGroup = 0;
        const CollisionFilter3DComponent* filter = entity.HasComponent<CollisionFilter3DComponent>() ? &entity.GetComponent<CollisionFilter3DComponent>() : nullptr;
        if (!filter && m_Scene && entity.HasComponent<RelationshipComponent>())
        {
            for (UUID childID : entity.GetComponent<RelationshipComponent>().Children)
            {
                Entity child = m_Scene->GetEntityByUUID(childID);
                if (child && !child.HasComponent<RigidBody3DComponent>() && child.HasComponent<CollisionFilter3DComponent>())
                {
                    filter = &child.GetComponent<CollisionFilter3DComponent>();
                    break;
                }
            }
        }
        auto* groups = static_cast<NoxCollisionGroupFilter*>(m_GroupFilter.GetPtr());
        if (filter)
        {
            groups->Filters.push_back({ filter->Systems, filter->CollideWith, filter->NotCollideWith });
            filterGroup = static_cast<uint32_t>(groups->Filters.size());
        }
        const uint32_t serial = m_NextBodySerial++;
        m_BodySerials[static_cast<uint32_t>(entity)] = serial;
        bodySettings.mCollisionGroup = JPH::CollisionGroup(m_GroupFilter, filterGroup, serial);

        // rb.Mass is the body's mass (kg), independent of the shape's native-unit volume: set it directly instead of through density
        // (mDensity x volume), so a collider's mass no longer changes when the world unit -- and so the native geometry's size --
        // changes (U6). CalculateInertia takes only the mass from here and works out the inertia tensor from the shape and it.
        // 0: from the volume instead, at Jolt's density of water (Unreal's default physical material is the same 1 g/cm^3); the
        // native unit is meters, so that is the real-world mass. What glTF physics (KHR_physics_rigid_bodies) asks when it gives none.
        if (rb.Mass > 0.0f)
        {
            bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bodySettings.mMassPropertiesOverride.mMass = rb.Mass;
        }

        JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
        JPH::Body* body = bodyInterface.CreateBody(bodySettings);
        if (!body)
        {
            SPDLOG_ERROR("Failed to create Jolt body for entity {}", static_cast<uint32_t>(entity));
            return;
        }

        bodyInterface.AddBody(body->GetID(), (motionType == JPH::EMotionType::Static) ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        if (motionType == JPH::EMotionType::Dynamic && rollingResistance > 0.0f && rollingRadius > 0.0f)
            m_RollingResistance->Track(body->GetID(), rollingResistance, rollingRadius);

        uint32_t bodyIdVal = body->GetID().GetIndexAndSequenceNumber();
        rb.RuntimeBodyID = bodyIdVal;

        uint32_t entIdVal = static_cast<uint32_t>(entity);
        m_EntityToBodyMap[entIdVal] = body->GetID();
        m_BodyToEntityMap[bodyIdVal] = entity;
    }

    JPH::Ref<JPH::Shape> JoltPhysics3DScene::MeshColliderShape(const MeshCollider3DComponent& collider, bool convex)
    {
        const std::string key = std::to_string(static_cast<uint64_t>(collider.Mesh)) + ':' + std::to_string(collider.SubmeshIndex) + ':' +
                                std::to_string(collider.SubmeshCount) + (convex ? ":hull" : ":mesh");
        if (const auto found = m_MeshShapes.find(key); found != m_MeshShapes.end())
            return found->second;
        JPH::Ref<JPH::Shape>& shape = m_MeshShapes[key]; // a mesh that cannot be read is not tried again this session

        const AssetType type = AssetManager::GetAssetType(collider.Mesh);
        if (type != AssetType::StaticMesh && type != AssetType::Mesh)
            return shape;
        const std::optional<CookedMesh> cooked = MeshImporter::ReadCookedMesh(Project::GetActiveAssetDirectory(), AssetManager::GetMetadata(collider.Mesh));
        if (!cooked)
        {
            SPDLOG_WARN("Mesh collider: the cooked mesh of asset {} cannot be read", static_cast<uint64_t>(collider.Mesh));
            return shape;
        }

        // The original triangles (the clusters of LOD level 0, as the ray tracing BLAS and HLOD take them), the copies of a vertex
        // that streaming pages hold welded back into one, in native units.
        const float nativePerWorld = PhysicsScale();
        JPH::VertexList vertices;
        JPH::IndexedTriangleList triangles;
        std::unordered_map<std::string, uint32_t> welded;
        const size_t first = std::min<size_t>(collider.SubmeshIndex, cooked->Submeshes.size());
        const size_t last = collider.SubmeshCount == UINT32_MAX ? cooked->Submeshes.size()
                                                                : std::min<size_t>(first + collider.SubmeshCount, cooked->Submeshes.size());
        for (size_t submesh = first; submesh < last; ++submesh)
        {
            const MeshData& data = cooked->Submeshes[submesh];
            for (size_t index = 0; index < data.Draws.size() && index < data.Bounds.size(); ++index)
            {
                if (data.Bounds[index].lodLevel != 0)
                    continue;
                const shaderio::MeshletDraw& draw = data.Draws[index];
                for (uint32_t triangle = 0; triangle < draw.triangleCount; ++triangle)
                {
                    uint32_t corners[3];
                    for (uint32_t corner = 0; corner < 3; ++corner)
                    {
                        const glm::vec3 position = data.Vertices[draw.globalVertexOffset +
                            data.MeshletVertices[draw.vertexOffset + data.MeshletTriangles[draw.triangleOffset + triangle * 3 + corner]]].pos * nativePerWorld;
                        auto [found, inserted] = welded.try_emplace(std::string(reinterpret_cast<const char*>(&position), sizeof(position)),
                                                                    static_cast<uint32_t>(vertices.size()));
                        if (inserted)
                            vertices.push_back(JPH::Float3(position.x, position.y, position.z));
                        corners[corner] = found->second;
                    }
                    triangles.push_back(JPH::IndexedTriangle(corners[0], corners[1], corners[2], 0));
                }
            }
        }
        if (triangles.empty())
            return shape;

        JPH::Shape::ShapeResult result;
        if (convex)
        {
            JPH::Array<JPH::Vec3> points;
            points.reserve(vertices.size());
            for (const JPH::Float3& vertex : vertices)
                points.push_back(JPH::Vec3(vertex));
            result = JPH::ConvexHullShapeSettings(points).Create();
        }
        else
        {
            result = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
        }
        if (result.HasError())
            SPDLOG_ERROR("Mesh collider of asset {}: {}", static_cast<uint64_t>(collider.Mesh), result.GetError().c_str());
        else
            shape = result.Get();
        return shape;
    }

    JPH::BodyID JoltPhysics3DScene::BodyAtOrAbove(Entity entity) const
    {
        for (int depth = 0; entity && depth < 256; ++depth, entity = entity.GetParent())
        {
            if (const auto found = m_EntityToBodyMap.find(static_cast<uint32_t>(entity)); found != m_EntityToBodyMap.end())
                return found->second;
        }
        return JPH::BodyID();
    }

    void JoltPhysics3DScene::RemoveConstraints()
    {
        for (JPH::Ref<JPH::Constraint>& constraint : m_Constraints)
            m_PhysicsSystem->RemoveConstraint(constraint);
        m_Constraints.clear();
        static_cast<NoxCollisionGroupFilter*>(m_GroupFilter.GetPtr())->Apart.clear();
        m_ConstraintsDirty = true;
    }

    void JoltPhysics3DScene::RebuildConstraints()
    {
        RemoveConstraints();
        m_ConstraintsDirty = false;
        if (!m_Scene)
            return;
        auto* groups = static_cast<NoxCollisionGroupFilter*>(m_GroupFilter.GetPtr());
        JPH::BodyInterface& bodies = m_PhysicsSystem->GetBodyInterface();
        auto view = m_Scene->GetRegistry().view<PhysicsJoint3DComponent>();
        for (auto handle : view)
        {
            Entity entity{ handle, m_Scene };
            const PhysicsJoint3DComponent& joint = view.get<PhysicsJoint3DComponent>(handle);
            Entity connected = joint.ConnectedEntity != 0 ? m_Scene->GetEntityByUUID(joint.ConnectedEntity) : Entity();
            const JPH::BodyID first = BodyAtOrAbove(entity);
            const JPH::BodyID second = connected ? BodyAtOrAbove(connected) : JPH::BodyID();
            if (first.IsInvalid() || first == second)
                continue;

            // The frames: this entity's and the connected one's (the world: where this entity stands), in world space (native units).
            const WorldPose pose1 = WorldPoseOf(entity);
            const WorldPose pose2 = connected ? WorldPoseOf(connected) : pose1;
            JPH::SixDOFConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPosition1 = JoltUtils::ToJoltPosition(ToNative(pose1.Position));
            settings.mAxisX1 = JoltUtils::ToJolt(pose1.Rotation * glm::vec3(1, 0, 0));
            settings.mAxisY1 = JoltUtils::ToJolt(pose1.Rotation * glm::vec3(0, 1, 0));
            settings.mPosition2 = JoltUtils::ToJoltPosition(ToNative(pose2.Position));
            settings.mAxisX2 = JoltUtils::ToJolt(pose2.Rotation * glm::vec3(1, 0, 0));
            settings.mAxisY2 = JoltUtils::ToJolt(pose2.Rotation * glm::vec3(0, 1, 0));
            for (int axis = 0; axis < 6; ++axis)
            {
                const auto jointAxis = static_cast<JPH::SixDOFConstraintSettings::EAxis>(axis);
                const float toNative = axis < 3 ? PhysicsScale() : 1.0f; // translation in world units, rotation in radians
                switch (joint.Axes[axis])
                {
                case PhysicsJoint3DComponent::Motion::Locked: settings.MakeFixedAxis(jointAxis); break;
                case PhysicsJoint3DComponent::Motion::Limited: settings.SetLimitedAxis(jointAxis, joint.Min[axis] * toNative, joint.Max[axis] * toNative); break;
                default: settings.MakeFreeAxis(jointAxis); break;
                }
                if (axis < 3 && joint.LimitStiffness[axis] > 0.0f)
                    settings.mLimitsSpringSettings[axis] = JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping, joint.LimitStiffness[axis], joint.LimitDamping[axis]);
                const PhysicsJoint3DComponent::Drive& drive = joint.Drives[axis];
                if (drive.Enabled)
                {
                    const float limit = drive.MaxForce > 0.0f ? drive.MaxForce : FLT_MAX;
                    settings.mMotorSettings[axis] = JPH::MotorSettings(JPH::ESpringMode::StiffnessAndDamping, drive.Stiffness, drive.Damping, limit, limit);
                }
            }

            JPH::TwoBodyConstraint* constraint = bodies.CreateConstraint(&settings, first, second);
            if (!constraint)
                continue;
            auto* sixDOF = static_cast<JPH::SixDOFConstraint*>(constraint);
            // Drives: a position target with a spring (stiffness), else the velocity.
            JPH::Vec3 position = JPH::Vec3::sZero(), velocity = JPH::Vec3::sZero(), angularVelocity = JPH::Vec3::sZero();
            glm::vec3 angles(0.0f);
            for (int axis = 0; axis < 6; ++axis)
            {
                const PhysicsJoint3DComponent::Drive& drive = joint.Drives[axis];
                if (!drive.Enabled)
                    continue;
                const auto jointAxis = static_cast<JPH::SixDOFConstraintSettings::EAxis>(axis);
                sixDOF->SetMotorState(jointAxis, drive.Stiffness > 0.0f ? JPH::EMotorState::Position : JPH::EMotorState::Velocity);
                if (axis < 3)
                {
                    position.SetComponent(axis, drive.PositionTarget * PhysicsScale());
                    velocity.SetComponent(axis, drive.VelocityTarget * PhysicsScale());
                }
                else
                {
                    angles[axis - 3] = drive.PositionTarget;
                    angularVelocity.SetComponent(axis - 3, drive.VelocityTarget);
                }
            }
            sixDOF->SetTargetPositionCS(position);
            sixDOF->SetTargetVelocityCS(velocity);
            sixDOF->SetTargetAngularVelocityCS(angularVelocity);
            sixDOF->SetTargetOrientationCS(JoltUtils::ToJolt(glm::quat(angles)));
            m_PhysicsSystem->AddConstraint(constraint);
            bodies.ActivateConstraint(constraint);
            m_Constraints.emplace_back(constraint);

            if (!joint.EnableCollision && !second.IsInvalid())
            {
                const auto serialOf = [&](JPH::BodyID body) -> uint32_t
                {
                    const auto found = m_BodyToEntityMap.find(body.GetIndexAndSequenceNumber());
                    if (found == m_BodyToEntityMap.end())
                        return 0;
                    const auto serial = m_BodySerials.find(static_cast<uint32_t>(found->second));
                    return serial != m_BodySerials.end() ? serial->second : 0;
                };
                const uint32_t a = serialOf(first), b = serialOf(second);
                if (a && b)
                    groups->Apart.insert({ std::min(a, b), std::max(a, b) });
            }
        }
    }

    void JoltPhysics3DScene::DestroyBody(Entity entity)
    {
        if (!m_PhysicsSystem || !entity)
            return;
        // Constraints keep pointers to their bodies: they go first, and are made again before the next step.
        if (m_EntityToBodyMap.contains(static_cast<uint32_t>(entity)) && !m_Constraints.empty())
            RemoveConstraints();

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
                m_RollingResistance->Untrack(bodyId);

                m_BodyToEntityMap.erase(bodyId.GetIndexAndSequenceNumber());
            }
            m_EntityToBodyMap.erase(it);
            m_BodyPoses.erase(entIdVal);
        }

        if (entity.HasComponent<RigidBody3DComponent>())
            entity.GetComponent<RigidBody3DComponent>().RuntimeBodyID = 0xFFFFFFFF;
    }

    void JoltPhysics3DScene::SetMeshCollider(Entity entity, std::span<const glm::vec3> positions, std::span<const uint32_t> indices,
                                             float friction, float restitution)
    {
        RemoveMeshCollider(entity);
        if (!m_PhysicsSystem || !entity || positions.empty() || indices.size() < 3)
            return;

        // The entity's rotation and scale are baked into the vertices, the body stands at its position (double precision: the vertices
        // stay small, so they keep their precision far from the world's origin). From its local transform, current even when the
        // entity was made or moved this frame (before propagation ran).
        glm::dmat4 world = entity.GetComponent<TransformComponent>().GetTransform();
        if (m_Scene && entity.HasComponent<RelationshipComponent>())
        {
            if (Entity parent = m_Scene->GetEntityByUUID(entity.GetComponent<RelationshipComponent>().Parent);
                parent && parent.HasComponent<WorldTransformComponent>())
                world = parent.GetComponent<WorldTransformComponent>().WorldMatrix * world;
        }

        const glm::dvec3 bodyPosition(world[3]);
        const glm::mat4 basis = Math::RelativeTo(world, bodyPosition);
        JPH::VertexList vertices;
        vertices.reserve(positions.size());
        for (const glm::vec3& position : positions)
        {
            const glm::vec3 placed = ToNative(glm::vec3(basis * glm::vec4(position, 1.0f)));
            vertices.push_back(JPH::Float3(placed.x, placed.y, placed.z));
        }
        JPH::IndexedTriangleList triangles;
        triangles.reserve(indices.size() / 3);
        for (size_t corner = 0; corner + 2 < indices.size(); corner += 3)
        {
            if (indices[corner] < positions.size() && indices[corner + 1] < positions.size() && indices[corner + 2] < positions.size())
                triangles.push_back(JPH::IndexedTriangle(indices[corner], indices[corner + 1], indices[corner + 2], 0));
        }

        JPH::MeshShapeSettings shapeSettings(std::move(vertices), std::move(triangles));
        JPH::Shape::ShapeResult shape = shapeSettings.Create();
        if (shape.HasError())
        {
            SPDLOG_ERROR("Mesh collider for entity {}: {}", static_cast<uint32_t>(entity), shape.GetError().c_str());
            return;
        }

        JPH::BodyCreationSettings bodySettings(shape.Get(), JoltUtils::ToJoltPosition(ToNative(bodyPosition)), JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                               PhysicsLayers::NON_MOVING);
        bodySettings.mFriction = friction;
        bodySettings.mRestitution = restitution;
        bodySettings.mUserData = static_cast<uint64_t>(static_cast<uint32_t>(entity));

        JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
        JPH::Body* body = bodyInterface.CreateBody(bodySettings);
        if (!body)
        {
            SPDLOG_ERROR("Failed to create the mesh collider body for entity {} (too many bodies?)", static_cast<uint32_t>(entity));
            return;
        }
        bodyInterface.AddBody(body->GetID(), JPH::EActivation::DontActivate);
        m_MeshColliders[static_cast<uint32_t>(entity)] = body->GetID();
        m_BodyToEntityMap[body->GetID().GetIndexAndSequenceNumber()] = entity;
    }

    void JoltPhysics3DScene::RemoveMeshCollider(Entity entity)
    {
        if (!m_PhysicsSystem)
            return;
        auto found = m_MeshColliders.find(static_cast<uint32_t>(entity));
        if (found == m_MeshColliders.end())
            return;
        JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
        bodyInterface.RemoveBody(found->second);
        bodyInterface.DestroyBody(found->second);
        m_BodyToEntityMap.erase(found->second.GetIndexAndSequenceNumber());
        m_MeshColliders.erase(found);
    }

    // Ported from Jolt's Samples/Tests/Character/CharacterVirtualTest.cpp: the capsule shape is offset so the
    // character position is at the feet, the supporting volume is the lower sphere cap.
    void JoltPhysics3DScene::CreateCharacter(Entity entity)
    {
        if (!m_PhysicsSystem || !entity)
            return;

        const auto& controller = entity.GetComponent<CharacterController3DComponent>();
        const auto& transform = entity.GetComponent<TransformComponent>();

        glm::dvec3 worldPos = transform.Translation;
        if (entity.HasComponent<WorldTransformComponent>())
            worldPos = entity.GetComponent<WorldTransformComponent>().GetPosition();

        // controller.Radius / Height are world-unit lengths (script-facing); the shape and every native tolerance below are Jolt's own
        // unit -> ToNative once here.
        const float radius = ToNative(std::max(controller.Radius, 0.01f));
        const float halfHeight = ToNative(std::max(controller.Height, 0.0f) * 0.5f);

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
            settings, JoltUtils::ToJoltPosition(ToNative(worldPos)), JPH::Quat::sIdentity(), static_cast<uint64_t>(static_cast<uint32_t>(entity)), m_PhysicsSystem.get());

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

            // controller.JumpSpeed / MoveVelocity / StepHeight are world-unit lengths (script-facing); the velocity math below stays
            // entirely in Jolt's native unit (like groundVelocity / currentVelocity / gravity, all read from Jolt already), so each is
            // converted with ToNative at the one place it enters the math. state.Horizontal is therefore a native-unit velocity.
            JPH::Vec3 velocity;
            if (onGround && movingTowardsGround)
            {
                velocity = groundVelocity;
                if (controller.JumpSpeed > 0.0f)
                    velocity += up * ToNative(controller.JumpSpeed);
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
                const glm::vec3 wanted = ToNative(glm::vec3(controller.MoveVelocity.x, 0.0f, controller.MoveVelocity.z));
                const glm::vec3 toWanted = wanted - state.Horizontal;
                const float distance = glm::length(toWanted);
                const bool wantsToMove = glm::length(wanted) > 0.001f;
                const float rate = ToNative(wantsToMove ? controller.MaxAcceleration : controller.BrakingDeceleration) *
                                   (character->IsSupported() ? 1.0f : std::max(controller.AirControl, 0.0f));
                const float step = rate * dt;
                state.Horizontal = distance <= step || distance < 1e-5f ? wanted : state.Horizontal + toWanted * (step / distance);
            }
            velocity += JPH::Vec3(state.Horizontal.x, 0.0f, state.Horizontal.z);
            // The animation's root motion, as it is (not ramped: the clip already starts and stops the way it moves).
            const glm::vec3 rootMotion = ToNative(glm::vec3(controller.RootMotionVelocity.x, 0.0f, controller.RootMotionVelocity.z));
            velocity += JPH::Vec3(rootMotion.x, 0.0f, rootMotion.z);

            character->SetLinearVelocity(velocity);

            JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
            updateSettings.mStickToFloorStepDown = -up * 0.5f; // Jolt's own tolerance (Samples/CharacterVirtualTest.cpp): native, untouched
            updateSettings.mWalkStairsStepUp = up * ToNative(controller.StepHeight);

            character->ExtendedUpdate(dt, -up * gravity.Length(), updateSettings,
                m_PhysicsSystem->GetDefaultBroadPhaseLayerFilter(PhysicsLayers::CHARACTER),
                m_PhysicsSystem->GetDefaultLayerFilter(PhysicsLayers::CHARACTER),
                {}, {}, *m_TempAllocator);
            state.Current = ToWorld(JoltUtils::ToGLMPosition(character->GetPosition()));
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
            controller.Velocity = ToWorld(JoltUtils::ToGLM(character->GetLinearVelocity())); // script-facing: world units

            // Drawn between the last two fixed steps by how far the accumulator is into the next one.
            const float alpha = glm::clamp(m_Accumulator / c_FixedDeltaTime, 0.0f, 1.0f);
            glm::dvec3 position = glm::mix(state.Previous, state.Current, static_cast<double>(alpha));
            if (entity.HasComponent<RelationshipComponent>())
            {
                UUID parentUUID = entity.GetComponent<RelationshipComponent>().Parent;
                if (parentUUID != 0)
                {
                    Entity parentEntity = m_Scene->GetEntityByUUID(parentUUID);
                    if (parentEntity && parentEntity.HasComponent<WorldTransformComponent>())
                        position = glm::dvec3(glm::inverse(parentEntity.GetComponent<WorldTransformComponent>().WorldMatrix) * glm::dvec4(position, 1.0));
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

            const glm::dvec3 position = ToWorld(JoltUtils::ToGLMPosition(bodyInterface.GetPosition(bodyId)));
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

        if (m_ConstraintsDirty)
            RebuildConstraints();

        while (m_Accumulator >= c_FixedDeltaTime && steps < maxSubSteps)
        {
            StepCharacters(c_FixedDeltaTime);
            m_PhysicsSystem->Update(c_FixedDeltaTime, 1, m_TempAllocator.get(), m_JobSystem.get());
            m_RollingResistance->Apply(*m_PhysicsSystem, c_FixedDeltaTime);
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

                    glm::dvec3 worldPos;
                    glm::quat worldRot;
                    if (pose)
                    {
                        // Drawn between the last two fixed steps by how far the accumulator is into the next one.
                        const float alpha = glm::clamp(m_Accumulator / c_FixedDeltaTime, 0.0f, 1.0f);
                        worldPos = glm::mix(pose->Previous, pose->Current, static_cast<double>(alpha));
                        worldRot = glm::slerp(pose->PreviousRotation, pose->CurrentRotation, alpha);
                        if (!active && pose->Previous == pose->Current && pose->PreviousRotation == pose->CurrentRotation)
                            pose->NeedsSync = false; // settled: this frame writes the final pose
                    }
                    else
                    {
                        worldPos = ToWorld(JoltUtils::ToGLMPosition(bodyInterface.GetPosition(bodyId)));
                        worldRot = JoltUtils::ToGLM(bodyInterface.GetRotation(bodyId));
                    }

                    bool hasParent = false;
                    glm::dmat4 parentWorldInv(1.0);
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
                        const glm::dmat4 worldMat = glm::translate(glm::dmat4(1.0), worldPos) * glm::dmat4(glm::toMat4(worldRot)) *
                                                    glm::scale(glm::dmat4(1.0), glm::dvec3(transform.Scale));
                        const glm::dmat4 localMat = parentWorldInv * worldMat;

                        glm::dvec3 localScale, localSkew;
                        glm::dvec4 localPerspective;
                        glm::dquat localRot;
                        glm::dvec3 localPos;
                        glm::decompose(localMat, localScale, localRot, localPos, localSkew, localPerspective);

                        transform.Translation = localPos;
                        transform.Rotation = glm::vec3(glm::eulerAngles(glm::conjugate(localRot)));
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

    bool JoltPhysics3DScene::RayCast(const glm::dvec3& origin, const glm::vec3& direction, float maxDistance,
                                     RayCastHit& outHit, uint16_t layerMask)
    {
        if (!m_PhysicsSystem)
            return false;

        // origin / maxDistance are world-unit (the caller's, e.g. a script); Jolt sees native. direction is a unit vector: not a length,
        // never scaled. outHit.Distance / Position are worked out from the world-unit inputs, so they come out in world units already.
        JPH::RRayCast ray(JoltUtils::ToJoltPosition(ToNative(origin)), JoltUtils::ToJolt(direction * ToNative(maxDistance)));
        JPH::RayCastResult hit;

        if (m_PhysicsSystem->GetNarrowPhaseQuery().CastRay(ray, hit))
        {
            outHit.Hit = true;
            outHit.Fraction = hit.mFraction;
            outHit.Distance = hit.mFraction * maxDistance;
            outHit.Position = origin + glm::dvec3(direction * outHit.Distance);

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

    bool JoltPhysics3DScene::SphereCast(const glm::dvec3& origin, float radius, const glm::vec3& direction,
                                        float maxDistance, ShapeCastHit& outHit, uint16_t layerMask)
    {
        if (!m_PhysicsSystem)
            return false;

        JPH::SphereShape sphere(ToNative(radius));
        // The contact points come back relative to the base offset: the cast's own origin, so they keep their precision.
        const JPH::RVec3 base = JoltUtils::ToJoltPosition(ToNative(origin));
        JPH::RShapeCast shapeCast(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(base), JoltUtils::ToJolt(direction * ToNative(maxDistance)));

        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        m_PhysicsSystem->GetNarrowPhaseQuery().CastShape(shapeCast, JPH::ShapeCastSettings(), base, collector);

        if (collector.HadHit())
        {
            const JPH::ShapeCastResult& hit = collector.mHit;
            outHit.Hit = true;
            outHit.Fraction = hit.mFraction;
            outHit.Distance = hit.mFraction * maxDistance;
            outHit.ContactPosition = origin + glm::dvec3(ToWorld(JoltUtils::ToGLM(hit.mContactPointOn2)));
            outHit.ContactNormal = JoltUtils::ToGLM(-hit.mPenetrationAxis.Normalized());

            auto it = m_BodyToEntityMap.find(hit.mBodyID2.GetIndexAndSequenceNumber());
            if (it != m_BodyToEntityMap.end())
                outHit.HitEntity = it->second;

            return true;
        }

        outHit.Hit = false;
        return false;
    }

    std::vector<Entity> JoltPhysics3DScene::OverlapSphere(const glm::dvec3& center, float radius, uint16_t layerMask)
    {
        std::vector<Entity> results;
        if (!m_PhysicsSystem)
            return results;

        JPH::SphereShape sphere(ToNative(radius));
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
        m_PhysicsSystem->GetNarrowPhaseQuery().CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JoltUtils::ToJoltPosition(ToNative(center))), JPH::CollideShapeSettings(),
                                                        JoltUtils::ToJoltPosition(ToNative(center)), collector);

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
            // force = mass x acceleration (length / time^2): mass is unaffected by the world unit, so only the length side scales.
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
            bodyInterface.AddForce(it->second, JoltUtils::ToJolt(ToNative(force)));
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
            // impulse = mass x velocity: same reasoning as AddForce.
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem->GetBodyInterface();
            bodyInterface.AddImpulse(it->second, JoltUtils::ToJolt(ToNative(impulse)));
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
            bodyInterface.SetLinearVelocity(it->second, JoltUtils::ToJolt(ToNative(velocity)));
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
            return ToWorld(JoltUtils::ToGLM(bodyInterface.GetLinearVelocity(it->second)));
        }
        return glm::vec3(0.0f);
    }

    // Angular velocity is radians / second, not a length: never scaled by the physics unit boundary.
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
