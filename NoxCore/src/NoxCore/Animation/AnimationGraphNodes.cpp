#include "AnimationGraphNodes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/NodeGraph/GraphEvalContext.h"
#include "NoxCore/NodeGraph/NodeType.h"

namespace Nox
{
    NodeGraphValue AnimGraphEvalData::GetParameter(const std::string& name, const NodeGraphValue& fallback) const
    {
        if (!Parameters)
            return fallback;
        auto it = Parameters->find(name);
        return it != Parameters->end() ? it->second : fallback;
    }

    namespace
    {
        // A Clip node's playhead: where it is in the clip, and whether it has started (so it starts at "StartPosition").
        struct ClipPlayhead
        {
            float Time = 0.0f;
            bool Started = false;
        };

        // Leaf, UE's Sequence Player: samples a .nanim clip at its own persistent playhead (GraphEvalContext::GetState),
        // advanced by this evaluation's DeltaTime. Properties: "Clip" (uint64_t AssetHandle), "Loop" (bool, default true),
        // "StartPosition" (float seconds, default 0: where the playhead starts, again each time a state machine enters the
        // node's state). Input pin "Speed" (play rate, default 1): wire a parameter in to drive it, or leave it unconnected
        // to use the property of the same name.
        void EvaluateClip(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            AnimPose pose;
            if (!data || !data->TargetSkeleton)
            {
                ctx.SetOutput(nodeIndex, 0, std::move(pose));
                return;
            }

            uint64_t clipHandle = ctx.GetProperty<uint64_t>(nodeIndex, "Clip", 0);
            AnimationSequence* clip = clipHandle != 0 ? AssetManager::FindLoadedAsset<AnimationSequence>(clipHandle) : nullptr;

            if (!clip || clip->Duration <= 0.0f)
            {
                pose.ResetToRestPose(*data->TargetSkeleton);
                ctx.SetOutput(nodeIndex, 0, std::move(pose));
                return;
            }

            bool loop = ctx.GetProperty<bool>(nodeIndex, "Loop", true);
            // Graphs saved before "Speed" became a pin keep it as a property; GetInput falls back to that property.
            float speed = ctx.GetInput<float>(nodeIndex, 0);

            ClipPlayhead& playhead = ctx.GetState<ClipPlayhead>(nodeIndex); // persists across evaluations
            if (!playhead.Started)
            {
                playhead.Started = true;
                playhead.Time = ctx.GetProperty<float>(nodeIndex, "StartPosition", 0.0f);
            }
            float& time = playhead.Time;
            const float previousTime = time;
            time += data->DeltaTime * speed;
            bool wrapped = false;
            if (loop)
            {
                wrapped = time >= clip->Duration || time < 0.0f;
                time = std::fmod(time, clip->Duration);
                if (time < 0.0f)
                    time += clip->Duration;
            }
            else
            {
                time = glm::clamp(time, 0.0f, clip->Duration);
            }
            if (data->FiredNotifies)
                CollectCrossedNotifies(*clip, previousTime, time, wrapped, *data->FiredNotifies);

            SampleClipPose(*clip, time, *data->TargetSkeleton, pose);
            ctx.SetOutput(nodeIndex, 0, std::move(pose));
        }

        // Blends two poses by "Alpha" (0 = all PoseA, 1 = all PoseB): wire a GetParameter node into the pin to
        // drive it live, or leave it unconnected to use the node's own constant "Alpha" property (GetInput falls
        // back to the property named after the pin). A first cut of the full N-source 2D blend space the name
        // promises -- see the plan doc's decisions log.
        void EvaluateBlend2D(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            AnimPose a = ctx.GetInput<AnimPose>(nodeIndex, 0);
            AnimPose b = ctx.GetInput<AnimPose>(nodeIndex, 1);

            // An unconnected Pose pin reads as an empty pose. Blending against nothing used to produce an empty
            // result (skeleton falls back to its rest pose), so with only one side wired the node played
            // nothing. Pass the connected side through instead -- there's nothing to blend with.
            if (a.LocalTransforms.empty() || b.LocalTransforms.empty())
            {
                ctx.SetOutput(nodeIndex, 0, a.LocalTransforms.empty() ? std::move(b) : std::move(a));
                return;
            }

            AnimPose out;
            BlendPoses(a, b, ctx.GetInput<float>(nodeIndex, 2), out);
            ctx.SetOutput(nodeIndex, 0, std::move(out));
        }

        // UE's Blend Poses by Int: one of the node's "Pose N" pins (as many as the + Pin button made), picked by "Active Index"
        // (rounded, clamped). Switching cross-fades from the pose shown to the new one over "Blend Time" seconds.
        struct BlendListState
        {
            int32_t Active = -1;
            int32_t Previous = -1;
            float Weight = 1.0f; // of Active over Previous
        };

        void EvaluateBlendByInt(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            const uint32_t poses = static_cast<uint32_t>(ctx.Graph().Nodes[nodeIndex].Inputs.size()) - 1; // pin 0: the index
            if (poses == 0)
            {
                ctx.SetOutput(nodeIndex, 0, AnimPose{});
                return;
            }
            const int32_t wanted = std::clamp(static_cast<int32_t>(std::lround(ctx.GetInput<float>(nodeIndex, 0))), 0, static_cast<int32_t>(poses) - 1);
            BlendListState& state = ctx.GetState<BlendListState>(nodeIndex);
            const float blendTime = ctx.GetProperty<float>(nodeIndex, "BlendTime", 0.2f);
            if (state.Active < 0)
            {
                state.Active = wanted;
                state.Weight = 1.0f;
            }
            else if (wanted != state.Active)
            {
                state.Previous = state.Active;
                state.Active = wanted;
                state.Weight = blendTime > 0.0f ? 0.0f : 1.0f;
            }
            if (state.Weight < 1.0f)
                state.Weight = std::min(1.0f, state.Weight + (data ? data->DeltaTime : 0.0f) / std::max(blendTime, 1e-4f));

            AnimPose active = ctx.GetInput<AnimPose>(nodeIndex, 1 + state.Active);
            if (state.Weight >= 1.0f || state.Previous < 0)
            {
                ctx.SetOutput(nodeIndex, 0, std::move(active));
                return;
            }
            AnimPose previous = ctx.GetInput<AnimPose>(nodeIndex, 1 + state.Previous);
            if (previous.LocalTransforms.empty() || active.LocalTransforms.empty())
            {
                ctx.SetOutput(nodeIndex, 0, active.LocalTransforms.empty() ? std::move(previous) : std::move(active));
                return;
            }
            AnimPose out;
            BlendPoses(previous, active, state.Weight, out);
            ctx.SetOutput(nodeIndex, 0, std::move(out));
        }

        // UE's Blend Space 1D: up to four clips ("Clip0".."Clip3") placed along one axis ("Position0".."Position3"); the
        // "Value" pin (or, unconnected, the property of the same name) picks the point, e.g. Speed for idle / walk / run.
        // The two clips around the value play in step -- one shared normalized phase, advanced at the rate of their blended
        // duration -- so a walk and a run blend with their feet together (sync by normalized time; UE's sync markers are
        // the refinement). Unassigned clips are skipped. "Loop" (default true), "Speed" (play rate, default 1).
        void EvaluateBlendSpace1D(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            AnimPose pose;
            if (!data || !data->TargetSkeleton)
            {
                ctx.SetOutput(nodeIndex, 0, std::move(pose));
                return;
            }

            struct BlendSample
            {
                AnimationSequence* Clip = nullptr;
                float Position = 0.0f;
            };
            std::array<BlendSample, 4> samples;
            size_t count = 0;
            for (int i = 0; i < 4; ++i)
            {
                const std::string index = std::to_string(i);
                const uint64_t clipHandle = ctx.GetProperty<uint64_t>(nodeIndex, "Clip" + index, 0);
                AnimationSequence* clip = clipHandle != 0 ? AssetManager::FindLoadedAsset<AnimationSequence>(clipHandle) : nullptr;
                if (clip && clip->Duration > 0.0f)
                    samples[count++] = { clip, ctx.GetProperty<float>(nodeIndex, "Position" + index, static_cast<float>(i)) };
            }
            if (count == 0)
            {
                pose.ResetToRestPose(*data->TargetSkeleton);
                ctx.SetOutput(nodeIndex, 0, std::move(pose));
                return;
            }
            std::stable_sort(samples.begin(), samples.begin() + count,
                             [](const BlendSample& a, const BlendSample& b) { return a.Position < b.Position; });

            // The two samples around the value and how far it is from the first to the second; clamped at the ends.
            const float value = ctx.GetInput<float>(nodeIndex, 0);
            size_t first = 0;
            size_t second = 0;
            float weight = 0.0f;
            if (value >= samples[count - 1].Position)
            {
                first = second = count - 1;
            }
            else if (value > samples[0].Position)
            {
                for (size_t i = 0; i + 1 < count; ++i)
                {
                    if (value <= samples[i + 1].Position)
                    {
                        first = i;
                        second = i + 1;
                        const float span = samples[second].Position - samples[first].Position;
                        weight = span > 1e-6f ? (value - samples[first].Position) / span : 0.0f;
                        break;
                    }
                }
            }

            const bool loop = ctx.GetProperty<bool>(nodeIndex, "Loop", true);
            const float speed = ctx.GetProperty<float>(nodeIndex, "Speed", 1.0f);
            const float duration = glm::mix(samples[first].Clip->Duration, samples[second].Clip->Duration, weight);
            float& phase = ctx.GetState<float>(nodeIndex); // normalized 0..1, shared by every clip of the space
            const float previousPhase = phase;
            phase += data->DeltaTime * speed / std::max(duration, 1e-4f);
            const bool wrapped = loop && (phase >= 1.0f || phase < 0.0f);
            phase = loop ? phase - std::floor(phase) : glm::clamp(phase, 0.0f, 1.0f);
            if (data->FiredNotifies)
            {
                // The leading clip's notifies (UE fires the notifies of the clips weighted above a threshold; one is enough
                // for footsteps and keeps them from doubling).
                const AnimationSequence& leading = *samples[weight < 0.5f ? first : second].Clip;
                CollectCrossedNotifies(leading, previousPhase * leading.Duration, phase * leading.Duration, wrapped, *data->FiredNotifies);
            }

            // The leading clip plays at the phase; the other one at its sync markers' matching place (else the same phase).
            const size_t leadingIndex = weight < 0.5f ? first : second;
            const AnimationSequence& leadingClip = *samples[leadingIndex].Clip;
            auto clipTime = [&](size_t index)
            {
                const AnimationSequence& clip = *samples[index].Clip;
                const float normalized = phase * clip.Duration;
                return index == leadingIndex ? normalized : SyncedClipTime(leadingClip, phase * leadingClip.Duration, clip, normalized);
            };
            SampleClipPose(*samples[first].Clip, clipTime(first), *data->TargetSkeleton, pose);
            if (second != first && weight > 0.0f)
            {
                AnimPose other;
                SampleClipPose(*samples[second].Clip, clipTime(second), *data->TargetSkeleton, other);
                AnimPose blended;
                BlendPoses(pose, other, weight, blended);
                pose = std::move(blended);
            }
            ctx.SetOutput(nodeIndex, 0, std::move(pose));
        }

        // UE's Blend Space (2D), e.g. direction x speed: up to eight clips ("Clip0".."Clip7") placed at ("X0", "Y0")..("X7", "Y7");
        // the "X" and "Y" pins pick the point. Weights by gradient band interpolation (Johansen 2009, Unity's freeform
        // cartesian blend tree): a sample's weight falls off linearly towards each other sample and is the smallest of
        // those, then all are normalized -- exact at the samples, smooth between, no triangulation needed. The clips play
        // in step (one normalized phase at their weighted duration), like BlendSpace1D. "Loop", "Speed".
        void EvaluateBlendSpace2D(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            AnimPose pose;
            if (!data || !data->TargetSkeleton)
            {
                ctx.SetOutput(nodeIndex, 0, std::move(pose));
                return;
            }

            struct BlendSample
            {
                AnimationSequence* Clip = nullptr;
                glm::vec2 Position{ 0.0f };
            };
            std::array<BlendSample, 8> samples;
            size_t count = 0;
            for (int i = 0; i < 8; ++i)
            {
                const std::string index = std::to_string(i);
                const uint64_t clipHandle = ctx.GetProperty<uint64_t>(nodeIndex, "Clip" + index, 0);
                AnimationSequence* clip = clipHandle != 0 ? AssetManager::FindLoadedAsset<AnimationSequence>(clipHandle) : nullptr;
                if (clip && clip->Duration > 0.0f)
                    samples[count++] = { clip, { ctx.GetProperty<float>(nodeIndex, "X" + index, 0.0f), ctx.GetProperty<float>(nodeIndex, "Y" + index, 0.0f) } };
            }
            if (count == 0)
            {
                pose.ResetToRestPose(*data->TargetSkeleton);
                ctx.SetOutput(nodeIndex, 0, std::move(pose));
                return;
            }

            const glm::vec2 point(ctx.GetInput<float>(nodeIndex, 0), ctx.GetInput<float>(nodeIndex, 1));
            std::array<float, 8> weights{};
            float total = 0.0f;
            for (size_t i = 0; i < count; ++i)
            {
                float weight = 1.0f;
                for (size_t j = 0; j < count; ++j)
                {
                    if (j == i)
                        continue;
                    const glm::vec2 edge = samples[j].Position - samples[i].Position;
                    const float lengthSquared = glm::dot(edge, edge);
                    if (lengthSquared < 1e-8f)
                        continue; // two samples at one place share it
                    weight = std::min(weight, std::max(1.0f - glm::dot(point - samples[i].Position, edge) / lengthSquared, 0.0f));
                }
                weights[i] = weight;
                total += weight;
            }
            if (total <= 1e-6f)
            {
                // Outside every band (far beyond the samples): the nearest one.
                size_t nearest = 0;
                for (size_t i = 1; i < count; ++i)
                {
                    if (glm::distance(point, samples[i].Position) < glm::distance(point, samples[nearest].Position))
                        nearest = i;
                }
                weights[nearest] = total = 1.0f;
            }

            float duration = 0.0f;
            size_t leading = 0;
            for (size_t i = 0; i < count; ++i)
            {
                weights[i] /= total;
                duration += weights[i] * samples[i].Clip->Duration;
                if (weights[i] > weights[leading])
                    leading = i;
            }

            const bool loop = ctx.GetProperty<bool>(nodeIndex, "Loop", true);
            const float speed = ctx.GetProperty<float>(nodeIndex, "Speed", 1.0f);
            float& phase = ctx.GetState<float>(nodeIndex);
            const float previousPhase = phase;
            phase += data->DeltaTime * speed / std::max(duration, 1e-4f);
            const bool wrapped = loop && (phase >= 1.0f || phase < 0.0f);
            phase = loop ? phase - std::floor(phase) : glm::clamp(phase, 0.0f, 1.0f);
            if (data->FiredNotifies)
            {
                const AnimationSequence& clip = *samples[leading].Clip;
                CollectCrossedNotifies(clip, previousPhase * clip.Duration, phase * clip.Duration, wrapped, *data->FiredNotifies);
            }

            std::array<AnimPose, 8> sampled;
            std::vector<const AnimPose*> poses;
            std::vector<float> poseWeights;
            for (size_t i = 0; i < count; ++i)
            {
                if (weights[i] < 0.001f)
                    continue;
                const AnimationSequence& leadingClip = *samples[leading].Clip;
                const float normalized = phase * samples[i].Clip->Duration;
                const float time = i == leading ? normalized : SyncedClipTime(leadingClip, phase * leadingClip.Duration, *samples[i].Clip, normalized);
                SampleClipPose(*samples[i].Clip, time, *data->TargetSkeleton, sampled[i]);
                poses.push_back(&sampled[i]);
                poseWeights.push_back(weights[i]);
            }
            BlendPosesWeighted(poses, poseWeights, pose);
            ctx.SetOutput(nodeIndex, 0, std::move(pose));
        }

        // UE's Layered Blend per Bone: "Blend" replaces "Base" from the bone named "Bone" (e.g. "spine_02") down its whole
        // branch, by "Alpha" -- upper body from one animation, legs from another. "BlendDepth" > 0 fades in over that many
        // bones down the branch (1 = full weight right at the bone). An empty or unknown bone blends the whole pose.
        struct LayeredBlendMask
        {
            const Skeleton* ForSkeleton = nullptr;
            std::string Bone;
            int32_t Depth = -1;
            std::vector<float> Weights; // per node, at Alpha 1
        };

        void EvaluateLayeredBlend(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            AnimPose base = ctx.GetInput<AnimPose>(nodeIndex, 0);
            AnimPose blend = ctx.GetInput<AnimPose>(nodeIndex, 1);
            const float alpha = glm::clamp(ctx.GetInput<float>(nodeIndex, 2), 0.0f, 1.0f);
            if (base.LocalTransforms.empty() || blend.LocalTransforms.empty() || !data || !data->TargetSkeleton)
            {
                ctx.SetOutput(nodeIndex, 0, base.LocalTransforms.empty() ? std::move(blend) : std::move(base));
                return;
            }

            const std::string bone = ctx.GetProperty<std::string>(nodeIndex, "Bone", std::string());
            const int32_t depth = std::max(ctx.GetProperty<int32_t>(nodeIndex, "BlendDepth", 0), 0);
            LayeredBlendMask& mask = ctx.GetState<LayeredBlendMask>(nodeIndex);
            if (mask.ForSkeleton != data->TargetSkeleton || mask.Bone != bone || mask.Depth != depth)
            {
                mask.ForSkeleton = data->TargetSkeleton;
                mask.Bone = bone;
                mask.Depth = depth;
                const Skeleton& skeleton = *data->TargetSkeleton;
                const Node* root = nullptr;
                for (const Node* node : skeleton.AllNodes)
                {
                    if (node && !bone.empty() && node->Name == bone)
                    {
                        root = node;
                        break;
                    }
                }
                mask.Weights.assign(skeleton.AllNodes.size(), root ? 0.0f : 1.0f);
                if (root)
                {
                    auto mark = [&](auto& self, const Node* node, int32_t level) -> void
                    {
                        if (node->Index >= 0 && node->Index < static_cast<int32_t>(mask.Weights.size()))
                            mask.Weights[node->Index] = depth > 0 ? std::min(1.0f, float(level + 1) / float(depth)) : 1.0f;
                        for (const Node* child : node->Children)
                            self(self, child, level + 1);
                    };
                    mark(mark, root, 0);
                }
            }

            std::vector<float> weights = mask.Weights;
            for (float& weight : weights)
                weight *= alpha;
            AnimPose out;
            BlendPosesPerNode(base, blend, weights, out);
            ctx.SetOutput(nodeIndex, 0, std::move(out));
        }

        // UE's Apply Additive: "Additive" relative to "Reference" put on top of "Base", by "Alpha" (breathing, aim offsets,
        // leaning, hit reactions over any base animation). "Reference" unconnected = the skeleton's rest pose; for an additive
        // clip made from a normal one, wire the same clip at time 0 (a Clip node with Speed 0) into it.
        void EvaluateApplyAdditive(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            AnimPose base = ctx.GetInput<AnimPose>(nodeIndex, 0);
            AnimPose additive = ctx.GetInput<AnimPose>(nodeIndex, 1);
            AnimPose reference = ctx.GetInput<AnimPose>(nodeIndex, 2);
            const float alpha = ctx.GetInput<float>(nodeIndex, 3);
            if (base.LocalTransforms.empty() || additive.LocalTransforms.empty() || !data || !data->TargetSkeleton)
            {
                ctx.SetOutput(nodeIndex, 0, std::move(base));
                return;
            }
            if (reference.LocalTransforms.empty())
                reference.ResetToRestPose(*data->TargetSkeleton);
            AnimPose out;
            ApplyAdditivePose(base, additive, reference, alpha, out);
            ctx.SetOutput(nodeIndex, 0, std::move(out));
        }

        // UE's Slot: passes "Source" through, unless a montage plays in the slot named "SlotName" (AnimationGraphInstance::
        // PlayMontage, scripts' Animator.PlayMontage): then the montage's clip over it, blended by the montage's weight.
        void EvaluateSlot(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            AnimPose source = ctx.GetInput<AnimPose>(nodeIndex, 0);
            if (!data || !data->TargetSkeleton || !data->Montages || data->Montages->empty())
            {
                ctx.SetOutput(nodeIndex, 0, std::move(source));
                return;
            }
            const std::string slot = ctx.GetProperty<std::string>(nodeIndex, "SlotName", std::string("DefaultSlot"));
            const auto montage = data->Montages->find(slot);
            if (montage == data->Montages->end() || !montage->second.Clip || montage->second.Weight <= 0.0f)
            {
                ctx.SetOutput(nodeIndex, 0, std::move(source));
                return;
            }
            AnimPose montagePose;
            SampleClipPose(*montage->second.Clip, montage->second.Time, *data->TargetSkeleton, montagePose);
            if (source.LocalTransforms.empty())
            {
                ctx.SetOutput(nodeIndex, 0, std::move(montagePose));
                return;
            }
            AnimPose out;
            BlendPoses(source, montagePose, montage->second.Weight, out);
            ctx.SetOutput(nodeIndex, 0, std::move(out));
        }

        // Reads one of the instance's graph parameters onto a Float pin: the parameter is named by the string
        // property kParameterReferenceProperty ("Parameter"). Bool and int parameters read as 0/1 and the number;
        // an unknown name reads 0.
        void EvaluateGetParameter(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            float value = 0.0f;
            if (auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData))
            {
                std::string name = ctx.GetProperty<std::string>(nodeIndex, kParameterReferenceProperty, std::string());
                value = NodeGraphValueToFloat(data->GetParameter(name, 0.0f));
            }
            ctx.SetOutput(nodeIndex, 0, value);
        }

        // True when every rule of a transition holds against the instance's parameters.
        bool TransitionRulesHold(const std::vector<TransitionRule>& rules, const AnimGraphEvalData& data)
        {
            for (const TransitionRule& rule : rules)
            {
                const float parameter = NodeGraphValueToFloat(data.GetParameter(rule.Parameter, 0.0f));
                const float value = NodeGraphValueToFloat(rule.Value);
                bool holds = false;
                switch (rule.Compare)
                {
                case TransitionCompare::Equal:        holds = std::abs(parameter - value) < 1e-4f; break;
                case TransitionCompare::NotEqual:     holds = std::abs(parameter - value) >= 1e-4f; break;
                case TransitionCompare::Greater:      holds = parameter > value; break;
                case TransitionCompare::GreaterEqual: holds = parameter >= value; break;
                case TransitionCompare::Less:         holds = parameter < value; break;
                case TransitionCompare::LessEqual:    holds = parameter <= value; break;
                case TransitionCompare::IsTrue:       holds = parameter != 0.0f; break;
                case TransitionCompare::IsFalse:      holds = parameter == 0.0f; break;
                }
                if (!holds)
                    return false;
            }
            return true;
        }

        // A state machine's running state, kept in the node's GraphEvalContext slot: one evaluation context per state (each
        // state's own playheads), which state is active and, during a transition, which one it is blending to.
        struct StateMachineRuntime
        {
            std::vector<GraphEvalContext> States;
            int Current = -1;
            int Next = -1;
            float Elapsed = 0.0f;
            float Duration = 0.0f;
            bool EaseInOut = true;
        };

        // The State Machine node: outputs the active state's pose, and while a transition runs the blend of the old and
        // new state (each evaluated every frame, blended by the eased progress). Transitions are checked in order from
        // the active state; the first whose rules hold starts. The entered state starts from its beginning.
        // Properties: "EntryState" (int32, the NodeSubGraph::Id the machine starts in).
        void EvaluateStateMachine(GraphEvalContext& ctx, uint32_t nodeIndex)
        {
            auto* data = static_cast<AnimGraphEvalData*>(ctx.UserData);
            const CompiledNode& node = ctx.Graph().Nodes[nodeIndex];
            AnimPose result;
            if (!data || !data->TargetSkeleton || node.SubGraphs.empty())
            {
                ctx.SetOutput(nodeIndex, 0, std::move(result));
                return;
            }

            StateMachineRuntime& runtime = ctx.GetState<StateMachineRuntime>(nodeIndex);
            auto indexOfState = [&](uint32_t id) -> int
            {
                for (size_t i = 0; i < node.SubGraphs.size(); ++i)
                {
                    if (node.SubGraphs[i].Id == id)
                        return static_cast<int>(i);
                }
                return -1;
            };

            if (runtime.States.size() != node.SubGraphs.size())
            {
                runtime.States.assign(node.SubGraphs.size(), GraphEvalContext{});
                for (size_t i = 0; i < node.SubGraphs.size(); ++i)
                    runtime.States[i].Init(node.SubGraphs[i].Graph);
                const int entry = indexOfState(static_cast<uint32_t>(ctx.GetProperty<int32_t>(nodeIndex, "EntryState", 0)));
                runtime.Current = entry >= 0 ? entry : 0;
                runtime.Next = -1;
            }

            if (runtime.Next < 0)
            {
                for (const NodeTransition& transition : node.Transitions)
                {
                    if (indexOfState(transition.FromState) != runtime.Current || !TransitionRulesHold(transition.Rules, *data))
                        continue;
                    const int to = indexOfState(transition.ToState);
                    if (to < 0 || to == runtime.Current)
                        continue;

                    runtime.Next = to;
                    runtime.Elapsed = 0.0f;
                    runtime.Duration = std::max(transition.Duration, 0.0f);
                    runtime.EaseInOut = transition.EaseInOut;
                    runtime.States[to].Init(node.SubGraphs[to].Graph); // starts from its beginning
                    break;
                }
            }

            auto evaluateState = [&](int index, AnimPose& out)
            {
                GraphEvalContext& state = runtime.States[index];
                const CompiledGraph& graph = node.SubGraphs[index].Graph;
                state.UserData = ctx.UserData;
                EvaluateGraph(state, graph);
                out = state.GetInput<AnimPose>(graph.OutputNodeIndex, 0);
            };

            AnimPose current;
            evaluateState(runtime.Current, current);
            if (runtime.Next >= 0)
            {
                AnimPose next;
                evaluateState(runtime.Next, next);

                runtime.Elapsed += data->DeltaTime;
                const float t = runtime.Duration > 0.0f ? glm::clamp(runtime.Elapsed / runtime.Duration, 0.0f, 1.0f) : 1.0f;
                const float weight = runtime.EaseInOut ? t * t * (3.0f - 2.0f * t) : t;
                if (current.LocalTransforms.empty() || next.LocalTransforms.empty())
                    result = current.LocalTransforms.empty() ? std::move(next) : std::move(current);
                else
                    BlendPoses(current, next, weight, result);

                if (t >= 1.0f)
                {
                    runtime.Current = runtime.Next;
                    runtime.Next = -1;
                }
            }
            else
            {
                result = std::move(current);
            }
            ctx.SetOutput(nodeIndex, 0, std::move(result));
        }

        // Sink: no outputs. AnimationGraphInstance reads the graph's result with
        // ctx.GetInput<AnimPose>(compiled.OutputNodeIndex, 0) after EvaluateGraph, so this has nothing to do.
        void EvaluateOutput(GraphEvalContext&, uint32_t) {}
    }

    void RegisterAnimationGraphNodeTypes()
    {
        NodeTypeRegistry::Register({
            /* TypeName          */ "Clip",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "Sources",
            /* Inputs            */ { { "Speed", "Float" } },
            /* Outputs           */ { { "Pose", "Pose" } },
            // "Clip" starts at 0 (no clip assigned) -- pick one via the editor's asset picker on this property.
            /* DefaultProperties */ { { "Clip", uint64_t(0) }, { "Loop", true }, { "Speed", 1.0f }, { "StartPosition", 0.0f } },
            /* PropertyAssetTypeHints */ { { "Clip", "AssetType::AnimationSequence" } },
            /* PropertyRanges    */ {},
            /* Evaluate          */ &EvaluateClip
        });

        NodeTypeRegistry::Register({
            /* TypeName          */ "Blend2D",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "Blending",
            /* Inputs            */ { { "PoseA", "Pose" }, { "PoseB", "Pose" }, { "Alpha", "Float" } },
            /* Outputs           */ { { "Pose", "Pose" } },
            // "Alpha" is both the pin and (when the pin is unconnected) its constant value.
            /* DefaultProperties */ { { "Alpha", 0.0f } },
            /* PropertyAssetTypeHints */ {},
            /* PropertyRanges    */ { { "Alpha", { 0.0f, 1.0f } } },
            /* Evaluate          */ &EvaluateBlend2D
        });

        {
            // Variable inputs: "Pose 0", "Pose 1", ... (the editor's + Pin / - Pin).
            NodeTypeDesc blendByInt;
            blendByInt.TypeName = "BlendPosesByInt";
            blendByInt.Domain = kAnimationGraphDomain;
            blendByInt.Category = "Blending";
            blendByInt.Inputs = { { "Active Index", "Float" } };
            blendByInt.Outputs = { { "Pose", "Pose" } };
            blendByInt.DefaultProperties = { { "Active Index", 0.0f }, { "BlendTime", 0.2f }, { kVariableInputCountProperty, int32_t(2) } };
            blendByInt.PropertyRanges = { { "BlendTime", { 0.0f, 5.0f } } };
            blendByInt.Evaluate = &EvaluateBlendByInt;
            blendByInt.VariableInput = { "Pose", "Pose" };
            blendByInt.MinVariableInputs = 2;
            NodeTypeRegistry::Register(std::move(blendByInt));
        }

        NodeTypeRegistry::Register({
            /* TypeName          */ "BlendSpace1D",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "Blending",
            /* Inputs            */ { { "Value", "Float" } },
            /* Outputs           */ { { "Pose", "Pose" } },
            /* DefaultProperties */ { { "Value", 0.0f },
                                      { "Clip0", uint64_t(0) }, { "Position0", 0.0f },
                                      { "Clip1", uint64_t(0) }, { "Position1", 1.0f },
                                      { "Clip2", uint64_t(0) }, { "Position2", 2.0f },
                                      { "Clip3", uint64_t(0) }, { "Position3", 3.0f },
                                      { "Loop", true }, { "Speed", 1.0f } },
            /* PropertyAssetTypeHints */ { { "Clip0", "AssetType::AnimationSequence" }, { "Clip1", "AssetType::AnimationSequence" },
                                           { "Clip2", "AssetType::AnimationSequence" }, { "Clip3", "AssetType::AnimationSequence" } },
            /* PropertyRanges    */ {},
            /* Evaluate          */ &EvaluateBlendSpace1D
        });
        {
            // Interleaved per sample so the editor shows each clip with its point. A locomotion layout to start from:
            // idle in the middle, forward walk / run up, back down, strafes left and right, forward diagonals.
            const std::array<glm::vec2, 8> layout = { glm::vec2(0, 0), glm::vec2(0, 1), glm::vec2(0, 2), glm::vec2(0, -1),
                                                      glm::vec2(-1, 0), glm::vec2(1, 0), glm::vec2(-1, 1), glm::vec2(1, 1) };
            std::vector<std::pair<std::string, NodeGraphValue>> properties = { { "X", 0.0f }, { "Y", 0.0f } };
            std::vector<std::pair<std::string, std::string>> clipHints;
            for (int i = 0; i < 8; ++i)
            {
                const std::string index = std::to_string(i);
                properties.push_back({ "Clip" + index, uint64_t(0) });
                properties.push_back({ "X" + index, layout[i].x });
                properties.push_back({ "Y" + index, layout[i].y });
                clipHints.push_back({ "Clip" + index, "AssetType::AnimationSequence" });
            }
            properties.push_back({ "Loop", true });
            properties.push_back({ "Speed", 1.0f });
            NodeTypeRegistry::Register({
                /* TypeName          */ "BlendSpace2D",
                /* Domain            */ kAnimationGraphDomain,
                /* Category          */ "Blending",
                /* Inputs            */ { { "X", "Float" }, { "Y", "Float" } },
                /* Outputs           */ { { "Pose", "Pose" } },
                /* DefaultProperties */ { properties.begin(), properties.end() },
                /* PropertyAssetTypeHints */ { clipHints.begin(), clipHints.end() },
                /* PropertyRanges    */ {},
                /* Evaluate          */ &EvaluateBlendSpace2D
            });
        }

        NodeTypeRegistry::Register({
            /* TypeName          */ "LayeredBlend",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "Blending",
            /* Inputs            */ { { "Base", "Pose" }, { "Blend", "Pose" }, { "Alpha", "Float" } },
            /* Outputs           */ { { "Pose", "Pose" } },
            /* DefaultProperties */ { { "Alpha", 1.0f }, { "Bone", std::string("spine_02") }, { "BlendDepth", int32_t(0) } },
            /* PropertyAssetTypeHints */ {},
            /* PropertyRanges    */ { { "Alpha", { 0.0f, 1.0f } } },
            /* Evaluate          */ &EvaluateLayeredBlend
        });

        NodeTypeRegistry::Register({
            /* TypeName          */ "ApplyAdditive",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "Blending",
            /* Inputs            */ { { "Base", "Pose" }, { "Additive", "Pose" }, { "Reference", "Pose" }, { "Alpha", "Float" } },
            /* Outputs           */ { { "Pose", "Pose" } },
            /* DefaultProperties */ { { "Alpha", 1.0f } },
            /* PropertyAssetTypeHints */ {},
            /* PropertyRanges    */ { { "Alpha", { 0.0f, 1.0f } } },
            /* Evaluate          */ &EvaluateApplyAdditive
        });

        NodeTypeRegistry::Register({
            /* TypeName          */ "Slot",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "Montages",
            /* Inputs            */ { { "Source", "Pose" } },
            /* Outputs           */ { { "Pose", "Pose" } },
            /* DefaultProperties */ { { "SlotName", std::string("DefaultSlot") } },
            /* PropertyAssetTypeHints */ {},
            /* PropertyRanges    */ {},
            /* Evaluate          */ &EvaluateSlot
        });

        NodeTypeRegistry::Register({
            /* TypeName          */ "GetParameter",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "Inputs",
            /* Inputs            */ {},
            /* Outputs           */ { { "Value", "Float" } },
            // Normally created from the create-node menu's "Parameters / <name>" entries, which fill this in.
            /* DefaultProperties */ { { kParameterReferenceProperty, std::string() } },
            /* PropertyAssetTypeHints */ {},
            /* PropertyRanges    */ {},
            /* Evaluate          */ &EvaluateGetParameter
        });

        // Its states are its sub graphs (each ends in its own Output node) and the transitions between them; both live
        // on the node, not in properties (NodeGraph::GraphNode::SubGraphs / Transitions). No editor UI for them yet.
        NodeTypeRegistry::Register({
            /* TypeName          */ "StateMachine",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "State Machines",
            /* Inputs            */ {},
            /* Outputs           */ { { "Pose", "Pose" } },
            /* DefaultProperties */ { { "EntryState", int32_t(0) } },
            /* PropertyAssetTypeHints */ {},
            /* PropertyRanges    */ {},
            /* Evaluate          */ &EvaluateStateMachine,
            /* OwnsSubGraphs     */ true
        });

        NodeTypeRegistry::Register({
            /* TypeName          */ "Output",
            /* Domain            */ kAnimationGraphDomain,
            /* Category          */ "Sinks",
            /* Inputs            */ { { "Pose", "Pose" } },
            /* Outputs           */ {},
            /* DefaultProperties */ {},
            /* PropertyAssetTypeHints */ {},
            /* PropertyRanges    */ {},
            /* Evaluate          */ &EvaluateOutput
        });
    }

    NodeGraph CreateEmptyAnimationGraph()
    {
        NodeGraph graph;
        graph.Domain = kAnimationGraphDomain;

        GraphNode output;
        output.Id = graph.NextNodeId++;
        output.TypeName = "Output";
        output.EditorPosition = { 400.0f, 100.0f };
        graph.Nodes.push_back(output);
        graph.OutputNode = output.Id;
        return graph;
    }
}
