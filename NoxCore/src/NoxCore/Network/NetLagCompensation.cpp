#include "NetLagCompensation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Nox::Net
{
    namespace
    {
        bool RaySphere(const glm::dvec3& origin, const glm::dvec3& direction, const glm::dvec3& center, double radius, double& t)
        {
            const glm::dvec3 m = origin - center;
            const double b = glm::dot(m, direction);
            const double c = glm::dot(m, m) - radius * radius;
            if (c > 0.0 && b > 0.0)
                return false; // outside and pointing away
            const double discriminant = b * b - c;
            if (discriminant < 0.0)
                return false;
            t = std::max(-b - std::sqrt(discriminant), 0.0); // (inside: at once)
            return true;
        }

        // A ray against a capsule: the segment a-b swept by radius.
        bool RayCapsule(const glm::dvec3& origin, const glm::dvec3& direction, const glm::dvec3& a, const glm::dvec3& b, double radius,
                        double& t, glm::dvec3& normal)
        {
            double best = std::numeric_limits<double>::infinity();
            const glm::dvec3 axis = b - a;
            const double length2 = glm::dot(axis, axis);
            // The side: a cylinder around the axis, between the two caps.
            if (length2 > 1e-12)
            {
                const glm::dvec3 m = origin - a;
                const double md = glm::dot(m, axis), nd = glm::dot(direction, axis);
                const glm::dvec3 mPerp = m - axis * (md / length2);
                const glm::dvec3 dPerp = direction - axis * (nd / length2);
                const double qa = glm::dot(dPerp, dPerp), qb = glm::dot(mPerp, dPerp), qc = glm::dot(mPerp, mPerp) - radius * radius;
                if (qa > 1e-12)
                {
                    const double discriminant = qb * qb - qa * qc;
                    if (discriminant >= 0.0)
                    {
                        const double hit = (-qb - std::sqrt(discriminant)) / qa;
                        const double along = (md + hit * nd) / length2;
                        if (hit >= 0.0 && along >= 0.0 && along <= 1.0)
                        {
                            best = hit;
                            const glm::dvec3 point = origin + direction * hit;
                            normal = glm::normalize(point - (a + axis * along));
                        }
                    }
                }
            }
            // The two ends.
            for (const glm::dvec3& end : { a, b })
            {
                double hit = 0.0;
                if (RaySphere(origin, direction, end, radius, hit) && hit < best)
                {
                    best = hit;
                    const glm::dvec3 offset = origin + direction * hit - end;
                    normal = glm::length(offset) > 1e-9 ? glm::normalize(offset) : -direction;
                }
            }
            if (!std::isfinite(best))
                return false;
            t = best;
            return true;
        }

        Hitbox Mix(const Hitbox& from, const Hitbox& to, double t)
        {
            Hitbox mixed = to;
            const float tf = static_cast<float>(t);
            mixed.Shape.Center = glm::mix(from.Shape.Center, to.Shape.Center, t);
            mixed.Shape.Rotation = glm::slerp(from.Shape.Rotation, to.Shape.Rotation, tf);
            mixed.Shape.HalfExtents = glm::mix(from.Shape.HalfExtents, to.Shape.HalfExtents, tf);
            mixed.Shape.Radius = glm::mix(from.Shape.Radius, to.Shape.Radius, tf);
            mixed.Shape.HalfHeight = glm::mix(from.Shape.HalfHeight, to.Shape.HalfHeight, tf);
            if (from.Shape.Kind != to.Shape.Kind)
                mixed.Shape = t < 0.5 ? from.Shape : to.Shape;
            return mixed;
        }
    }

    bool RayHitShape(const glm::dvec3& origin, const glm::dvec3& direction, double maxDistance, const HitShape& shape, double& distance,
                     glm::vec3& normal)
    {
        double t = 0.0;
        glm::dvec3 n(0.0, 1.0, 0.0);
        switch (shape.Kind)
        {
        case HitShape::Type::Sphere:
            if (!RaySphere(origin, direction, shape.Center, shape.Radius, t))
                return false;
            {
                const glm::dvec3 offset = origin + direction * t - shape.Center;
                n = glm::length(offset) > 1e-9 ? glm::normalize(offset) : -direction;
            }
            break;
        case HitShape::Type::Capsule:
        {
            const glm::dvec3 up = glm::dvec3(shape.Rotation * glm::vec3(0.0f, 1.0f, 0.0f)) * static_cast<double>(shape.HalfHeight);
            if (!RayCapsule(origin, direction, shape.Center - up, shape.Center + up, shape.Radius, t, n))
                return false;
            break;
        }
        case HitShape::Type::Box:
        {
            // In the box's own space: the slabs.
            const glm::dquat inverse = glm::conjugate(glm::dquat(shape.Rotation));
            const glm::dvec3 o = inverse * (origin - shape.Center);
            const glm::dvec3 d = inverse * direction;
            const glm::dvec3 half(shape.HalfExtents);
            double enter = 0.0, leave = maxDistance;
            int axisHit = -1;
            double sign = 1.0;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::abs(d[axis]) < 1e-12)
                {
                    if (o[axis] < -half[axis] || o[axis] > half[axis])
                        return false; // parallel to this slab and outside it
                    continue;
                }
                double t0 = (-half[axis] - o[axis]) / d[axis];
                double t1 = (half[axis] - o[axis]) / d[axis];
                double faceSign = -1.0;
                if (t0 > t1)
                {
                    std::swap(t0, t1);
                    faceSign = 1.0;
                }
                if (t0 > enter)
                {
                    enter = t0;
                    axisHit = axis;
                    sign = faceSign;
                }
                leave = std::min(leave, t1);
                if (enter > leave)
                    return false;
            }
            t = enter;
            glm::dvec3 local(0.0);
            if (axisHit >= 0)
                local[axisHit] = sign;
            n = axisHit >= 0 ? glm::dquat(shape.Rotation) * local : -direction; // (the ray starts inside)
            break;
        }
        }
        if (t > maxDistance)
            return false;
        distance = t;
        normal = glm::vec3(n);
        return true;
    }

    void LagCompensationHistory::Record(uint64_t tick, std::vector<Hitbox> hitboxes, double ticksToKeep)
    {
        if (!m_Frames.empty() && tick <= m_Frames.back().Tick)
            m_Frames.clear(); // (the server's clock went back: a new scene)
        std::sort(hitboxes.begin(), hitboxes.end(), [](const Hitbox& a, const Hitbox& b) { return a.Entity < b.Entity; });
        m_Frames.push_back({ tick, std::move(hitboxes) });
        while (m_Frames.size() > 1 && static_cast<double>(tick - m_Frames.front().Tick) > ticksToKeep)
            m_Frames.pop_front();
    }

    void LagCompensationHistory::HitboxesAt(double tick, std::vector<Hitbox>& out) const
    {
        out.clear();
        if (m_Frames.empty())
            return;
        tick = std::clamp(tick, static_cast<double>(m_Frames.front().Tick), static_cast<double>(m_Frames.back().Tick));
        // The two frames around it (frames are one tick apart, but a hitch may have skipped some).
        size_t next = 0;
        while (next + 1 < m_Frames.size() && static_cast<double>(m_Frames[next].Tick) < tick)
            ++next;
        const Frame& b = m_Frames[next];
        const Frame& a = next > 0 ? m_Frames[next - 1] : b;
        const double span = static_cast<double>(b.Tick) - static_cast<double>(a.Tick);
        const double t = span > 0.0 ? (tick - static_cast<double>(a.Tick)) / span : 1.0;
        // Entity by entity (both sorted): in both, in between; in one only (spawned or removed in between), the nearer frame's.
        size_t i = 0, j = 0;
        while (i < a.Hitboxes.size() || j < b.Hitboxes.size())
        {
            if (j >= b.Hitboxes.size() || (i < a.Hitboxes.size() && a.Hitboxes[i].Entity < b.Hitboxes[j].Entity))
            {
                if (t < 0.5)
                    out.push_back(a.Hitboxes[i]);
                ++i;
            }
            else if (i >= a.Hitboxes.size() || b.Hitboxes[j].Entity < a.Hitboxes[i].Entity)
            {
                if (t >= 0.5)
                    out.push_back(b.Hitboxes[j]);
                ++j;
            }
            else
            {
                // (An entity may have several hitboxes: matched in order.)
                const NetId entity = a.Hitboxes[i].Entity;
                while (i < a.Hitboxes.size() && j < b.Hitboxes.size() && a.Hitboxes[i].Entity == entity && b.Hitboxes[j].Entity == entity)
                    out.push_back(Mix(a.Hitboxes[i++], b.Hitboxes[j++], t));
                while (i < a.Hitboxes.size() && a.Hitboxes[i].Entity == entity)
                    ++i;
                while (j < b.Hitboxes.size() && b.Hitboxes[j].Entity == entity)
                    ++j;
            }
        }
    }

    bool LagCompensationHistory::Raycast(double tick, const glm::dvec3& origin, const glm::dvec3& direction, double maxDistance,
                                         ClientId shooter, RewoundHit& hit) const
    {
        std::vector<Hitbox> hitboxes;
        HitboxesAt(tick, hitboxes);
        bool found = false;
        double best = maxDistance;
        for (const Hitbox& hitbox : hitboxes)
        {
            if (shooter != InvalidClientId && hitbox.Owner == shooter)
                continue;
            double distance = 0.0;
            glm::vec3 normal;
            if (RayHitShape(origin, direction, best, hitbox.Shape, distance, normal) && (!found || distance < best))
            {
                found = true;
                best = distance;
                hit.Entity = hitbox.Entity;
                hit.Distance = distance;
                hit.Point = origin + direction * distance;
                hit.Normal = normal;
            }
        }
        return found;
    }

    double ClampViewTick(double claimed, double serverTick, double expectedDelayTicks, double toleranceTicks, double maxRewindTicks)
    {
        const double expected = serverTick - expectedDelayTicks;
        double tick = std::isfinite(claimed) ? claimed : expected;
        tick = std::clamp(tick, expected - toleranceTicks, expected + toleranceTicks);
        return std::clamp(tick, serverTick - maxRewindTicks, serverTick);
    }
}
