// IK nodes of the animation graph (Backlog C6): Two Bone IK, Look At and Foot Placement -- Unreal's nodes of the same names,
// Godot's SkeletonIK3D / LookAtModifier3D. They work in the skeleton's model space (the character entity's own space) on the pose
// coming in. Targets come from scripts (AnimatorComponent::IKTargets, by name); foot placement uses physics rays the scene casts
// for it (AnimGraphEvalData::FootProbes, answered next frame in FootGround).

#include "AnimationGraphNodes.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "NoxCore/NodeGraph/GraphEvalContext.h"
#include "NoxCore/NodeGraph/NodeType.h"

namespace Nox
{
    namespace
    {
        const Node* FindBone(const Skeleton& skeleton, const std::string& name)
        {
            if (name.empty())
                return nullptr;
            for (const Node* node : skeleton.AllNodes)
            {
                if (node && node->Name == name)
                    return node;
            }
            return nullptr;
        }

        bool ValidIndex(const Node* node, const AnimPose& pose)
        {
            return node && node->Index >= 0 && node->Index < static_cast<int32_t>(pose.LocalTransforms.size());
        }

        // Every node's model-space matrix for the pose (parents before children), with the root bone where root motion leaves it.
        void ComputeGlobals(const Skeleton& skeleton, const AnimPose& pose, const AnimGraphEvalData& data, std::vector<glm::mat4>& out)
        {
            out.assign(skeleton.AllNodes.size(), glm::mat4(1.0f));
            auto visit = [&](auto& self, const Node* node, const glm::mat4& parent, uint32_t depth) -> void
            {
                if (!node || depth > 512)
                    return;
                glm::mat4 global = parent;
                if (node->Index >= 0 && node->Index < static_cast<int32_t>(out.size()))
                {
                    glm::vec3 translation = node->RestTranslation;
                    glm::quat rotation = node->RestRotation;
                    glm::vec3 scale = node->RestScale;
                    if (node->Index < static_cast<int32_t>(pose.LocalTransforms.size()))
                    {
                        const Animator::NodeTransform& t = pose.LocalTransforms[node->Index];
                        translation = t.Translation;
                        rotation = t.Rotation;
                        scale = t.Scale;
                    }
                    if (node->Index == data.InPlaceRootIndex)
                    {
                        const glm::vec3 position = glm::vec3(data.InPlaceRootParent * glm::vec4(translation, 1.0f));
                        translation = glm::vec3(glm::inverse(data.InPlaceRootParent) *
                                                glm::vec4(data.InPlaceRootRest.x, position.y, data.InPlaceRootRest.z, 1.0f));
                    }
                    global = parent * glm::translate(glm::mat4(1.0f), translation) * glm::toMat4(rotation) * glm::scale(glm::mat4(1.0f), scale);
                    out[node->Index] = global;
                }
                for (const Node* child : node->Children)
                    self(self, child, global, depth + 1);
            };
            for (const Node* root : skeleton.RootNodes)
                visit(visit, root, glm::mat4(1.0f), 0);
        }

        glm::vec3 Position(const glm::mat4& m) { return glm::vec3(m[3]); }

        glm::quat Rotation(const glm::mat4& m)
        {
            glm::mat3 basis(m);
            for (int axis = 0; axis < 3; ++axis)
            {
                const float length = glm::length(basis[axis]);
                if (length > 1e-8f)
                    basis[axis] /= length;
            }
            return glm::normalize(glm::quat_cast(basis));
        }

        glm::quat ParentRotation(const Node* node, const std::vector<glm::mat4>& globals)
        {
            return node->Parent && node->Parent->Index >= 0 && node->Parent->Index < static_cast<int32_t>(globals.size())
                ? Rotation(globals[node->Parent->Index]) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }

        // A bone given a new model-space rotation (its children follow).
        void SetGlobalRotation(AnimPose& pose, const Node* node, const glm::quat& global, const std::vector<glm::mat4>& globals)
        {
            pose.LocalTransforms[node->Index].Rotation = glm::normalize(glm::inverse(ParentRotation(node, globals)) * global);
        }

        float SafeAcos(float value) { return std::acos(std::clamp(value, -1.0f, 1.0f)); }

        // The shortest rotation taking direction `from` to `to` (both unit length).
        glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to)
        {
            const float cosine = glm::dot(from, to);
            if (cosine > 0.99999f)
                return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            glm::vec3 axis = glm::cross(from, to);
            if (glm::length(axis) < 1e-6f) // opposite: any perpendicular axis
                axis = glm::abs(from.x) < 0.9f ? glm::cross(from, glm::vec3(1.0f, 0.0f, 0.0f)) : glm::cross(from, glm::vec3(0.0f, 1.0f, 0.0f));
            return glm::angleAxis(SafeAcos(cosine), glm::normalize(axis));
        }

        // Analytic two-bone IK (law of cosines, as in Unreal's Two Bone IK and Holden's "Simple Two Joint IK"): the end bone's parent
        // (elbow / knee) and grandparent (shoulder / hip) turn so the end reaches `target` (clamped to the chain's reach). The bend stays
        // in the chain's current plane, turned towards `pole` when given. The end bone keeps its model-space rotation (a hand or foot
        // keeps its orientation). `globals` is current on entry and on return.
        bool SolveTwoBone(const Skeleton& skeleton, AnimPose& pose, const AnimGraphEvalData& data, const Node* end, const glm::vec3& target,
                          const glm::vec3* pole, std::vector<glm::mat4>& globals)
        {
            const Node* mid = end ? end->Parent : nullptr;
            const Node* upper = mid ? mid->Parent : nullptr;
            if (!ValidIndex(end, pose) || !ValidIndex(mid, pose) || !ValidIndex(upper, pose))
                return false;

            const glm::vec3 a = Position(globals[upper->Index]);
            const glm::vec3 b = Position(globals[mid->Index]);
            const glm::vec3 c = Position(globals[end->Index]);
            const glm::quat aGlobal = Rotation(globals[upper->Index]);
            const glm::quat bGlobal = Rotation(globals[mid->Index]);
            const glm::quat endGlobal = Rotation(globals[end->Index]);

            const float lab = glm::length(b - a);
            const float lcb = glm::length(c - b);
            const float toTarget = glm::length(target - a);
            if (lab < 1e-6f || lcb < 1e-6f || toTarget < 1e-6f || glm::length(c - a) < 1e-6f)
                return false;
            const float lat = std::clamp(toTarget, 1e-4f, (lab + lcb) * 0.9999f);

            const glm::vec3 ac = glm::normalize(c - a);
            const glm::vec3 ab = glm::normalize(b - a);
            const glm::vec3 bc = glm::normalize(c - b);
            const glm::vec3 at = glm::normalize(target - a);
            const float acAb0 = SafeAcos(glm::dot(ac, ab));
            const float baBc0 = SafeAcos(glm::dot(-ab, bc));
            const float acAt0 = SafeAcos(glm::dot(ac, at));
            const float acAb1 = SafeAcos((lcb * lcb - lab * lab - lat * lat) / (-2.0f * lab * lat));
            const float baBc1 = SafeAcos((lat * lat - lab * lab - lcb * lcb) / (-2.0f * lab * lcb));

            glm::vec3 axis0 = glm::cross(c - a, b - a);
            if (glm::length(axis0) < 1e-6f) // a straight chain: bend towards the pole, else any side
                axis0 = pole ? glm::cross(c - a, *pole - a) : glm::cross(c - a, glm::abs(ac.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f));
            if (glm::length(axis0) < 1e-6f)
                return false;
            axis0 = glm::normalize(axis0);
            glm::vec3 axis1 = glm::cross(c - a, target - a);
            const bool swing = glm::length(axis1) > 1e-6f;
            axis1 = swing ? glm::normalize(axis1) : axis0;

            const glm::quat r0 = glm::angleAxis(acAb1 - acAb0, glm::inverse(aGlobal) * axis0);
            const glm::quat r1 = glm::angleAxis(baBc1 - baBc0, glm::inverse(bGlobal) * axis0);
            const glm::quat r2 = swing ? glm::angleAxis(acAt0, glm::inverse(aGlobal) * axis1) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            Animator::NodeTransform& upperLocal = pose.LocalTransforms[upper->Index];
            Animator::NodeTransform& midLocal = pose.LocalTransforms[mid->Index];
            upperLocal.Rotation = glm::normalize(upperLocal.Rotation * (r0 * r2));
            midLocal.Rotation = glm::normalize(midLocal.Rotation * r1);
            ComputeGlobals(skeleton, pose, data, globals);

            // Turned around the hip -> target line so the knee / elbow points at the pole.
            if (pole)
            {
                const glm::vec3 line = glm::normalize(target - a);
                const glm::vec3 knee = Position(globals[mid->Index]) - a;
                const glm::vec3 wanted = *pole - a;
                glm::vec3 kneeFlat = knee - line * glm::dot(knee, line);
                glm::vec3 wantedFlat = wanted - line * glm::dot(wanted, line);
                if (glm::length(kneeFlat) > 1e-6f && glm::length(wantedFlat) > 1e-6f)
                {
                    kneeFlat = glm::normalize(kneeFlat);
                    wantedFlat = glm::normalize(wantedFlat);
                    const float angle = std::atan2(glm::dot(glm::cross(kneeFlat, wantedFlat), line), glm::dot(kneeFlat, wantedFlat));
                    const glm::quat upperNow = Rotation(globals[upper->Index]);
                    upperLocal.Rotation = glm::normalize(upperLocal.Rotation * glm::angleAxis(angle, glm::inverse(upperNow) * line));
                    ComputeGlobals(skeleton, pose, data, globals);
                }
            }

            SetGlobalRotation(pose, end, endGlobal, globals);
            ComputeGlobals(skeleton, pose, data, globals);
            return true;
        }

        void PassThrough(GraphEvalContext& ctx, uint32_t nodeIndex, AnimPose&& pose)
        {
            ctx.SetOutput(nodeIndex, 0, std::move(pose));
        }

        const AnimIKTarget* FindTarget(const AnimGraphEvalData& data, const std::string& name)
        {
            if (!data.IKTargets || name.empty())
                return nullptr;
            const auto found = data.IKTargets->find(name);
            return found != data.IKTargets->end() ? &found->second : nullptr;
        }

        // Two Bone IK: properties "End Bone" (hand / foot), "Target" (an IK target a script sets: Animator.SetIKTarget), "Pole"
        // (optional target the elbow / knee points at). Pin "Alpha" (0..1, times the target's weight).
        void EvaluateTwoBoneIK(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            AnimPose pose = ctx.GetInput<AnimPose>(nodeIndex, 0);
            const float alpha = std::clamp(ctx.GetInput<float>(nodeIndex, 1), 0.0f, 1.0f);
            const AnimIKTarget* target = data ? FindTarget(*data, ctx.GetProperty<std::string>(nodeIndex, "Target", std::string())) : nullptr;
            const Node* end = data && data->TargetSkeleton ? FindBone(*data->TargetSkeleton, ctx.GetProperty<std::string>(nodeIndex, "EndBone", std::string())) : nullptr;
            const float weight = target ? alpha * std::clamp(target->Weight, 0.0f, 1.0f) : 0.0f;
            if (pose.LocalTransforms.empty() || !end || weight <= 0.0f)
                return PassThrough(ctx, nodeIndex, std::move(pose));

            const AnimIKTarget* pole = FindTarget(*data, ctx.GetProperty<std::string>(nodeIndex, "Pole", std::string()));
            std::vector<glm::mat4> globals;
            ComputeGlobals(*data->TargetSkeleton, pose, *data, globals);
            AnimPose solved = pose;
            if (!SolveTwoBone(*data->TargetSkeleton, solved, *data, end, target->Position, pole ? &pole->Position : nullptr, globals))
                return PassThrough(ctx, nodeIndex, std::move(pose));
            AnimPose out;
            BlendPoses(pose, solved, weight, out);
            ctx.SetOutput(nodeIndex, 0, std::move(out));
        }

        // The bone's local axis that points forward: "Axis" 0 = automatic (the model's forward, +Z, as the bone has it at rest),
        // 1..6 = +X, -X, +Y, -Y, +Z, -Z.
        glm::vec3 ForwardAxis(const Node* bone, int32_t axis)
        {
            switch (axis)
            {
            case 1: return { 1.0f, 0.0f, 0.0f };
            case 2: return { -1.0f, 0.0f, 0.0f };
            case 3: return { 0.0f, 1.0f, 0.0f };
            case 4: return { 0.0f, -1.0f, 0.0f };
            case 5: return { 0.0f, 0.0f, 1.0f };
            case 6: return { 0.0f, 0.0f, -1.0f };
            default:
            {
                glm::quat rest(1.0f, 0.0f, 0.0f, 0.0f);
                for (const Node* node = bone; node; node = node->Parent)
                    rest = node->RestRotation * rest;
                return glm::normalize(glm::inverse(rest) * glm::vec3(0.0f, 0.0f, 1.0f));
            }
            }
        }

        // Look At's eased turn (Unreal's Look At Interpolation Time): the yaw and pitch it turns the bone by and how much of it applies.
        struct LookAtState
        {
            float Yaw = 0.0f;
            float Pitch = 0.0f;
            float Weight = 0.0f;
            bool Started = false;
        };

        // Look At: property "Bone" (head, neck, a spine bone) turns its forward axis towards the IK target "Target". The turn is a
        // yaw about the character's up axis and a pitch, each at most "MaxAngle" degrees -- not one rotation about the axis between
        // the two directions, which turns unstable as the target goes behind (the axis flips with every bob of the animation and
        // the head nods). Past the limit to the side the look fades out over the next 45 degrees, so a target behind lets the head go
        // back to the animation (Unity's look-at clamp does the same). The result eases over "InterpolationTime" seconds. Chain a few
        // (spine 0.3, neck 0.5, head 1.0) for a natural turn. Pin "Alpha".
        void EvaluateLookAt(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            AnimPose pose = ctx.GetInput<AnimPose>(nodeIndex, 0);
            const float alpha = std::clamp(ctx.GetInput<float>(nodeIndex, 1), 0.0f, 1.0f);
            const AnimIKTarget* target = data ? FindTarget(*data, ctx.GetProperty<std::string>(nodeIndex, "Target", std::string())) : nullptr;
            const Node* bone = data && data->TargetSkeleton ? FindBone(*data->TargetSkeleton, ctx.GetProperty<std::string>(nodeIndex, "Bone", std::string())) : nullptr;
            if (pose.LocalTransforms.empty() || !ValidIndex(bone, pose))
                return PassThrough(ctx, nodeIndex, std::move(pose));

            std::vector<glm::mat4> globals;
            ComputeGlobals(*data->TargetSkeleton, pose, *data, globals);
            const glm::quat global = Rotation(globals[bone->Index]);
            const glm::vec3 forward = glm::normalize(global * ForwardAxis(bone, ctx.GetProperty<int32_t>(nodeIndex, "Axis", 0)));
            const glm::vec3 up(0.0f, 1.0f, 0.0f); // model space: the character stands along +Y

            // What it wants this frame: the yaw and pitch to the target, clamped, and how much of it (target weight x Alpha x the fade
            // past the limit). No target: nothing, eased out like the rest.
            float wantedYaw = 0.0f, wantedPitch = 0.0f, wantedWeight = 0.0f;
            const glm::vec3 flat(forward.x, 0.0f, forward.z);
            if (target && glm::length(flat) > 1e-3f)
            {
                const glm::vec3 toTarget = target->Position - Position(globals[bone->Index]);
                if (glm::length(toTarget) > 1e-5f)
                {
                    const glm::vec3 wanted = glm::normalize(toTarget);
                    const glm::vec3 flatWanted(wanted.x, 0.0f, wanted.z);
                    const glm::vec3 f = glm::normalize(flat);
                    const float yaw = glm::length(flatWanted) > 1e-4f
                        ? std::atan2(glm::dot(up, glm::cross(f, glm::normalize(flatWanted))), glm::dot(f, glm::normalize(flatWanted))) : 0.0f;
                    const float pitch = std::asin(std::clamp(wanted.y, -1.0f, 1.0f)) - std::asin(std::clamp(forward.y, -1.0f, 1.0f));
                    const float limit = glm::radians(std::clamp(ctx.GetProperty<float>(nodeIndex, "MaxAngle", 70.0f), 0.0f, 180.0f));
                    const float beyond = std::abs(yaw) - limit;
                    const float fade = beyond > 0.0f ? 1.0f - glm::smoothstep(0.0f, glm::radians(45.0f), beyond) : 1.0f;
                    wantedYaw = std::clamp(yaw, -limit, limit);
                    wantedPitch = std::clamp(pitch, -limit, limit);
                    wantedWeight = alpha * std::clamp(target->Weight, 0.0f, 1.0f) * fade;
                }
            }

            LookAtState& state = ctx.GetState<LookAtState>(nodeIndex);
            const float time = std::max(ctx.GetProperty<float>(nodeIndex, "InterpolationTime", 0.15f), 0.0f);
            if (!state.Started || time <= 0.0f)
            {
                state = { wantedYaw, wantedPitch, wantedWeight, true };
            }
            else
            {
                const float ease = 1.0f - std::exp(-4.0f * std::max(data->DeltaTime, 0.0f) / time);
                state.Yaw += (wantedYaw - state.Yaw) * ease;
                state.Pitch += (wantedPitch - state.Pitch) * ease;
                state.Weight += (wantedWeight - state.Weight) * ease;
            }
            if (state.Weight <= 1e-4f)
                return PassThrough(ctx, nodeIndex, std::move(pose));

            // Yaw about up, then pitch about the turned forward's side axis (towards up for a positive pitch).
            const glm::quat yawTurn = glm::angleAxis(state.Yaw * state.Weight, up);
            const glm::vec3 turned = yawTurn * forward;
            glm::quat turn = yawTurn;
            const glm::vec3 side = glm::cross(turned, up);
            if (glm::length(side) > 1e-4f)
                turn = glm::angleAxis(state.Pitch * state.Weight, glm::normalize(side)) * yawTurn;
            SetGlobalRotation(pose, bone, glm::normalize(turn * global), globals);
            ctx.SetOutput(nodeIndex, 0, std::move(pose));
        }

        struct FootPlacementState
        {
            float Offset[2] = { 0.0f, 0.0f };
            float Pelvis = 0.0f;
            glm::quat Align[2] = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };
        };

        // Foot Placement (Unreal's leg IK / foot placement): each foot ("LeftFoot", "RightFoot") is raised or lowered onto what a ray
        // under it found ("TraceUp" / "TraceDown" meters above / below it), the "Pelvis" lowered so the lower foot can reach, and the
        // feet turned to the ground's slope ("AlignToGround", at most "MaxAlignAngle" degrees). Changes ease in at "Speed" (1/s). Only in
        // Play / Simulate (the rays need the physics scene); in the air the rays find nothing and the feet go back to the animation.
        void EvaluateFootPlacement(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            AnimPose pose = ctx.GetInput<AnimPose>(nodeIndex, 0);
            const float alpha = std::clamp(ctx.GetInput<float>(nodeIndex, 1), 0.0f, 1.0f);
            if (pose.LocalTransforms.empty() || !data || !data->TargetSkeleton || alpha <= 0.0f)
                return PassThrough(ctx, nodeIndex, std::move(pose));
            const Skeleton& skeleton = *data->TargetSkeleton;
            const Node* feet[2] = { FindBone(skeleton, ctx.GetProperty<std::string>(nodeIndex, "LeftFoot", std::string())),
                                    FindBone(skeleton, ctx.GetProperty<std::string>(nodeIndex, "RightFoot", std::string())) };
            const Node* pelvis = FindBone(skeleton, ctx.GetProperty<std::string>(nodeIndex, "Pelvis", std::string()));

            const float meter = data->ModelUnitsPerMeter;
            const float traceUp = std::max(ctx.GetProperty<float>(nodeIndex, "TraceUp", 0.5f), 0.0f);
            const float traceDown = std::max(ctx.GetProperty<float>(nodeIndex, "TraceDown", 0.6f), 0.0f);
            const float maxDrop = std::max(ctx.GetProperty<float>(nodeIndex, "MaxPelvisDrop", 0.35f), 0.0f) * meter;
            const float speed = std::max(ctx.GetProperty<float>(nodeIndex, "Speed", 12.0f), 0.0f);
            const bool align = ctx.GetProperty<bool>(nodeIndex, "AlignToGround", true);
            const float maxAlign = glm::radians(std::clamp(ctx.GetProperty<float>(nodeIndex, "MaxAlignAngle", 35.0f), 0.0f, 89.0f));
            const float blend = speed > 0.0f ? std::min(1.0f, speed * data->DeltaTime) : 1.0f;

            std::vector<glm::mat4> globals;
            ComputeGlobals(skeleton, pose, *data, globals);
            FootPlacementState& state = ctx.GetState<FootPlacementState>(nodeIndex);
            glm::vec3 animated[2]{};
            glm::vec3 normals[2] = { glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) };
            bool valid[2] = { false, false };
            for (int side = 0; side < 2; ++side)
            {
                if (!ValidIndex(feet[side], pose) || !ValidIndex(feet[side]->Parent, pose) || !ValidIndex(feet[side]->Parent->Parent, pose))
                    continue;
                valid[side] = true;
                animated[side] = Position(globals[feet[side]->Index]);
                if (data->FootProbes)
                    data->FootProbes->push_back({ feet[side]->Name, animated[side], traceUp, traceDown });
                // The ground under the foot, as high above or below the character's floor (model height 0: the capsule's bottom).
                float wanted = 0.0f;
                if (data->FootGround)
                {
                    const auto ground = data->FootGround->find(feet[side]->Name);
                    if (ground != data->FootGround->end() && ground->second.Hit)
                    {
                        wanted = std::clamp(ground->second.Point.y, -traceDown * meter, traceUp * meter);
                        normals[side] = ground->second.Normal;
                    }
                }
                state.Offset[side] += (wanted - state.Offset[side]) * blend;
            }
            if (!valid[0] && !valid[1])
                return PassThrough(ctx, nodeIndex, std::move(pose));

            AnimPose placed = pose;
            // The pelvis down by the lower foot's drop (never up: the legs would have to stretch).
            float drop = 0.0f;
            for (int side = 0; side < 2; ++side)
            {
                if (valid[side])
                    drop = std::min(drop, state.Offset[side]);
            }
            drop = std::max(drop, -maxDrop);
            state.Pelvis += (drop - state.Pelvis) * blend;
            if (ValidIndex(pelvis, placed) && std::abs(state.Pelvis) > 1e-6f)
            {
                const glm::mat4 parent = pelvis->Parent && ValidIndex(pelvis->Parent, placed) ? globals[pelvis->Parent->Index] : glm::mat4(1.0f);
                placed.LocalTransforms[pelvis->Index].Translation += glm::vec3(glm::inverse(parent) * glm::vec4(0.0f, state.Pelvis, 0.0f, 0.0f));
                ComputeGlobals(skeleton, placed, *data, globals);
            }

            for (int side = 0; side < 2; ++side)
            {
                if (!valid[side])
                    continue;
                const glm::vec3 target = animated[side] + glm::vec3(0.0f, state.Offset[side], 0.0f);
                SolveTwoBone(skeleton, placed, *data, feet[side], target, nullptr, globals);
                if (align)
                {
                    glm::quat tilt = RotationBetween(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(normals[side]));
                    const float angle = 2.0f * std::acos(std::clamp(std::abs(tilt.w), 0.0f, 1.0f));
                    if (angle > maxAlign && angle > 1e-6f)
                        tilt = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), tilt, maxAlign / angle);
                    state.Align[side] = glm::normalize(glm::slerp(state.Align[side], tilt, blend));
                    SetGlobalRotation(placed, feet[side], state.Align[side] * Rotation(globals[feet[side]->Index]), globals);
                    ComputeGlobals(skeleton, placed, *data, globals);
                }
            }

            AnimPose out;
            BlendPoses(pose, placed, alpha, out);
            ctx.SetOutput(nodeIndex, 0, std::move(out));
        }
    }

    void RegisterAnimationIKNodeTypes()
    {
        NodeTypeRegistry::Register({
            /* TypeName          */ "TwoBoneIK",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "IK",
            /* Inputs            */ { { "Pose", "Pose" }, { "Alpha", "Float" } },
            /* Outputs           */ { { "Pose", "Pose" } },
            /* DefaultProperties */ { { "Alpha", 1.0f }, { "EndBone", std::string("hand_r") }, { "Target", std::string("RightHand") },
                                      { "Pole", std::string() } },
            /* PropertyAssetTypeHints */ { { "EndBone", "Bone" } }, // "Bone": the editor offers the preview skeleton's bones
            /* PropertyRanges    */ { { "Alpha", { 0.0f, 1.0f } } },
            /* Evaluate          */ &EvaluateTwoBoneIK
        });
        NodeTypeRegistry::Register({
            /* TypeName          */ "LookAt",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "IK",
            /* Inputs            */ { { "Pose", "Pose" }, { "Alpha", "Float" } },
            /* Outputs           */ { { "Pose", "Pose" } },
            /* DefaultProperties */ { { "Alpha", 1.0f }, { "Bone", std::string("Head") }, { "Target", std::string("LookAt") },
                                      { "Axis", int32_t(0) }, { "MaxAngle", 70.0f }, { "InterpolationTime", 0.15f } },
            /* PropertyAssetTypeHints */ { { "Bone", "Bone" } },
            /* PropertyRanges    */ { { "Alpha", { 0.0f, 1.0f } }, { "MaxAngle", { 0.0f, 180.0f } } },
            /* Evaluate          */ &EvaluateLookAt
        });
        NodeTypeRegistry::Register({
            /* TypeName          */ "FootPlacement",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "IK",
            /* Inputs            */ { { "Pose", "Pose" }, { "Alpha", "Float" } },
            /* Outputs           */ { { "Pose", "Pose" } },
            /* DefaultProperties */ { { "Alpha", 1.0f }, { "LeftFoot", std::string("foot_l") }, { "RightFoot", std::string("foot_r") },
                                      { "Pelvis", std::string("pelvis") }, { "TraceUp", 0.5f }, { "TraceDown", 0.6f },
                                      { "MaxPelvisDrop", 0.35f }, { "Speed", 12.0f }, { "AlignToGround", true }, { "MaxAlignAngle", 35.0f } },
            /* PropertyAssetTypeHints */ { { "LeftFoot", "Bone" }, { "RightFoot", "Bone" }, { "Pelvis", "Bone" } },
            /* PropertyRanges    */ { { "Alpha", { 0.0f, 1.0f } }, { "MaxAlignAngle", { 0.0f, 89.0f } } },
            /* Evaluate          */ &EvaluateFootPlacement
        });
    }
}
