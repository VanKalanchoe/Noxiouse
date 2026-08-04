#include "Animator.h"
#include "NoxCore/Core/Log.h"

namespace Nox
{
    Animator::Animator(const Ref<AnimationSequence>& animation)
    {
        PlayAnimation(animation);
    }

    void Animator::PlayAnimation(const Ref<AnimationSequence>& animation)
    {
        m_CurrentAnimation = animation;
        m_CurrentTime = 0.0f;
        m_IsPlaying = (animation != nullptr);
    }

    void Animator::Stop()
    {
        m_IsPlaying = false;
        m_CurrentTime = 0.0f;
    }

    void Animator::Pause()
    {
        m_IsPlaying = false;
    }

    void Animator::Resume()
    {
        if (m_CurrentAnimation)
            m_IsPlaying = true;
    }

    void Animator::Update(float deltaTime, const Skeleton& skeleton)
    {
        if (skeleton.Bones.empty())
            return;

        if (!m_IsPlaying || !m_CurrentAnimation || m_CurrentAnimation->Duration <= 0.0f)
        {
            UpdateBoneTransforms(skeleton);
            return;
        }

        // Advance playback time
        m_CurrentTime += deltaTime * m_PlaybackSpeed;

        // Loop / clamp boundaries
        if (m_IsLooping)
        {
            m_CurrentTime = std::fmod(m_CurrentTime, m_CurrentAnimation->Duration);
            if (m_CurrentTime < 0.0f)
                m_CurrentTime += m_CurrentAnimation->Duration;
        }
        else
        {
            if (m_CurrentTime >= m_CurrentAnimation->Duration)
            {
                m_CurrentTime = m_CurrentAnimation->Duration;
                m_IsPlaying = false;
            }
        }

        UpdateBoneTransforms(skeleton);
    }

   void Animator::UpdateBoneTransforms(const Skeleton& skeleton)
{
    size_t boneCount = skeleton.Bones.size();
    m_GlobalBoneTransforms.resize(boneCount);
    m_FinalBoneTransforms.resize(boneCount);

    // 1. Initialize local TRS matrices to the rest pose
    std::vector<glm::mat4> localTransforms(boneCount);
    for (size_t i = 0; i < boneCount; ++i)
    {
        localTransforms[i] = skeleton.Bones[i].LocalRestTransform;
    }

    // 2. Evaluate animation tracks and override animated channels
    if (m_CurrentAnimation)
    {
        for (const auto& channel : m_CurrentAnimation->Channels)
        {
            int32_t boneIndex = channel.BoneIndex;

            if (boneIndex < 0 || boneIndex >= static_cast<int32_t>(boneCount))
            {
                boneIndex = skeleton.FindBoneIndex(channel.BoneName);
            }

            if (boneIndex < 0 || boneIndex >= static_cast<int32_t>(boneCount))
                continue;

            glm::vec3 pos = InterpolatePosition(m_CurrentTime, channel);
            glm::quat rot = InterpolateRotation(m_CurrentTime, channel);
            glm::vec3 scale = InterpolateScale(m_CurrentTime, channel);

            glm::mat4 T = glm::translate(glm::mat4(1.0f), pos);
            glm::mat4 R = glm::toMat4(rot);
            glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);

            localTransforms[boneIndex] = T * R * S;
        }
    }

    // 3. Recursively calculate global hierarchy transforms (safely handles out-of-order joints)
    std::vector<bool> calculated(boneCount, false);

    std::function<void(int32_t)> computeGlobalTransform = [&](int32_t i) {
        if (calculated[i]) return;
        calculated[i] = true;

        const BoneInfo& bone = skeleton.Bones[i];
        int32_t parentIndex = bone.ParentIndex;

        if (parentIndex >= 0 && parentIndex < static_cast<int32_t>(boneCount))
        {
            // Ensure parent is computed first
            computeGlobalTransform(parentIndex);
            m_GlobalBoneTransforms[i] = m_GlobalBoneTransforms[parentIndex] * localTransforms[i];
        }
        else
        {
            m_GlobalBoneTransforms[i] = localTransforms[i];
        }
    };

    // Trigger calculation for all bones
    for (size_t i = 0; i < boneCount; ++i)
    {
        computeGlobalTransform(static_cast<int32_t>(i));
        m_FinalBoneTransforms[i] = m_GlobalBoneTransforms[i] * skeleton.Bones[i].InverseBindMatrix;
    }
}

    template<typename KeyType>
    size_t Animator::FindKeyframeIndex(float time, const std::vector<KeyType>& keys) const
    {
        // Binary search using std::upper_bound -> O(log N)
        auto it = std::upper_bound(keys.begin(), keys.end(), time,
            [](float t, const KeyType& key) {
                return t < key.Time;
            });

        if (it == keys.begin())
            return 0;

        return std::distance(keys.begin(), it) - 1;
    }

    glm::vec3 Animator::InterpolatePosition(float time, const BoneAnimationChannel& channel)
    {
        if (channel.PositionKeys.empty())
            return glm::vec3(0.0f);

        if (channel.PositionKeys.size() == 1 || time <= channel.PositionKeys.front().Time)
            return channel.PositionKeys.front().Value;

        if (time >= channel.PositionKeys.back().Time)
            return channel.PositionKeys.back().Value;

        size_t idx = FindKeyframeIndex(time, channel.PositionKeys);
        size_t nextIdx = idx + 1;

        if (channel.Interpolation == AnimationInterpolation::Step)
            return channel.PositionKeys[idx].Value;

        float t0 = channel.PositionKeys[idx].Time;
        float t1 = channel.PositionKeys[nextIdx].Time;
        float factor = (time - t0) / (t1 - t0);

        return glm::mix(channel.PositionKeys[idx].Value, channel.PositionKeys[nextIdx].Value, factor);
    }

    glm::quat Animator::InterpolateRotation(float time, const BoneAnimationChannel& channel)
    {
        if (channel.RotationKeys.empty())
            return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        if (channel.RotationKeys.size() == 1 || time <= channel.RotationKeys.front().Time)
            return glm::normalize(channel.RotationKeys.front().Value);

        if (time >= channel.RotationKeys.back().Time)
            return glm::normalize(channel.RotationKeys.back().Value);

        size_t idx = FindKeyframeIndex(time, channel.RotationKeys);
        size_t nextIdx = idx + 1;

        if (channel.Interpolation == AnimationInterpolation::Step)
            return glm::normalize(channel.RotationKeys[idx].Value);

        float t0 = channel.RotationKeys[idx].Time;
        float t1 = channel.RotationKeys[nextIdx].Time;
        float factor = (time - t0) / (t1 - t0);

        // Spherical linear interpolation (SLERP) for smooth quat rotation
        return glm::normalize(glm::slerp(channel.RotationKeys[idx].Value, channel.RotationKeys[nextIdx].Value, factor));
    }

    glm::vec3 Animator::InterpolateScale(float time, const BoneAnimationChannel& channel)
    {
        if (channel.ScaleKeys.empty())
            return glm::vec3(1.0f);

        if (channel.ScaleKeys.size() == 1 || time <= channel.ScaleKeys.front().Time)
            return channel.ScaleKeys.front().Value;

        if (time >= channel.ScaleKeys.back().Time)
            return channel.ScaleKeys.back().Value;

        size_t idx = FindKeyframeIndex(time, channel.ScaleKeys);
        size_t nextIdx = idx + 1;

        if (channel.Interpolation == AnimationInterpolation::Step)
            return channel.ScaleKeys[idx].Value;

        float t0 = channel.ScaleKeys[idx].Time;
        float t1 = channel.ScaleKeys[nextIdx].Time;
        float factor = (time - t0) / (t1 - t0);

        return glm::mix(channel.ScaleKeys[idx].Value, channel.ScaleKeys[nextIdx].Value, factor);
    }
}