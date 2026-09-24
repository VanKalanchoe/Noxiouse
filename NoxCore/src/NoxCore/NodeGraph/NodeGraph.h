#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "NodeGraphTypes.h"

namespace Nox
{
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
}
