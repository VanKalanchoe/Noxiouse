#include "NoxCore/Physics/Jolt/JoltContactEvents.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <algorithm>

namespace Nox
{
    void JoltContactEvents::SetReporting(JPH::BodyID body, bool report)
    {
        if (report)
            m_Reporting.insert(body.GetIndexAndSequenceNumber());
        else
            m_Reporting.erase(body.GetIndexAndSequenceNumber());
    }

    void JoltContactEvents::Clear()
    {
        m_Reporting.clear();
        std::lock_guard lock(m_Mutex);
        m_Contacts.clear();
    }

    void JoltContactEvents::Take(std::vector<Contact>& out)
    {
        out.clear();
        std::lock_guard lock(m_Mutex);
        out.swap(m_Contacts);
    }

    JPH::ValidateResult JoltContactEvents::OnContactValidate(const JPH::Body& body1, const JPH::Body& body2, JPH::RVec3Arg baseOffset,
                                                            const JPH::CollideShapeResult& result)
    {
        return m_Next ? m_Next->OnContactValidate(body1, body2, baseOffset, result) : JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void JoltContactEvents::OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                                           JPH::ContactSettings& settings)
    {
        if (m_Next)
            m_Next->OnContactAdded(body1, body2, manifold, settings);
        if (m_Reporting.empty() || body1.IsSensor() || body2.IsSensor() || manifold.mRelativeContactPointsOn1.empty())
            return;
        if (!m_Reporting.contains(body1.GetID().GetIndexAndSequenceNumber()) && !m_Reporting.contains(body2.GetID().GetIndexAndSequenceNumber()))
            return;
        // The middle of the contact points (on body 1's surface), and the bodies' velocities there: body 2 moving against the normal
        // (toward body 1) relative to body 1 is the speed they hit with.
        JPH::Vec3 sum = JPH::Vec3::sZero();
        for (const JPH::Vec3& point : manifold.mRelativeContactPointsOn1)
            sum += point;
        const JPH::RVec3 position = manifold.mBaseOffset + sum / static_cast<float>(manifold.mRelativeContactPointsOn1.size());
        const JPH::Vec3 relative = body2.GetPointVelocity(position) - body1.GetPointVelocity(position);
        const float approach = std::max(-relative.Dot(manifold.mWorldSpaceNormal), 0.0f);
        std::lock_guard lock(m_Mutex);
        if (m_Contacts.size() < kMaxContacts)
            m_Contacts.push_back({ body1.GetID(), body2.GetID(), position, manifold.mWorldSpaceNormal, approach });
    }

    void JoltContactEvents::OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                                               JPH::ContactSettings& settings)
    {
        if (m_Next)
            m_Next->OnContactPersisted(body1, body2, manifold, settings);
    }

    void JoltContactEvents::OnContactRemoved(const JPH::SubShapeIDPair& pair)
    {
        if (m_Next)
            m_Next->OnContactRemoved(pair);
    }
}
