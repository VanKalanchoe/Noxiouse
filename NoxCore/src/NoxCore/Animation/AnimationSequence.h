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

        static AssetType GetStaticType() { return AssetType::AnimationSequence; }
        virtual AssetType GetType() const override { return GetStaticType(); }
    };
}