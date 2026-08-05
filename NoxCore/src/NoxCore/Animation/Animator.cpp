#include "Animator.h"
#include "NoxCore/Core/Log.h"
#include <glm/gtx/quaternion.hpp>

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
        if (skeleton.AllNodes.empty())
            return;

        if (!m_IsPlaying || !m_CurrentAnimation || m_CurrentAnimation->Duration <= 0.0f)
        {
            UpdateTransforms(skeleton);
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

        UpdateTransforms(skeleton);
    }

    void Animator::UpdateTransforms(const Skeleton& skeleton)
    {
        // 1. Reset all nodes to their rest pose
        for (Node* node : skeleton.AllNodes)
        {
            if (!node) continue;
            node->Translation = node->RestTranslation;
            node->Rotation = node->RestRotation;
            node->Scale = node->RestScale;
            node->Matrix = node->RestMatrix;
            node->HasMatrix = node->HasRestMatrix;
        }

        // 2. Evaluate animation tracks and override animated node channels
        if (m_CurrentAnimation)
        {
            for (const auto& channel : m_CurrentAnimation->Channels)
            {
                int32_t nodeIndex = channel.TargetNodeIndex;

                if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(skeleton.AllNodes.size()))
                    continue;

                Node* node = skeleton.AllNodes[nodeIndex];
                if (!node) continue;

                // Clear matrix flag so animated TRS takes effect
                node->HasMatrix = false;

                if (!channel.PositionKeys.empty())
                    node->Translation = InterpolatePosition(m_CurrentTime, channel);
                if (!channel.RotationKeys.empty())
                    node->Rotation = InterpolateRotation(m_CurrentTime, channel);
                if (!channel.ScaleKeys.empty())
                    node->Scale = InterpolateScale(m_CurrentTime, channel);
            }
        }

        // 3. Recursively calculate node local and global matrices down the tree
        auto updateNodeRecursive = [](auto& self, Node* node) -> void {
            glm::mat4 trsMatrix = glm::translate(glm::mat4(1.0f), node->Translation) *
                                  glm::toMat4(node->Rotation) *
                                  glm::scale(glm::mat4(1.0f), node->Scale);

            if (node->HasMatrix)
            {
                node->LocalMatrix = node->Matrix;
            }
            else
            {
                node->LocalMatrix = trsMatrix;
            }

            if (node->Parent)
                node->GlobalMatrix = node->Parent->GlobalMatrix * node->LocalMatrix;
            else
                node->GlobalMatrix = node->LocalMatrix;

            for (Node* child : node->Children)
            {
                self(self, child);
            }
        };

        for (Node* root : skeleton.RootNodes)
        {
            updateNodeRecursive(updateNodeRecursive, root);
        }

        // 4. Populate final shader matrices using ONLY the Skin joints array
        if (!skeleton.Skins.empty() && skeleton.Skins[0] != nullptr)
        {
            const Skin* skin = skeleton.Skins[0];
            size_t jointCount = skin->Joints.size();
            m_FinalBoneTransforms.resize(jointCount);

            for (size_t i = 0; i < jointCount; ++i)
            {
                Node* jointNode = skin->Joints[i];
                if (jointNode)
                {
                    // Transforms vertex: Mesh Local -> Joint Bind Space -> Animated Skeleton Space
                    m_FinalBoneTransforms[i] = jointNode->GlobalMatrix * skin->InverseBindMatrices[i];
                }
                else
                {
                    m_FinalBoneTransforms[i] = glm::mat4(1.0f);
                }
            }
        }
        else
        {
            m_FinalBoneTransforms.clear();
        }
    }

    template<typename KeyType>
    size_t Animator::FindKeyframeIndex(float time, const std::vector<KeyType>& keys) const
    {
        auto it = std::upper_bound(keys.begin(), keys.end(), time,
            [](float t, const KeyType& key) {
                return t < key.Time;
            });

        if (it == keys.begin())
            return 0;

        return std::distance(keys.begin(), it) - 1;
    }

    glm::vec3 Animator::InterpolatePosition(float time, const NodeAnimationChannel& channel)
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

    glm::quat Animator::InterpolateRotation(float time, const NodeAnimationChannel& channel)
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

        glm::quat q0 = channel.RotationKeys[idx].Value;
        glm::quat q1 = channel.RotationKeys[nextIdx].Value;

        // Take shortest path
        if (glm::dot(q0, q1) < 0.0f)
        {
            q1 = -q1;
        }

        return glm::normalize(glm::slerp(q0, q1, factor));
    }

    glm::vec3 Animator::InterpolateScale(float time, const NodeAnimationChannel& channel)
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