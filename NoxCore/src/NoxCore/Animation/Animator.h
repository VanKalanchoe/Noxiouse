#pragma once
#include "NoxCore/Animation/AnimationSequence.h"
#include "NoxCore/Animation/Skeleton.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <vector>
#include <algorithm>

namespace Nox
{
    class Animator
    {
    public:
        Animator() = default;
        explicit Animator(const Ref<AnimationSequence>& animation);

        // Animation Playback Control
        void PlayAnimation(const Ref<AnimationSequence>& animation);
        void Stop();
        void Pause();
        void Resume();

        // Main evaluation loop
        void Update(float deltaTime, const Skeleton& skeleton);

        // Parameters & Queries
        void SetLooping(bool loop) { m_IsLooping = loop; }
        void SetPlaybackSpeed(float speed) { m_PlaybackSpeed = speed; }
        void SetCurrentTime(float time) { m_CurrentTime = time; }

        float GetCurrentAnimationTime() const { return m_CurrentTime; }
        float GetPlaybackSpeed() const { return m_PlaybackSpeed; }
        bool IsLooping() const { return m_IsLooping; }
        bool IsPlaying() const { return m_IsPlaying; }
        
        Ref<AnimationSequence> GetCurrentAnimation() const { return m_CurrentAnimation; }

        // Returns final skinning matrices for the shader (GlobalPose * InvBindMatrix)
        const std::vector<glm::mat4>& GetFinalBoneTransforms() const { return m_FinalBoneTransforms; }
        
        // Returns unskinned global transforms for bone debug drawing
        const std::vector<glm::mat4>& GetGlobalBoneTransforms() const { return m_GlobalBoneTransforms; }

    private:
        void UpdateBoneTransforms(const Skeleton& skeleton);

        // Keyframe sampling helpers
        glm::vec3 InterpolatePosition(float time, const BoneAnimationChannel& channel);
        glm::quat InterpolateRotation(float time, const BoneAnimationChannel& channel);
        glm::vec3 InterpolateScale(float time, const BoneAnimationChannel& channel);

        // O(log N) Binary search for keyframe lookup
        template<typename KeyType>
        size_t FindKeyframeIndex(float time, const std::vector<KeyType>& keys) const;

    private:
        Ref<AnimationSequence> m_CurrentAnimation;
        float m_CurrentTime = 0.0f;
        float m_PlaybackSpeed = 1.0f;
        bool m_IsLooping = true;
        bool m_IsPlaying = false;

        std::vector<glm::mat4> m_GlobalBoneTransforms;
        std::vector<glm::mat4> m_FinalBoneTransforms;
    };
}