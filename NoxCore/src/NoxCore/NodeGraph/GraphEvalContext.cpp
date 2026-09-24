#include "GraphEvalContext.h"

namespace Nox
{
    void GraphEvalContext::Init(const CompiledGraph& graph)
    {
        m_Graph = &graph;
        m_OutputValues.clear();
        m_OutputValues.resize(graph.TotalOutputSlots);
        m_NodeState.clear();
        m_NodeState.resize(graph.Nodes.size());
    }

    void EvaluateGraph(GraphEvalContext& ctx, const CompiledGraph& graph)
    {
        for (uint32_t i = 0; i < (uint32_t)graph.Nodes.size(); i++)
        {
            const CompiledNode& node = graph.Nodes[i];
            if (node.Type->Evaluate)
                node.Type->Evaluate(ctx, i);
        }
    }
}
