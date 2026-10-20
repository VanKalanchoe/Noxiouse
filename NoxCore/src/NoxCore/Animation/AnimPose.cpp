#include "AnimPose.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

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

    void BlendPosesWeighted(const std::vector<const AnimPose*>& poses, const std::vector<float>& weights, AnimPose& out)
    {
        size_t count = 0;
        float total = 0.0f;
        const AnimPose* first = nullptr;
        for (size_t i = 0; i < poses.size() && i < weights.size(); ++i)
        {
            if (!poses[i] || poses[i]->LocalTransforms.empty() || weights[i] <= 0.0f)
                continue;
            count = first ? std::min(count, poses[i]->LocalTransforms.size()) : poses[i]->LocalTransforms.size();
            first = first ? first : poses[i];
            total += weights[i];
        }
        if (!first)
        {
            out.LocalTransforms.clear();
            return;
        }

        std::vector<Animator::NodeTransform> blended(count);
        for (size_t node = 0; node < count; ++node)
        {
            glm::vec3 translation(0.0f);
            glm::vec3 scale(0.0f);
            glm::quat rotation(0.0f, 0.0f, 0.0f, 0.0f);
            const glm::quat reference = first->LocalTransforms[node].Rotation;
            for (size_t i = 0; i < poses.size() && i < weights.size(); ++i)
            {
                if (!poses[i] || poses[i]->LocalTransforms.size() <= node || weights[i] <= 0.0f)
                    continue;
                const float w = weights[i] / total;
                const Animator::NodeTransform& t = poses[i]->LocalTransforms[node];
                translation += t.Translation * w;
                scale += t.Scale * w;
                glm::quat q = t.Rotation;
                if (glm::dot(q, reference) < 0.0f)
                    q = -q;
                rotation.x += q.x * w;
                rotation.y += q.y * w;
                rotation.z += q.z * w;
                rotation.w += q.w * w;
            }
            blended[node].Translation = translation;
            blended[node].Scale = scale;
            blended[node].Rotation = glm::length(rotation) > 1e-6f ? glm::normalize(rotation) : reference;
        }
        out.LocalTransforms = std::move(blended);
    }

    void BlendPosesPerNode(const AnimPose& base, const AnimPose& blend, const std::vector<float>& weights, AnimPose& out)
    {
        const size_t count = std::min(base.LocalTransforms.size(), blend.LocalTransforms.size());
        std::vector<Animator::NodeTransform> result = base.LocalTransforms;
        for (size_t i = 0; i < count && i < weights.size(); ++i)
        {
            const float alpha = glm::clamp(weights[i], 0.0f, 1.0f);
            if (alpha <= 0.0f)
                continue;
            const Animator::NodeTransform& ta = base.LocalTransforms[i];
            const Animator::NodeTransform& tb = blend.LocalTransforms[i];
            glm::quat qb = tb.Rotation;
            if (glm::dot(ta.Rotation, qb) < 0.0f)
                qb = -qb;
            result[i].Translation = glm::mix(ta.Translation, tb.Translation, alpha);
            result[i].Rotation = glm::normalize(glm::slerp(ta.Rotation, qb, alpha));
            result[i].Scale = glm::mix(ta.Scale, tb.Scale, alpha);
        }
        out.LocalTransforms = std::move(result);
    }

    void ApplyAdditivePose(const AnimPose& base, const AnimPose& additive, const AnimPose& reference, float alpha, AnimPose& out)
    {
        const size_t count = std::min({ base.LocalTransforms.size(), additive.LocalTransforms.size(), reference.LocalTransforms.size() });
        std::vector<Animator::NodeTransform> result = base.LocalTransforms;
        alpha = glm::max(alpha, 0.0f);
        const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
        for (size_t i = 0; i < count; ++i)
        {
            const Animator::NodeTransform& b = base.LocalTransforms[i];
            const Animator::NodeTransform& a = additive.LocalTransforms[i];
            const Animator::NodeTransform& r = reference.LocalTransforms[i];
            glm::quat delta = glm::normalize(a.Rotation * glm::inverse(r.Rotation));
            if (delta.w < 0.0f)
                delta = -delta; // the short way round
            const glm::quat scaledDelta = glm::normalize(glm::slerp(identity, delta, alpha));
            result[i].Translation = b.Translation + (a.Translation - r.Translation) * alpha;
            result[i].Rotation = glm::normalize(scaledDelta * b.Rotation);
            const glm::vec3 ratio = a.Scale / glm::max(glm::abs(r.Scale), glm::vec3(1e-6f)) * glm::sign(r.Scale + glm::vec3(1e-12f));
            result[i].Scale = b.Scale * glm::mix(glm::vec3(1.0f), ratio, alpha);
        }
        out.LocalTransforms = std::move(result);
    }

    void CollectCrossedNotifies(const AnimationSequence& clip, float from, float to, bool wrapped, std::vector<std::string>& out)
    {
        if (clip.Notifies.empty() || (from == to && !wrapped))
            return;
        // Forward: (from, to], or (from, end] and [0, to] across the end. Backward: [to, from), or [0, from) and [to, end].
        const bool forward = wrapped ? to < from : to > from;
        for (const AnimationNotify& notify : clip.Notifies)
        {
            if (notify.SyncMarker)
                continue; // lines clips up, raises nothing
            const float t = notify.Time;
            bool crossed = false;
            if (forward)
                crossed = wrapped ? (t > from || t <= to) : (t > from && t <= to);
            else
                crossed = wrapped ? (t < from || t >= to) : (t < from && t >= to);
            if (crossed)
                out.push_back(notify.Name);
        }
    }

    void EvaluateSkeleton(const Skeleton& skeleton, const std::vector<glm::mat4>& localMatrices, std::vector<glm::mat4>& outGlobalMatrices,
                          std::vector<glm::mat4>& outFinalBoneTransforms)
    {
        // Parents before children, from the roots; a node's global matrix is its parent's times its local one.
        outGlobalMatrices.assign(localMatrices.size(), glm::mat4(1.0f));
        auto walk = [&](auto& self, const Node* node, const glm::mat4& parentGlobal) -> void
        {
            glm::mat4 global = parentGlobal;
            if (node->Index >= 0 && node->Index < static_cast<int32_t>(localMatrices.size()))
            {
                global = parentGlobal * localMatrices[node->Index];
                outGlobalMatrices[node->Index] = global;
            }
            for (const Node* child : node->Children)
                self(self, child, global);
        };
        for (const Node* root : skeleton.RootNodes)
        {
            if (root)
                walk(walk, root, glm::mat4(1.0f));
        }

        // Skinning matrices: the skin's joints' global matrices times their inverse bind matrices.
        outFinalBoneTransforms.clear();
        if (!skeleton.Skins.empty() && skeleton.Skins[0] != nullptr)
        {
            const Skin* skin = skeleton.Skins[0];
            outFinalBoneTransforms.resize(skin->Joints.size());
            for (size_t i = 0; i < skin->Joints.size(); ++i)
            {
                const Node* jointNode = skin->Joints[i];
                const bool valid = jointNode && jointNode->Index >= 0 && jointNode->Index < static_cast<int32_t>(outGlobalMatrices.size());
                outFinalBoneTransforms[i] = valid && i < skin->InverseBindMatrices.size()
                    ? outGlobalMatrices[jointNode->Index] * skin->InverseBindMatrices[i]
                    : glm::mat4(1.0f);
            }
        }
    }

    glm::mat4 NodeRestLocalMatrix(const Node& node)
    {
        if (node.HasRestMatrix)
            return node.RestMatrix;
        return glm::translate(glm::mat4(1.0f), node.RestTranslation) * glm::toMat4(node.RestRotation) * glm::scale(glm::mat4(1.0f), node.RestScale);
    }

    float SyncedClipTime(const AnimationSequence& leader, float leaderTime, const AnimationSequence& follower, float fallbackTime)
    {
        std::vector<const AnimationNotify*> leaderMarkers, followerMarkers;
        for (const AnimationNotify& notify : leader.Notifies)
            if (notify.SyncMarker)
                leaderMarkers.push_back(&notify);
        for (const AnimationNotify& notify : follower.Notifies)
            if (notify.SyncMarker)
                followerMarkers.push_back(&notify);
        if (leaderMarkers.empty() || followerMarkers.empty() || leader.Duration <= 0.0f || follower.Duration <= 0.0f)
            return fallbackTime;

        // The leader's markers around its playhead (cyclic: before the first one, the previous is the last one of the loop before)
        // and how far between them it is.
        size_t next = 0;
        while (next < leaderMarkers.size() && leaderMarkers[next]->Time <= leaderTime)
            ++next;
        const AnimationNotify& before = *leaderMarkers[(next + leaderMarkers.size() - 1) % leaderMarkers.size()];
        const AnimationNotify& after = *leaderMarkers[next % leaderMarkers.size()];
        float beforeTime = before.Time, afterTime = after.Time;
        if (next == 0)
            beforeTime -= leader.Duration;
        if (next == leaderMarkers.size())
            afterTime += leader.Duration;
        if (afterTime <= beforeTime)
            afterTime += leader.Duration; // a single marker: from it round to itself
        const float fraction = std::clamp((leaderTime - beforeTime) / std::max(afterTime - beforeTime, 1e-5f), 0.0f, 1.0f);

        // The same pair in the follower (its marker named like `before` whose next marker is named like `after`), the one nearest to
        // where it would be anyway; the same fraction of the way between them.
        float best = -1.0f, bestDistance = FLT_MAX;
        for (size_t i = 0; i < followerMarkers.size(); ++i)
        {
            const AnimationNotify& from = *followerMarkers[i];
            const AnimationNotify& to = *followerMarkers[(i + 1) % followerMarkers.size()];
            if (from.Name != before.Name || to.Name != after.Name)
                continue;
            float toTime = to.Time;
            if (toTime <= from.Time)
                toTime += follower.Duration;
            float time = from.Time + fraction * (toTime - from.Time);
            time = std::fmod(time, follower.Duration);
            if (time < 0.0f)
                time += follower.Duration;
            float distance = std::abs(time - fallbackTime);
            distance = std::min(distance, follower.Duration - distance);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = time;
            }
        }
        return best >= 0.0f ? best : fallbackTime;
    }

    void ApplyPoseToSkeleton(const AnimPose& pose, const Skeleton& skeleton, std::vector<glm::mat4>& outFinalBoneTransforms,
                             std::vector<glm::mat4>* outGlobalMatrices)
    {
        // The pose's local TRS for the nodes it has, the rest pose for the others (always TRS: a pose is never a baked matrix).
        std::vector<glm::mat4> local(skeleton.AllNodes.size(), glm::mat4(1.0f));
        for (const Node* node : skeleton.AllNodes)
        {
            if (!node || node->Index < 0 || node->Index >= static_cast<int32_t>(local.size()))
                continue;
            if (node->Index < static_cast<int32_t>(pose.LocalTransforms.size()))
            {
                const Animator::NodeTransform& t = pose.LocalTransforms[node->Index];
                local[node->Index] = glm::translate(glm::mat4(1.0f), t.Translation) * glm::toMat4(t.Rotation) * glm::scale(glm::mat4(1.0f), t.Scale);
            }
            else
                local[node->Index] = glm::translate(glm::mat4(1.0f), node->RestTranslation) * glm::toMat4(node->RestRotation) *
                                     glm::scale(glm::mat4(1.0f), node->RestScale);
        }
        std::vector<glm::mat4> globals;
        EvaluateSkeleton(skeleton, local, outGlobalMatrices ? *outGlobalMatrices : globals, outFinalBoneTransforms);
    }
}
