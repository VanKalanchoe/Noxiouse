#pragma once

#include <string>
#include <unordered_map>

#include "AnimPose.h"
#include "NoxCore/NodeGraph/NodeGraph.h"
#include "NoxCore/NodeGraph/NodeGraphTypes.h"

namespace Nox
{
    class Skeleton;

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
    };

    // Registers "Clip", "Blend2D" and "Output" under kAnimationGraphDomain into NodeTypeRegistry. Call exactly
    // once at startup (NodeTypeRegistry has no de-duplication, so calling it twice duplicates every entry).
    void RegisterAnimationGraphNodeTypes();

    // A brand-new animation graph as the editor's "create" action starts it: just the Output node, already the
    // graph's output. Needs RegisterAnimationGraphNodeTypes() to have run only for the graph to *compile*, not to
    // build it.
    NodeGraph CreateEmptyAnimationGraph();
}
