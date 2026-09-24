#pragma once

#include <any>
#include <cassert>
#include <string>
#include <vector>

#include "CompiledGraph.h"

namespace Nox
{
    // Per-instance evaluation state for one running graph (e.g. one AnimationGraphInstance): every node's last
    // computed output values, plus any persistent per-node state (a Clip node's playhead, a state machine's
    // active state). Reused every evaluation; sized once against its CompiledGraph in Init(). Instances of the
    // same graph asset never share a GraphEvalContext -- the CompiledGraph is the only thing they share.
    class GraphEvalContext
    {
    public:
        void Init(const CompiledGraph& graph);

        // Node Evaluate callbacks read/write pins through these. T must be the type the domain's node types
        // agreed to put on that pin -- asserted in debug on a source read, silently returns a default-constructed
        // T if the pin was never connected/written (same contract as an unconnected pin's declared default).
        template<typename T>
        T GetInput(uint32_t compiledNodeIndex, uint32_t inputPinIndex) const
        {
            const CompiledNode& node = m_Graph->Nodes[compiledNodeIndex];
            const CompiledPin& pin = node.Inputs[inputPinIndex];
            if (pin.Node != kInvalidGraphIndex)
            {
                const CompiledNode& source = m_Graph->Nodes[pin.Node];
                const std::any& value = m_OutputValues[source.OutputSlotBase + pin.OutputSlot];
                assert(value.has_value() && "GraphEvalContext::GetInput - source output was never written; check compiled evaluation order");
                return std::any_cast<T>(value);
            }

            // Unconnected: the pin's literal, keyed by its name in this node's own properties -- only meaningful
            // when T is actually one of NodeGraphValue's alternatives (a "Pose"-typed pin, say, never has a
            // literal; its unconnected default is just a default-constructed T).
            if constexpr (kIsNodeGraphValueType<T>)
            {
                const std::string& pinName = node.Type->Inputs[inputPinIndex].Name;
                auto it = node.Properties.find(pinName);
                if (it == node.Properties.end())
                    return T{};
                const T* value = std::get_if<T>(&it->second);
                return value ? *value : T{};
            }
            else
            {
                return T{};
            }
        }

        template<typename T>
        void SetOutput(uint32_t compiledNodeIndex, uint32_t outputPinIndex, T value)
        {
            const CompiledNode& node = m_Graph->Nodes[compiledNodeIndex];
            m_OutputValues[node.OutputSlotBase + outputPinIndex] = std::move(value);
        }

        // Persistent state for one node (default-constructed on first access), kept across evaluations -- e.g. a
        // Clip node's playhead time. Never store the returned reference past the current Evaluate call.
        template<typename T>
        T& GetState(uint32_t compiledNodeIndex)
        {
            std::any& slot = m_NodeState[compiledNodeIndex];
            if (!slot.has_value())
                slot = T{};
            return *std::any_cast<T>(&slot);
        }

        // A node-specific property that isn't one of its declared input pins (e.g. a Clip node's loop flag). T
        // must be one of NodeGraphValue's alternatives (see kIsNodeGraphValueType) -- properties are always literals.
        template<typename T>
        T GetProperty(uint32_t compiledNodeIndex, const std::string& name, T fallback = T{}) const
        {
            static_assert(kIsNodeGraphValueType<T>, "GraphEvalContext::GetProperty - T must be a NodeGraphValue alternative");
            const CompiledNode& node = m_Graph->Nodes[compiledNodeIndex];
            auto it = node.Properties.find(name);
            if (it == node.Properties.end())
                return fallback;
            const T* value = std::get_if<T>(&it->second);
            return value ? *value : fallback;
        }

        const CompiledGraph& Graph() const { return *m_Graph; }

        void* UserData = nullptr; // domain-defined payload for this evaluation (e.g. delta time, parameter blackboard, skeleton)

    private:
        const CompiledGraph* m_Graph = nullptr;
        std::vector<std::any> m_OutputValues; // size Graph().TotalOutputSlots, indexed by CompiledNode::OutputSlotBase + pin
        std::vector<std::any> m_NodeState;    // size Graph().Nodes.size(), one slot per node
    };

    // Evaluates every node of graph in compiled order (which already guarantees each node's inputs were
    // evaluated first) and writes their outputs into ctx. ctx must have been Init()'d against graph.
    void EvaluateGraph(GraphEvalContext& ctx, const CompiledGraph& graph);
}
