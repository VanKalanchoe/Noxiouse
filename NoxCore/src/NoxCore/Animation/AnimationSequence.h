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

    struct BoneAnimationChannel
    {
        std::string BoneName;
        int32_t BoneIndex = -1;

        std::vector<Keyframe<glm::vec3>> PositionKeys;
        std::vector<Keyframe<glm::quat>> RotationKeys;
        std::vector<Keyframe<glm::vec3>> ScaleKeys;

        AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
    };

    class AnimationSequence : public Asset
    {
    public:
        AnimationSequence() = default;
        AnimationSequence(const AnimationSequence&) = default;
        AnimationSequence& operator=(const AnimationSequence&) = default;
        AnimationSequence(AnimationSequence&&) noexcept = default;
        AnimationSequence& operator=(AnimationSequence&&) noexcept = default;
        
        std::string Name;
        float Duration = 0.0f;
        float TicksPerSecond = 24.0f;
        std::vector<BoneAnimationChannel> Channels;

        static AssetType GetStaticType() { return AssetType::AnimationSequence; }
        virtual AssetType GetType() const override { return GetStaticType(); }
    };
}