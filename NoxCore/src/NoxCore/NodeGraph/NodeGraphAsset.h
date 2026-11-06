#pragma once

#include "NoxCore/Asset/Asset.h"

#include "CompiledGraph.h"
#include "GraphCompiler.h"
#include "NodeGraph.h"

namespace Nox
{
    // The saved/editable node graph as an engine asset: the authored NodeGraph (what the editor shows and what
    // NodeGraphSerializer writes to disk) plus its current CompiledGraph (rebuilt by Recompile(), read by every
    // running GraphEvalContext). One class for every domain: AssetType::AnimationGraph (.nanimgraph) and
    // AssetType::AudioGraph (.naudiograph), nothing here being specific to either.
    class NodeGraphAsset : public Asset
    {
    public:
        virtual AssetType GetType() const override { return Type; }

        AssetType Type = AssetType::AnimationGraph;

        // Re-runs GraphCompiler on Graph and replaces Compiled. Call after loading and after every editor edit.
        void Recompile()
        {
            Compiled = GraphCompiler::Compile(Graph);
            CompileVersion++;
        }

        NodeGraph Graph;
        CompiledGraph Compiled;

        // Bumped by every Recompile(). A running GraphEvalContext is sized and typed against one specific
        // CompiledGraph (per-node state, per-output-slot values), so anything holding one must re-Init() when
        // this changes -- otherwise a live edit that adds/removes nodes makes it index its old buffers with the
        // new graph's indices (vector out of range, or a stale std::any of the wrong type in a reshuffled slot).
        uint32_t CompileVersion = 0;
    };
}
