#pragma once

#include <vector>

#include "Animator.h"
#include "AnimationSequence.h"
#include "Skeleton.h"

namespace Nox
{
    // One graph evaluation's worth of a skeleton's local pose, index-parallel to Skeleton::AllNodes (by
    // Node::Index). This is the value type carried on every "Pose" pin of an animation graph: the Clip node
    // produces one by sampling a clip's channels, Blend2D blends two of them, Output applies the final one to
    // the skeleton for rendering -- the exact same shape Animator::UpdateTransforms works with internally.
    struct AnimPose
    {
        std::vector<Animator::NodeTransform> LocalTransforms;

        void ResetToRestPose(const Skeleton& skeleton);
    };

    // Samples clip's channels at time into pose, resetting every node to the skeleton's rest pose first --
    // exactly steps 1-2 of Animator::UpdateTransforms, factored out so the graph's Clip node and the plain
    // Animator share one sampling implementation.
    void SampleClipPose(const AnimationSequence& clip, float time, const Skeleton& skeleton, AnimPose& pose);

    // Per-node lerp (translation/scale) + shortest-path slerp (rotation) of two same-skeleton poses. alpha is
    // clamped to [0, 1]; out may alias a or b.
    void BlendPoses(const AnimPose& a, const AnimPose& b, float alpha, AnimPose& out);

    // Several same-skeleton poses by weight (normalized here; empty poses are skipped): weighted translation and scale,
    // rotation as the normalized weighted sum of sign-aligned quaternions (the blend spaces' N-way blend).
    void BlendPosesWeighted(const std::vector<const AnimPose*>& poses, const std::vector<float>& weights, AnimPose& out);

    // Per node: out = lerp(base, blend, weights[node]) (Layered Blend per Bone); nodes past weights' end keep base.
    void BlendPosesPerNode(const AnimPose& base, const AnimPose& blend, const std::vector<float>& weights, AnimPose& out);

    // Additive animation: base with (additive relative to reference) on top, scaled by alpha -- translation offset, rotation
    // delta (additive * inverse(reference)) applied before base's rotation, scale ratio. reference is usually the additive
    // clip's first frame or the rest pose.
    void ApplyAdditivePose(const AnimPose& base, const AnimPose& additive, const AnimPose& reference, float alpha, AnimPose& out);

    // The notifies a playhead crossed going from `from` to `to` (seconds, the direction of play; `wrapped`: it passed the
    // clip's end -- or start, playing backwards -- once in between), appended to `out`.
    void CollectCrossedNotifies(const AnimationSequence& clip, float from, float to, bool wrapped, std::vector<std::string>& out);

    // Walks the skeleton hierarchy exactly like Animator::UpdateTransforms steps 3-4: pose's local TRS -> each
    // node's LocalMatrix/GlobalMatrix -> final skinning matrices (GlobalMatrix * InverseBindMatrix). Writes into
    // skeleton's Node objects (shared per-asset state, same as Animator does) and outFinalBoneTransforms. Takes
    // Skeleton by const& like Animator::UpdateTransforms does -- AllNodes is a vector of Node*, so the pointees
    // aren't const even through a const Skeleton&.
    void ApplyPoseToSkeleton(const AnimPose& pose, const Skeleton& skeleton, std::vector<glm::mat4>& outFinalBoneTransforms);
}
