#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <mutex>
#include <unordered_set>
#include <vector>

namespace Nox
{
    // Jolt's contact listener (it takes one): hands every contact to the next listener (rolling resistance) and notes the contacts
    // that begin on bodies that report them (Unreal's Simulation Generates Hit Events, Unity's OnCollisionEnter): where, the normal,
    // and how fast the two closed in along it -- their velocities at the contact point before the solver resolved it.
    class JoltContactEvents final : public JPH::ContactListener
    {
    public:
        struct Contact
        {
            JPH::BodyID Body1, Body2;
            JPH::RVec3 Position;  // the contact points' middle
            JPH::Vec3 Normal;     // out of body 1, into body 2
            float ApproachSpeed;  // native units per second (>= 0)
        };

        explicit JoltContactEvents(JPH::ContactListener* next) : m_Next(next) {}

        // Between steps (not while one runs).
        void SetReporting(JPH::BodyID body, bool report);
        void Clear();
        void Take(std::vector<Contact>& out); // the contacts begun since the last Take

        JPH::ValidateResult OnContactValidate(const JPH::Body& body1, const JPH::Body& body2, JPH::RVec3Arg baseOffset,
                                              const JPH::CollideShapeResult& result) override;
        void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override;
        void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                                JPH::ContactSettings& settings) override;
        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override;

    private:
        JPH::ContactListener* m_Next = nullptr;
        std::unordered_set<uint32_t> m_Reporting; // BodyID::GetIndexAndSequenceNumber; read by the step's jobs, changed between steps
        std::vector<Contact> m_Contacts;          // (m_Mutex: the step's jobs add to it concurrently)
        std::mutex m_Mutex;
        static constexpr size_t kMaxContacts = 4096; // a pile-up cannot grow it without bound between two Takes
    };
}
