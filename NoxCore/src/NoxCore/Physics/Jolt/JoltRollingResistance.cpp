#include "NoxCore/Physics/Jolt/JoltRollingResistance.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <algorithm>

namespace Nox
{
    void JoltRollingResistance::Track(JPH::BodyID body, float coefficient, float radius)
    {
        std::lock_guard lock(m_Mutex);
        m_Bodies[body.GetIndexAndSequenceNumber()] = Tracked{ coefficient, radius };
    }

    void JoltRollingResistance::Untrack(JPH::BodyID body)
    {
        std::lock_guard lock(m_Mutex);
        m_Bodies.erase(body.GetIndexAndSequenceNumber());
    }

    void JoltRollingResistance::Clear()
    {
        std::lock_guard lock(m_Mutex);
        m_Bodies.clear();
    }

    void JoltRollingResistance::OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                                               JPH::ContactSettings&)
    {
        record(body1, body2, manifold);
    }

    void JoltRollingResistance::OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                                                   JPH::ContactSettings&)
    {
        record(body1, body2, manifold);
    }

    void JoltRollingResistance::record(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold)
    {
        if (m_Bodies.empty() || body1.IsSensor() || body2.IsSensor())
            return;
        // The manifold's normal moves body 2 out of body 1: it points into body 2, out of body 1.
        const std::pair<const JPH::Body*, JPH::Vec3> sides[2] = { { &body1, -manifold.mWorldSpaceNormal },
                                                                  { &body2, manifold.mWorldSpaceNormal } };
        for (int side = 0; side < 2; ++side)
        {
            const auto found = m_Bodies.find(sides[side].first->GetID().GetIndexAndSequenceNumber());
            if (found == m_Bodies.end())
                continue;
            const JPH::Vec3 normal = sides[side].second;
            const float support = std::max(0.0f, -normal.Dot(m_GravityDirection));
            std::lock_guard lock(m_Mutex);
            Tracked& tracked = found->second;
            if (!tracked.Touching || support > tracked.Support)
            {
                tracked.Touching = true;
                tracked.Support = support;
                tracked.Normal = normal;
                tracked.Other = sides[1 - side].first->GetID();
            }
        }
    }

    void JoltRollingResistance::Apply(JPH::PhysicsSystem& system, float dt)
    {
        const JPH::Vec3 gravity = system.GetGravity();
        const float gravityLength = gravity.Length();
        m_GravityDirection = gravityLength > 0.0f ? gravity / gravityLength : JPH::Vec3(0.0f, -1.0f, 0.0f);

        const JPH::BodyLockInterfaceNoLock& locks = system.GetBodyLockInterfaceNoLock(); // no step is running
        for (auto& [id, tracked] : m_Bodies)
        {
            if (!tracked.Touching)
                continue;
            tracked.Touching = false;
            if (tracked.Coefficient <= 0.0f || tracked.Support <= 0.0f)
                continue;

            JPH::Body* body = locks.TryGetBody(JPH::BodyID(id));
            if (!body || !body->IsDynamic() || !body->IsActive())
                continue;
            JPH::Body* other = locks.TryGetBody(tracked.Other);
            const bool otherDynamic = other && other->IsDynamic() && other->IsActive(); // a sleeping one holds still, as a static one

            // The rolling part of the relative angular velocity: about axes in the contact plane.
            const JPH::Vec3 normal = tracked.Normal;
            JPH::Vec3 relative = body->GetAngularVelocity() - (other ? other->GetAngularVelocity() : JPH::Vec3::sZero());
            relative -= normal * normal.Dot(relative);
            const float rolling = relative.Length();
            if (rolling < 1e-6f)
                continue;
            const JPH::Vec3 axis = relative / rolling;

            JPH::MotionProperties* motion = body->GetMotionProperties();
            const float mass = motion->GetInverseMass() > 0.0f ? 1.0f / motion->GetInverseMass() : 0.0f;
            const float normalForce = mass * gravityLength * motion->GetGravityFactor() * tracked.Support;
            // Angular impulse against the rolling, at most what stops it (the two bodies' inverse inertia about the axis).
            const JPH::Vec3 bodyResponse = motion->MultiplyWorldSpaceInverseInertiaByVector(body->GetRotation(), axis);
            const JPH::Vec3 otherResponse = otherDynamic
                ? other->GetMotionProperties()->MultiplyWorldSpaceInverseInertiaByVector(other->GetRotation(), axis) : JPH::Vec3::sZero();
            const float inverseInertia = axis.Dot(bodyResponse) + axis.Dot(otherResponse);
            if (inverseInertia <= 0.0f)
                continue;
            const float impulse = std::min(tracked.Coefficient * tracked.Radius * normalForce * dt, rolling / inverseInertia);

            body->SetAngularVelocityClamped(body->GetAngularVelocity() - bodyResponse * impulse);
            if (otherDynamic)
                other->SetAngularVelocityClamped(other->GetAngularVelocity() + otherResponse * impulse);
        }
    }
}
