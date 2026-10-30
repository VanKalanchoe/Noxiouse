#pragma once

#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    enum class AnimationInterpolation
    {
        Linear,
        Step,
        CubicSpline
    };

    template<typename T>
    struct Keyframe
    {
        float Time = 0.0f;
        T Value;
    };

    // Renamed from BoneAnimationChannel for accuracy, 
    // but you can keep the old name if you want.
    struct NodeAnimationChannel
    {
        std::string NodeName;
        int32_t TargetNodeIndex = -1; // This now matches Node::Index in Skeleton::AllNodes

        std::vector<Keyframe<glm::vec3>> PositionKeys;
        std::vector<Keyframe<glm::quat>> RotationKeys;
        std::vector<Keyframe<glm::vec3>> ScaleKeys;

        AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
    };

    // A morph target's weight over a clip (glTF's "weights" animation path; Unreal's morph target curves): by target name, on the
    // node whose mesh has the target.
    struct MorphCurve
    {
        std::string NodeName;
        int32_t TargetNodeIndex = -1;
        std::string Target;
        std::vector<Keyframe<float>> Keys;
        AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
    };

    // A morph target's weight at one moment (a sampled MorphCurve, or blended ones): what an animation hands to the mesh.
    struct MorphWeightSample
    {
        int32_t TargetNodeIndex = -1; // the node whose mesh has it (node animation routes it there)
        std::string Target;
        float Weight = 0.0f;
    };

    // A curve's weight at `time` (seconds): held before the first and after the last key; Step holds each key; CubicSpline keys are
    // read as their values (the tangents are dropped at import), interpolated linearly.
    inline float SampleMorphCurve(const MorphCurve& curve, float time)
    {
        if (curve.Keys.empty())
            return 0.0f;
        if (time <= curve.Keys.front().Time)
            return curve.Keys.front().Value;
        if (time >= curve.Keys.back().Time)
            return curve.Keys.back().Value;
        size_t next = 1;
        while (next < curve.Keys.size() && curve.Keys[next].Time <= time)
            ++next;
        const Keyframe<float>& a = curve.Keys[next - 1];
        const Keyframe<float>& b = curve.Keys[next];
        if (curve.Interpolation == AnimationInterpolation::Step || b.Time <= a.Time)
            return a.Value;
        return a.Value + (b.Value - a.Value) * ((time - a.Time) / (b.Time - a.Time));
    }

    // A named moment of a clip (UE's anim notify): "FootL" at 0.25 s. Raised when a playhead crosses it -- by a Clip node, a
    // blend space's leading clip or a montage -- and handed to the entity's scripts as OnAnimationEvent(name).
    // A sync marker (UE's anim sync marker: "L" / "R" where a foot plants) is not raised: blend spaces line their clips up by
    // their markers, so a walk and a run with different stride timing still plant the same foot together.
    struct AnimationNotify
    {
        float Time = 0.0f;
        std::string Name;
        bool SyncMarker = false;
    };

    class AnimationSequence : public Asset
    {
    public:
        AnimationSequence() = default;
        virtual ~AnimationSequence() = default;

        // Delete copy and move operations due to reference counting (std::atomic)
        AnimationSequence(const AnimationSequence&) = delete;
        AnimationSequence& operator=(const AnimationSequence&) = delete;
        AnimationSequence(AnimationSequence&&) = delete;
        AnimationSequence& operator=(AnimationSequence&&) = delete;
        
        std::string Name;
        float Duration = 0.0f;
        float TicksPerSecond = 24.0f;
        std::vector<NodeAnimationChannel> Channels;
        std::vector<MorphCurve> MorphCurves;
        // Kept next to the cooked clip in <clip>.nanim.notifies (YAML), so a re-cook of the clip keeps them.
        std::vector<AnimationNotify> Notifies;

        static AssetType GetStaticType() { return AssetType::AnimationSequence; }
        virtual AssetType GetType() const override { return GetStaticType(); }
    };
}