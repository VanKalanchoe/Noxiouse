#include "Timeline.h"

#include <algorithm>

namespace Nox::TimelineEvaluation
{
    const char* GetPropertyName(TimelineProperty property)
    {
        switch (property)
        {
        case TimelineProperty::Translation: return "Translation";
        case TimelineProperty::Rotation: return "Rotation";
        case TimelineProperty::Scale: return "Scale";
        case TimelineProperty::LightColor: return "Light Color";
        case TimelineProperty::LightIntensity: return "Light Intensity";
        case TimelineProperty::CameraFOV: return "Camera FOV";
        case TimelineProperty::Event: return "Event";
        default: return "Unknown";
        }
    }

    int GetComponentCount(TimelineProperty property)
    {
        switch (property)
        {
        case TimelineProperty::Translation:
        case TimelineProperty::Rotation:
        case TimelineProperty::Scale:
        case TimelineProperty::LightColor: return 3;
        case TimelineProperty::LightIntensity:
        case TimelineProperty::CameraFOV: return 1;
        default: return 0;
        }
    }

    glm::vec4 Evaluate(const TimelineTrack& track, float time)
    {
        const std::vector<TimelineKey>& keys = track.Keys;
        if (keys.empty())
            return glm::vec4(0.0f);
        if (keys.size() == 1 || time <= keys.front().Time)
            return keys.front().Value;
        if (time >= keys.back().Time)
            return keys.back().Value;

        // The segment [keys[next - 1], keys[next]] the time is in.
        const auto upper = std::upper_bound(keys.begin(), keys.end(), time, [](float t, const TimelineKey& key) { return t < key.Time; });
        const size_t next = static_cast<size_t>(upper - keys.begin());
        const TimelineKey& a = keys[next - 1];
        const TimelineKey& b = keys[next];
        const float span = b.Time - a.Time;
        if (span <= 1e-6f || a.Interpolation == TimelineInterpolation::Constant)
            return a.Value;
        const float t = (time - a.Time) / span;
        if (a.Interpolation == TimelineInterpolation::Linear)
            return glm::mix(a.Value, b.Value, t);

        // Cubic Hermite: each key's tangent the slope between its neighbours (Catmull-Rom on uneven spacing), flat at the ends.
        auto tangent = [&](size_t index) -> glm::vec4
        {
            if (index == 0 || index + 1 >= keys.size())
                return glm::vec4(0.0f);
            const float width = keys[index + 1].Time - keys[index - 1].Time;
            return width > 1e-6f ? (keys[index + 1].Value - keys[index - 1].Value) / width : glm::vec4(0.0f);
        };
        const glm::vec4 m0 = tangent(next - 1) * span;
        const glm::vec4 m1 = tangent(next) * span;
        const float t2 = t * t, t3 = t2 * t;
        return (2.0f * t3 - 3.0f * t2 + 1.0f) * a.Value + (t3 - 2.0f * t2 + t) * m0 + (-2.0f * t3 + 3.0f * t2) * b.Value + (t3 - t2) * m1;
    }

    void CollectEvents(const TimelineTrack& track, float from, float to, bool wrapped, std::vector<std::string>& out)
    {
        if (track.Property != TimelineProperty::Event || track.Muted)
            return;
        // Forward: (from, to], or (from, end] + [start, to] across the end; backward the mirror image.
        const bool forward = wrapped ? to < from : to >= from;
        for (const TimelineKey& key : track.Keys)
        {
            if (key.Event.empty())
                continue;
            bool crossed;
            if (forward)
                crossed = wrapped ? (key.Time > from || key.Time <= to) : (key.Time > from && key.Time <= to);
            else
                crossed = wrapped ? (key.Time < from || key.Time >= to) : (key.Time < from && key.Time >= to);
            if (crossed)
                out.push_back(key.Event);
        }
    }

    void SortKeys(TimelineTrack& track)
    {
        std::stable_sort(track.Keys.begin(), track.Keys.end(), [](const TimelineKey& a, const TimelineKey& b) { return a.Time < b.Time; });
    }
}
