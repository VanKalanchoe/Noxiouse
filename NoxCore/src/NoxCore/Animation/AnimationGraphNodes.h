#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "AnimPose.h"
#include "NoxCore/NodeGraph/NodeGraph.h"
#include "NoxCore/NodeGraph/NodeGraphTypes.h"

namespace Nox
{
    class Skeleton;
    class AnimationSequence;

    // A montage playing in a slot, as Slot nodes see it (AnimationGraphInstance runs the montages).
    struct MontageSlotState
    {
        const AnimationSequence* Clip = nullptr;
        float Time = 0.0f;   // seconds into the clip
        float Weight = 0.0f; // 0..1, blending in and out
    };

    // NodeGraph::Domain / NodeTypeDesc::Domain every animation-graph node type registers under.
    inline constexpr const char* kAnimationGraphDomain = "AnimationGraph";

    // Passed as GraphEvalContext::UserData for one evaluation of an animation graph (AnimationGraphInstance
    // owns and fills this in). Node Evaluate callbacks only read it.
    struct AnimGraphEvalData
    {
        const Skeleton* TargetSkeleton = nullptr;
        float DeltaTime = 0.0f;
        // The graph instance's runtime parameter blackboard (graph defaults, overridden per-instance -- by the
        // inspector today, by scripts once Step 5 wires that up), looked up by name from nodes that reference a
        // parameter (e.g. Blend2D's blend factor). Never null during a real evaluation.
        const std::unordered_map<std::string, NodeGraphValue>* Parameters = nullptr;

        NodeGraphValue GetParameter(const std::string& name, const NodeGraphValue& fallback) const;

        // Notifies the playheads crossed this evaluation (null: nobody listens).
        std::vector<std::string>* FiredNotifies = nullptr;
        // Montages by slot name (null or empty: none playing).
        const std::unordered_map<std::string, MontageSlotState>* Montages = nullptr;
    };

    // Registers "Clip", "Blend2D", "BlendSpace1D", "BlendSpace2D", "LayeredBlend", "ApplyAdditive", "Slot", "GetParameter",
    // "StateMachine" and "Output" under kAnimationGraphDomain
    // into NodeTypeRegistry. Call exactly
    // once at startup (NodeTypeRegistry has no de-duplication, so calling it twice duplicates every entry).
    void RegisterAnimationGraphNodeTypes();

    // A brand-new animation graph as the editor's "create" action starts it: just the Output node, already the
    // graph's output. Needs RegisterAnimationGraphNodeTypes() to have run only for the graph to *compile*, not to
    // build it.
    NodeGraph CreateEmptyAnimationGraph();
}
