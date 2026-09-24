#include "GraphCompiler.h"

#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    namespace
    {
        struct PendingLink
        {
            uint32_t ToPinIndex = 0;
            uint32_t FromNodeIndex = 0;
            uint32_t FromPinIndex = 0;
        };
    }

    CompiledGraph GraphCompiler::Compile(const NodeGraph& graph)
    {
        CompiledGraph result;

        if (graph.Nodes.empty())
        {
            NOX_CORE_WARN("GraphCompiler::Compile - graph '{}' has no nodes", graph.Domain);
            return result;
        }

        // GraphNode::Id -> index into graph.Nodes, and the resolved NodeTypeDesc for each.
        std::unordered_map<uint32_t, uint32_t> idToIndex;
        std::vector<const NodeTypeDesc*> types(graph.Nodes.size(), nullptr);
        idToIndex.reserve(graph.Nodes.size());
        for (uint32_t i = 0; i < (uint32_t)graph.Nodes.size(); i++)
        {
            const GraphNode& node = graph.Nodes[i];
            if (idToIndex.count(node.Id))
            {
                NOX_CORE_ERROR("GraphCompiler::Compile - duplicate node id {} in graph '{}'", node.Id, graph.Domain);
                return CompiledGraph{};
            }
            idToIndex[node.Id] = i;

            const NodeTypeDesc* type = NodeTypeRegistry::Find(graph.Domain, node.TypeName);
            if (!type)
            {
                NOX_CORE_ERROR("GraphCompiler::Compile - unknown node type '{}' for domain '{}'", node.TypeName, graph.Domain);
                return CompiledGraph{};
            }
            types[i] = type;
        }

        // Resolve links to node indices, validate pin ranges + pin type names match, and build the dependency
        // graph (ToNode depends on FromNode) for the topological sort below.
        std::vector<std::vector<PendingLink>> incomingByNode(graph.Nodes.size()); // indexed by ToNodeIndex
        std::vector<std::unordered_set<uint32_t>> dependsOn(graph.Nodes.size());  // indexed by ToNodeIndex, deduped

        for (const GraphLink& link : graph.Links)
        {
            auto fromIt = idToIndex.find(link.FromNode);
            auto toIt = idToIndex.find(link.ToNode);
            if (fromIt == idToIndex.end() || toIt == idToIndex.end())
            {
                NOX_CORE_ERROR("GraphCompiler::Compile - link references a missing node in graph '{}'", graph.Domain);
                return CompiledGraph{};
            }

            uint32_t fromIndex = fromIt->second;
            uint32_t toIndex = toIt->second;
            const NodeTypeDesc& fromType = *types[fromIndex];
            const NodeTypeDesc& toType = *types[toIndex];

            if (link.FromPin >= fromType.Outputs.size() || link.ToPin >= toType.Inputs.size())
            {
                NOX_CORE_ERROR("GraphCompiler::Compile - link pin index out of range in graph '{}'", graph.Domain);
                return CompiledGraph{};
            }
            if (fromType.Outputs[link.FromPin].Type != toType.Inputs[link.ToPin].Type)
            {
                NOX_CORE_ERROR("GraphCompiler::Compile - pin type mismatch linking {}.{} ({}) to {}.{} ({})",
                               fromType.TypeName, fromType.Outputs[link.FromPin].Name, fromType.Outputs[link.FromPin].Type,
                               toType.TypeName, toType.Inputs[link.ToPin].Name, toType.Inputs[link.ToPin].Type);
                return CompiledGraph{};
            }

            incomingByNode[toIndex].push_back({ link.ToPin, fromIndex, link.FromPin });
            dependsOn[toIndex].insert(fromIndex);
        }

        // Kahn's algorithm: nodes with no remaining dependency go first, ties broken by ascending original index
        // so the compiled order is stable across repeated compiles of the same graph.
        std::vector<uint32_t> remainingDeps(graph.Nodes.size());
        std::vector<std::vector<uint32_t>> dependents(graph.Nodes.size()); // FromNodeIndex -> nodes waiting on it
        for (uint32_t i = 0; i < (uint32_t)graph.Nodes.size(); i++)
        {
            remainingDeps[i] = (uint32_t)dependsOn[i].size();
            for (uint32_t dep : dependsOn[i])
                dependents[dep].push_back(i);
        }

        std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
        for (uint32_t i = 0; i < (uint32_t)graph.Nodes.size(); i++)
            if (remainingDeps[i] == 0)
                ready.push(i);

        std::vector<uint32_t> order;
        order.reserve(graph.Nodes.size());
        while (!ready.empty())
        {
            uint32_t index = ready.top();
            ready.pop();
            order.push_back(index);
            for (uint32_t dependent : dependents[index])
            {
                if (--remainingDeps[dependent] == 0)
                    ready.push(dependent);
            }
        }

        if (order.size() != graph.Nodes.size())
        {
            NOX_CORE_ERROR("GraphCompiler::Compile - graph '{}' has a cycle", graph.Domain);
            return CompiledGraph{};
        }

        // Flatten in the sorted order: original index -> compiled index, assign output slot ranges, then resolve
        // every link now that every node's compiled index is known.
        std::vector<uint32_t> compiledIndexOf(graph.Nodes.size());
        result.Nodes.resize(graph.Nodes.size());
        uint32_t nextOutputSlot = 0;
        for (uint32_t compiledIndex = 0; compiledIndex < (uint32_t)order.size(); compiledIndex++)
        {
            uint32_t sourceIndex = order[compiledIndex];
            compiledIndexOf[sourceIndex] = compiledIndex;

            CompiledNode& compiled = result.Nodes[compiledIndex];
            compiled.Type = types[sourceIndex];
            compiled.SourceNodeId = graph.Nodes[sourceIndex].Id;
            compiled.Properties = graph.Nodes[sourceIndex].Properties;
            compiled.Inputs.resize(compiled.Type->Inputs.size());
            compiled.OutputSlotBase = nextOutputSlot;
            nextOutputSlot += (uint32_t)compiled.Type->Outputs.size();
        }

        for (uint32_t toIndex = 0; toIndex < (uint32_t)graph.Nodes.size(); toIndex++)
        {
            CompiledNode& toCompiled = result.Nodes[compiledIndexOf[toIndex]];
            for (const PendingLink& link : incomingByNode[toIndex])
                toCompiled.Inputs[link.ToPinIndex] = { compiledIndexOf[link.FromNodeIndex], link.FromPinIndex };
        }

        result.TotalOutputSlots = nextOutputSlot;
        result.Parameters = graph.Parameters;

        auto outputIt = idToIndex.find(graph.OutputNode);
        if (outputIt == idToIndex.end())
        {
            // No output chosen yet (or it was deleted): a node whose type declares zero output pins is
            // structurally a sink -- the only kind of node "the graph's result" can mean, regardless of domain
            // -- so if there's exactly one, use it without making the user explicitly right-click "Set as Graph
            // Output" for the common single-sink case. More than one sink is genuinely ambiguous.
            uint32_t sinkCount = 0;
            uint32_t sinkNodeId = kInvalidGraphIndex;
            for (uint32_t i = 0; i < (uint32_t)graph.Nodes.size(); i++)
            {
                if (types[i]->Outputs.empty())
                {
                    sinkCount++;
                    sinkNodeId = graph.Nodes[i].Id;
                }
            }

            if (sinkCount == 1)
                outputIt = idToIndex.find(sinkNodeId);

            if (outputIt == idToIndex.end())
            {
                NOX_CORE_ERROR("GraphCompiler::Compile - graph '{}' has no valid OutputNode ({} candidate sink node(s) found; "
                               "right-click the intended one and choose 'Set as Graph Output')", graph.Domain, sinkCount);
                return CompiledGraph{};
            }
        }
        result.OutputNodeIndex = compiledIndexOf[outputIt->second];

        return result;
    }
}
