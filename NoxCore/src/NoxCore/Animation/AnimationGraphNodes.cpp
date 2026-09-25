#include "AnimationGraphNodes.h"

#include <algorithm>
#include <cmath>

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
        // Leaf: samples a .nanim clip at its own persistent playhead (GraphEvalContext::GetState), advanced by
        // this evaluation's DeltaTime. Properties: "Clip" (uint64_t AssetHandle), "Loop" (bool, default true),
        // "Speed" (float, default 1).
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
            float speed = ctx.GetProperty<float>(nodeIndex, "Speed", 1.0f);

            float& time = ctx.GetState<float>(nodeIndex); // this node's playhead, persists across evaluations
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
            /* Inputs            */ {},
            /* Outputs           */ { { "Pose", "Pose" } },
            // "Clip" starts at 0 (no clip assigned) -- pick one via the editor's asset picker on this property.
            /* DefaultProperties */ { { "Clip", uint64_t(0) }, { "Loop", true }, { "Speed", 1.0f } },
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
