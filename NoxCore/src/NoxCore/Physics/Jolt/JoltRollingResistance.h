#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <mutex>
#include <unordered_map>

namespace Nox
{
    // Rolling resistance for Jolt, which has none (Box2D v3's rollingResistance, Bullet's rolling friction). A body rolling on
    // another feels a torque against its rolling of coefficient x radius x normal force, as a real ball or wheel does from the
    // deformation of what it rolls on: it slows down at a constant rate and stops. Damping is not that -- it slows a body everywhere,
    // in the air too, and only exponentially (a ball at 5 m/s with Jolt's default 0.05 still rolls after a minute).
    //
    // The contact listener notes, per tracked body, the contact that holds it up the most during the step; Apply then turns the
    // rolling part of the two bodies' relative angular velocity (not the spin about the contact normal) down by the resistance's
    // angular impulse, never past zero. Friction carries that over to the linear velocity in the next step, as in Box2D. The normal
    // force is taken from the body's weight along that contact normal (Jolt's listener does not report the solver's impulses): exact
    // for a body resting on what it rolls on, low for one pressed on harder.
    class JoltRollingResistance final : public JPH::ContactListener
    {
    public:
        // A dynamic body that rolls: its resistance coefficient and rolling radius (Jolt units).
        void Track(JPH::BodyID body, float coefficient, float radius);
        void Untrack(JPH::BodyID body);
        void Clear();

        // After each PhysicsSystem::Update, on the thread that steps (no step running): applies the step's contacts.
        void Apply(JPH::PhysicsSystem& system, float dt);

        void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override;
        void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                                JPH::ContactSettings& settings) override;

    private:
        // Called from the step's jobs, concurrently: m_Bodies does not change during a step, its entries' contacts do (m_Mutex).
        void record(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold);

        struct Tracked
        {
            float Coefficient = 0.0f;
            float Radius = 0.0f;
            bool Touching = false;          // this step
            float Support = 0.0f;           // how much the contact holds it up: the contact normal against gravity's direction
            JPH::Vec3 Normal = JPH::Vec3::sZero(); // that contact's normal, pointing into the body
            JPH::BodyID Other;
        };
        std::unordered_map<uint32_t, Tracked> m_Bodies; // by BodyID::GetIndexAndSequenceNumber
        std::mutex m_Mutex;
        JPH::Vec3 m_GravityDirection = JPH::Vec3(0.0f, -1.0f, 0.0f); // from the last Apply
    };
}
