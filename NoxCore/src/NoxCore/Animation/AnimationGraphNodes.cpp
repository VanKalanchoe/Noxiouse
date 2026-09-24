#include "AnimationGraphNodes.h"

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
