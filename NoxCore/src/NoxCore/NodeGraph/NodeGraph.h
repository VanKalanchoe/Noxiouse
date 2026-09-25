#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "NodeGraphTypes.h"

namespace Nox
{
    struct NodeGraph;
    struct NodeSubGraph;

    // How a transition rule compares a graph parameter (bool and int parameters read as 0/1 and the number).
    enum class TransitionCompare : uint8_t
    {
        Equal = 0,
        NotEqual,
        Greater,
        GreaterEqual,
        Less,
        LessEqual,
        IsTrue,  // parameter != 0, Value unused
        IsFalse  // parameter == 0, Value unused
    };

    // One condition of a transition: `Parameter <Compare> Value`. A transition fires when all its rules hold.
    struct TransitionRule
    {
        std::string Parameter;
        TransitionCompare Compare = TransitionCompare::Greater;
        NodeGraphValue Value = 0.0f;
    };

    // A state machine node's edge: from one of its sub graphs (states) to another, taken when Rules hold, blending over
    // Duration seconds. FromState / ToState are NodeSubGraph::Id values.
    struct NodeTransition
    {
        uint32_t FromState = 0;
        uint32_t ToState = 0;
        float Duration = 0.2f;
        bool EaseInOut = true; // smoothstep instead of linear
        std::vector<TransitionRule> Rules;
    };

    // One authored node: which type it is, its literal input values / extra data, and where the editor drew it.
    struct GraphNode
    {
        uint32_t Id = 0; // stable within the graph; GraphLink and NodeGraph::OutputNode reference this, not an array index
        std::string TypeName; // looked up in NodeTypeRegistry against the graph's Domain at compile time
        glm::vec2 EditorPosition{0.0f}; // canvas position; unused at runtime, kept here so it round-trips with the node

        // Per-input-pin literal values (used when a pin has no incoming link), keyed by pin name, plus any
        // node-specific data a node type wants beyond its declared pins (e.g. a Clip node's clip AssetHandle,
        // loop flag) -- same map serves both, so the serializer stays fully generic with no per-node-type code.
        std::unordered_map<std::string, NodeGraphValue> Properties;

        // Nested graphs a node owns (a state machine's states: each a graph of the same domain producing that state's
        // result) and the transitions between them. Empty for ordinary nodes. Sub graphs share the root graph's parameters.
        std::vector<NodeSubGraph> SubGraphs;
        std::vector<NodeTransition> Transitions;
    };

    // One connection: FromNode's FromPin-th output feeds ToNode's ToPin-th input. Pin indices are 0-based in
    // the order the node type's NodeTypeDesc declares them.
    struct GraphLink
    {
        uint32_t FromNode = 0; uint32_t FromPin = 0;
        uint32_t ToNode = 0;   uint32_t ToPin = 0;
    };

    // A named value on the graph's blackboard (e.g. "Speed", "IsGrounded"), settable per-instance at runtime
    // (scripts, gameplay code) and readable by any node's Evaluate (e.g. a Blend2D's X/Y, a transition condition).
    struct GraphParameter
    {
        std::string Name;
        NodeGraphValue DefaultValue;
    };

    // The authored, editable graph -- what the editor shows and what NodeGraphSerializer saves to disk.
    // GraphCompiler turns this into a CompiledGraph for evaluation; nothing here is touched at runtime.
    struct NodeGraph
    {
        std::string Domain; // e.g. "AnimationGraph" -- which NodeTypeRegistry entries are legal in this graph
        std::vector<GraphNode> Nodes;
        std::vector<GraphLink> Links;
        std::vector<GraphParameter> Parameters;
        uint32_t OutputNode = kInvalidGraphIndex; // GraphNode::Id of the graph's sink (e.g. the Output Pose node)
        uint32_t NextNodeId = 1; // next id the editor hands out when adding a node
    };

    // One graph a node owns (a state of a state machine), with an id the node's transitions refer to.
    struct NodeSubGraph
    {
        uint32_t Id = 0;
        std::string Name;
        glm::vec2 EditorPosition{0.0f}; // where its state box sits on the owning node's state view
        NodeGraph Graph;
    };
}
