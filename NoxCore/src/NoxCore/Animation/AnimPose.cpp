#include "AnimPose.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Nox
{
    void AnimPose::ResetToRestPose(const Skeleton& skeleton)
    {
        LocalTransforms.assign(skeleton.AllNodes.size(), Animator::NodeTransform{});
        for (const Node* node : skeleton.AllNodes)
        {
            if (!node || node->Index < 0 || node->Index >= (int32_t)LocalTransforms.size())
                continue;

            Animator::NodeTransform& t = LocalTransforms[node->Index];
            t.Translation = node->RestTranslation;
            t.Rotation = node->RestRotation;
            t.Scale = node->RestScale;
        }
    }

    void SampleClipPose(const AnimationSequence& clip, float time, const Skeleton& skeleton, AnimPose& pose)
    {
        pose.ResetToRestPose(skeleton);

        for (const NodeAnimationChannel& channel : clip.Channels)
        {
            int32_t nodeIndex = channel.TargetNodeIndex;
            if (nodeIndex < 0 || nodeIndex >= (int32_t)pose.LocalTransforms.size())
                continue;

            Animator::NodeTransform& t = pose.LocalTransforms[nodeIndex];
            if (!channel.PositionKeys.empty())
                t.Translation = Animator::InterpolatePosition(time, channel);
            if (!channel.RotationKeys.empty())
                t.Rotation = Animator::InterpolateRotation(time, channel);
            if (!channel.ScaleKeys.empty())
                t.Scale = Animator::InterpolateScale(time, channel);
        }
    }

    void BlendPoses(const AnimPose& a, const AnimPose& b, float alpha, AnimPose& out)
    {
        alpha = glm::clamp(alpha, 0.0f, 1.0f);
        size_t count = std::min(a.LocalTransforms.size(), b.LocalTransforms.size());

        std::vector<Animator::NodeTransform> blended(count);
        for (size_t i = 0; i < count; i++)
        {
            const Animator::NodeTransform& ta = a.LocalTransforms[i];
            const Animator::NodeTransform& tb = b.LocalTransforms[i];

            glm::quat qa = ta.Rotation;
            glm::quat qb = tb.Rotation;
            if (glm::dot(qa, qb) < 0.0f) // shortest path, same as Animator::InterpolateRotation
                qb = -qb;

            blended[i].Translation = glm::mix(ta.Translation, tb.Translation, alpha);
            blended[i].Rotation = glm::normalize(glm::slerp(qa, qb, alpha));
            blended[i].Scale = glm::mix(ta.Scale, tb.Scale, alpha);
        }
        out.LocalTransforms = std::move(blended); // built into a temporary so out may alias a or b
    }

    void ApplyPoseToSkeleton(const AnimPose& pose, const Skeleton& skeleton, std::vector<glm::mat4>& outFinalBoneTransforms)
    {
        for (Node* node : skeleton.AllNodes)
        {
            if (!node)
                continue;

            if (node->Index >= 0 && node->Index < (int32_t)pose.LocalTransforms.size())
            {
                const Animator::NodeTransform& t = pose.LocalTransforms[node->Index];
                node->Translation = t.Translation;
                node->Rotation = t.Rotation;
                node->Scale = t.Scale;
            }
            else
            {
                node->Translation = node->RestTranslation;
                node->Rotation = node->RestRotation;
                node->Scale = node->RestScale;
            }
            node->HasMatrix = false; // the pose is always expressed as TRS, never a baked matrix
        }

        // Same recursive local/global matrix walk as Animator::UpdateTransforms step 3.
        auto updateNodeRecursive = [](auto& self, Node* node) -> void
        {
            glm::mat4 trsMatrix = glm::translate(glm::mat4(1.0f), node->Translation) *
                                  glm::toMat4(node->Rotation) *
                                  glm::scale(glm::mat4(1.0f), node->Scale);

            node->LocalMatrix = node->HasMatrix ? node->Matrix : trsMatrix;
            node->GlobalMatrix = node->Parent ? node->Parent->GlobalMatrix * node->LocalMatrix : node->LocalMatrix;

            for (Node* child : node->Children)
                self(self, child);
        };

        for (Node* root : skeleton.RootNodes)
            updateNodeRecursive(updateNodeRecursive, root);

        // Same final skinning matrix step as Animator::UpdateTransforms step 4.
        outFinalBoneTransforms.clear();
        if (!skeleton.Skins.empty() && skeleton.Skins[0] != nullptr)
        {
            const Skin* skin = skeleton.Skins[0];
            outFinalBoneTransforms.resize(skin->Joints.size());
            for (size_t i = 0; i < skin->Joints.size(); ++i)
            {
                Node* jointNode = skin->Joints[i];
                outFinalBoneTransforms[i] = jointNode
                    ? jointNode->GlobalMatrix * skin->InverseBindMatrices[i]
                    : glm::mat4(1.0f);
            }
        }
    }
}
