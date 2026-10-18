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
            time += data->DeltaTime * speed;
            if (loop)
            {
                time = std::fmod(time, clip->Duration);
                if (time < 0.0f)
                    time += clip->Duration;
            }
            else
            {
                time = glm::clamp(time, 0.0f, clip->Duration);
            }

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
            phase += data->DeltaTime * speed / std::max(duration, 1e-4f);
            phase = loop ? phase - std::floor(phase) : glm::clamp(phase, 0.0f, 1.0f);

            SampleClipPose(*samples[first].Clip, phase * samples[first].Clip->Duration, *data->TargetSkeleton, pose);
            if (second != first && weight > 0.0f)
            {
                AnimPose other;
                SampleClipPose(*samples[second].Clip, phase * samples[second].Clip->Duration, *data->TargetSkeleton, other);
                AnimPose blended;
                BlendPoses(pose, other, weight, blended);
                pose = std::move(blended);
            }
            ctx.SetOutput(nodeIndex, 0, std::move(pose));
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
