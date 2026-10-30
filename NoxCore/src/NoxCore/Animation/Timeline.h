#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    // A sequencer timeline (docs/Editor_Features_Plan_2026-10.md A8; UE's Sequencer, Unity's Timeline, Godot's AnimationPlayer):
    // tracks of keyed entity properties over time -- doors, elevators, light flicker, camera moves, cutscenes. A
    // TimelinePlayerComponent plays it on its entity. A track's target is found from the player by a path of entity names ("" the
    // player itself, "Door/Hinge" its child Door's child Hinge), so one timeline drives every copy of a prefab.
    enum class TimelineProperty : uint8_t
    {
        Translation,    // TransformComponent, local
        Rotation,       // TransformComponent, Euler degrees
        Scale,          // TransformComponent
        LightColor,     // the target's directional / point / spot light
        LightIntensity,
        CameraFOV,      // the target's camera, vertical, degrees
        Event,          // keys carry names: the player's scripts receive OnAnimationEvent(name) when the playhead passes them
        CameraCut,      // keys carry a camera's name path from the player: from the key on, that camera is the primary one (Play)
        MorphWeight,    // the target's mesh: the weight of the morph target the track names (TimelineTrack::Parameter)
        SubTimeline,    // another timeline (TimelineTrack::Parameter: its asset handle) played on the target (UE's Subsequence track):
                        // each key starts a section, Value = (length, 0: the child's duration; start offset in the child; rate, 0: 1;
                        // loop, 1: the child loops within the section); the child's tracks find their targets from the track's target
        MaterialParameter, // the target's material parameter the track names (TimelineTrack::Parameter: "Base Color", "Roughness"...),
                           // over its MaterialComponent::Parameters, on every slot (MaterialComponent::SequencerParameters)
        Count
    };

    enum class TimelineInterpolation : uint8_t
    {
        Constant, // holds the key's value until the next key
        Linear,
        Smooth    // cubic, through the neighbouring keys (flat at the first and last key), like UE's auto tangents
    };

    struct TimelineKey
    {
        float Time = 0.0f;
        glm::vec4 Value{ 0.0f };
        TimelineInterpolation Interpolation = TimelineInterpolation::Smooth; // from this key to the next
        std::string Event;
    };

    struct TimelineTrack
    {
        std::string Target; // entity name path from the player ("" = the player)
        TimelineProperty Property = TimelineProperty::Translation;
        std::string Parameter; // MorphWeight: the morph target's name
        std::vector<TimelineKey> Keys; // sorted by time
        bool Muted = false;
    };

    class Timeline : public Asset
    {
    public:
        float Duration = 5.0f; // seconds
        bool Loop = false;
        std::vector<TimelineTrack> Tracks;

        static AssetType GetStaticType() { return AssetType::Timeline; }
        AssetType GetType() const override { return GetStaticType(); }
    };

    namespace TimelineEvaluation
    {
        const char* GetPropertyName(TimelineProperty property);
        // How many of a key's value components the property uses (3 for vectors and colors, 1 for numbers, 0 for events).
        int GetComponentCount(TimelineProperty property);
        // The same for a track (a Material Parameter track's count depends on the parameter it names).
        int GetComponentCount(const TimelineTrack& track);
        // The track's value at `time` (seconds); the first key's before it, the last key's after it.
        glm::vec4 Evaluate(const TimelineTrack& track, float time);
        // The event names of the keys a playhead passed going from `from` to `to` (`wrapped`: it went past the end, or the start
        // playing backwards, once in between), appended to `out`.
        void CollectEvents(const TimelineTrack& track, float from, float to, bool wrapped, std::vector<std::string>& out);
        // A Camera Cut track's camera path at `time`: the last key at or before it (the first key's before the first key).
        const std::string* CameraCutAt(const TimelineTrack& track, float time);
        // Tracks whose keys carry a name or a section (Event, CameraCut, SubTimeline) instead of a value to interpolate.
        bool HasNamedKeys(TimelineProperty property);
        // A Sub-Timeline track's section at `time`: the child timeline's time there (true), or false outside every section.
        // `childDuration`: the child's length (a section of length 0 runs that long). `section`: the key that starts it.
        bool SubTimelineAt(const TimelineTrack& track, float time, float childDuration, float& outChildTime, int* section = nullptr);
        void SortKeys(TimelineTrack& track);
    }
}
