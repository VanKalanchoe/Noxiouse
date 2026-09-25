#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "NodeGraphTypes.h"

namespace Nox
{
    class GraphEvalContext;

    // A node type's behavior: read this node's already-evaluated input pins (and its literal properties) from
    // ctx, write its own output pins into ctx. Called once per node per graph evaluation, in the compiler's
    // topological order -- every input is guaranteed already evaluated by the time this runs.
    using NodeEvaluateFn = std::function<void(GraphEvalContext& ctx, uint32_t compiledNodeIndex)>;

    // Describes one kind of node (e.g. "Clip", "Blend2D", "Output") a domain makes available to its graphs.
    // Registered once, at startup, by the owning domain (e.g. an AnimationGraphNodes.cpp); NodeGraph/ itself
    // never constructs one of these.
    struct NodeTypeDesc
    {
        std::string TypeName; // unique within its Domain; what GraphNode::TypeName references
        std::string Domain;   // e.g. "AnimationGraph" -- which NodeGraph::Domain may use this type
        std::string Category; // editor grouping, e.g. "Sources", "Blending"
        std::vector<NodePinDesc> Inputs;
        std::vector<NodePinDesc> Outputs;
        // Seeded into a new GraphNode's Properties when the editor creates one of this type, so it has
        // something editable (and a usable default) immediately instead of an empty map with nothing to fill
        // in. Existing nodes loaded from disk are unaffected -- this only applies at creation time.
        std::vector<std::pair<std::string, NodeGraphValue>> DefaultProperties;
        // For a uint64_t property that's really an AssetHandle: which asset type an editor's asset picker
        // should filter to, as the same string AssetTypeToString/AssetTypeFromString use (e.g.
        // "AssetType::AnimationSequence") -- kept as an opaque string, not an AssetType, so this core module
        // stays domain-agnostic and doesn't depend on the asset system, same as NodePinDesc::Type.
        std::unordered_map<std::string, std::string> PropertyAssetTypeHints;
        // For a float property with a meaningful range (e.g. a 0-1 blend factor): an editor shows a slider
        // clamped to it instead of an open-ended drag box. Absent = unbounded.
        std::unordered_map<std::string, std::pair<float, float>> PropertyRanges;
        NodeEvaluateFn Evaluate;
        // The node owns sub graphs (GraphNode::SubGraphs) -- a state machine -- and an editor opens them on double-click.
        bool OwnsSubGraphs = false;
    };

    // Process-wide registry of node types, shared by every domain. Domain-agnostic: it only stores and looks up
    // NodeTypeDesc by (Domain, TypeName) -- this is what makes the graph core reusable for a future domain
    // (e.g. an audio graph) without touching anything under NodeGraph/.
    class NodeTypeRegistry
    {
    public:
        static void Register(NodeTypeDesc desc);
        static const NodeTypeDesc* Find(const std::string& domain, const std::string& typeName);
        static const std::vector<NodeTypeDesc>& All();

    private:
        static std::vector<NodeTypeDesc>& Types();
    };
}
