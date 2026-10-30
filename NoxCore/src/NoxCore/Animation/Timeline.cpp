#include "Timeline.h"
#include "NoxCore/Renderer/DataTypes.h"

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
        case TimelineProperty::CameraCut: return "Camera Cut";
        case TimelineProperty::MorphWeight: return "Morph Weight";
        case TimelineProperty::SubTimeline: return "Sub-Timeline";
        case TimelineProperty::MaterialParameter: return "Material Parameter";
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
        case TimelineProperty::CameraFOV:
        case TimelineProperty::MorphWeight: return 1;
        default: return 0;
        }
    }

    int GetComponentCount(const TimelineTrack& track)
    {
        if (track.Property == TimelineProperty::MaterialParameter)
        {
            const MaterialParameter parameter = MaterialParameters::FromName(track.Parameter);
            return parameter == MaterialParameter::Count ? 0 : MaterialParameters::ComponentCount(parameter);
        }
        return GetComponentCount(track.Property);
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

    const std::string* CameraCutAt(const TimelineTrack& track, float time)
    {
        if (track.Property != TimelineProperty::CameraCut || track.Keys.empty())
            return nullptr;
        const TimelineKey* current = &track.Keys.front();
        for (const TimelineKey& key : track.Keys)
        {
            if (key.Time > time)
                break;
            current = &key;
        }
        return &current->Event;
    }

    bool HasNamedKeys(TimelineProperty property)
    {
        return property == TimelineProperty::Event || property == TimelineProperty::CameraCut || property == TimelineProperty::SubTimeline;
    }

    bool SubTimelineAt(const TimelineTrack& track, float time, float childDuration, float& outChildTime, int* section)
    {
        if (track.Property != TimelineProperty::SubTimeline)
            return false;
        // The last section that started at or before `time`.
        int found = -1;
        for (int k = 0; k < static_cast<int>(track.Keys.size()) && track.Keys[k].Time <= time; ++k)
            found = k;
        if (found < 0)
            return false;
        const glm::vec4& value = track.Keys[found].Value;
        const float rate = value.z != 0.0f ? value.z : 1.0f;
        const float child = std::max(childDuration, 0.0f);
        const float length = value.x > 0.0f ? value.x : (child > 0.0f ? child / std::abs(rate) : 0.0f);
        const float local = time - track.Keys[found].Time;
        if (local > length)
            return false;
        float childTime = value.y + local * rate;
        if (value.w > 0.5f && child > 0.0f)
            childTime = std::fmod(std::fmod(childTime, child) + child, child);
        outChildTime = std::clamp(childTime, 0.0f, child > 0.0f ? child : childTime);
        if (section)
            *section = found;
        return true;
    }

    void SortKeys(TimelineTrack& track)
    {
        std::stable_sort(track.Keys.begin(), track.Keys.end(), [](const TimelineKey& a, const TimelineKey& b) { return a.Time < b.Time; });
    }
}
