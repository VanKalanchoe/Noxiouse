#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "NodeGraph.h"
#include "NodeGraphTypes.h"
#include "NodeType.h"

namespace Nox
{
    // A resolved input pin: either the output slot of an already-evaluated node (Node != kInvalidGraphIndex), or
    // unconnected, in which case GraphEvalContext::GetInput falls back to a literal in the owning CompiledNode's
    // Properties.
    struct CompiledPin
    {
        uint32_t Node = kInvalidGraphIndex;
        uint32_t OutputSlot = kInvalidGraphIndex;
    };

    struct CompiledSubGraph;

    struct CompiledNode
    {
        const NodeTypeDesc* Type = nullptr; // permanent-lifetime pointer into NodeTypeRegistry
        uint32_t SourceNodeId = 0;          // GraphNode::Id this came from, for logging/debugging only
        std::unordered_map<std::string, NodeGraphValue> Properties;
        std::vector<CompiledPin> Inputs;    // parallel to Type->Inputs
        uint32_t OutputSlotBase = 0;        // this node's outputs live at PinValues[OutputSlotBase, OutputSlotBase + Type->Outputs.size())
        // A state machine node's states (each compiled on its own) and the transitions between them; empty otherwise.
        std::vector<CompiledSubGraph> SubGraphs;
        std::vector<NodeTransition> Transitions;
    };

    // GraphCompiler's output: a NodeGraph flattened into evaluation order with every link resolved to direct
    // indices -- no name lookups or link traversal at evaluation time. Owned by the graph asset, shared
    // read-only by every running instance of it (GraphEvalContext holds the per-instance mutable state).
    struct CompiledGraph
    {
        std::vector<CompiledNode> Nodes; // topologically sorted: every node's inputs are evaluated before it
        uint32_t TotalOutputSlots = 0;   // sizes GraphEvalContext's output value storage
        uint32_t OutputNodeIndex = kInvalidGraphIndex; // index into Nodes; the graph's result
        std::vector<GraphParameter> Parameters;

        bool IsValid() const { return !Nodes.empty() && OutputNodeIndex != kInvalidGraphIndex; }
    };

    struct CompiledSubGraph
    {
        uint32_t Id = 0; // NodeSubGraph::Id, what NodeTransition refers to
        std::string Name;
        CompiledGraph Graph;
    };
}
