#pragma once

#include "CompiledGraph.h"
#include "NodeGraph.h"

namespace Nox
{
    class GraphCompiler
    {
    public:
        // Validates every node's type exists in the graph's Domain, every link's pin types match, the node
        // graph (as expressed by Links) has no cycles, and OutputNode resolves to a real node, then flattens it
        // into evaluation order. Returns an empty (IsValid() == false) CompiledGraph and logs the reason on
        // failure -- never partial output.
        static CompiledGraph Compile(const NodeGraph& graph);
    };
}
