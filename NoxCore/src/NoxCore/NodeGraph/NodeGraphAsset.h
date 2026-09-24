#pragma once

#include "NoxCore/Asset/Asset.h"

#include "CompiledGraph.h"
#include "GraphCompiler.h"
#include "NodeGraph.h"

namespace Nox
{
    // The saved/editable node graph as an engine asset: the authored NodeGraph (what the editor shows and what
    // NodeGraphSerializer writes to disk) plus its current CompiledGraph (rebuilt by Recompile(), read by every
    // running GraphEvalContext). AssetType::AnimationGraph today; a future audio graph domain would reuse this
    // exact class under its own AssetType, since nothing here is animation-specific.
    class NodeGraphAsset : public Asset
    {
    public:
        static AssetType GetStaticType() { return AssetType::AnimationGraph; }
        virtual AssetType GetType() const override { return GetStaticType(); }

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
