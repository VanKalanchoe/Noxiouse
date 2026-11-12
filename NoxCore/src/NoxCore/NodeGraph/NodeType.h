#pragma once

#include <algorithm>
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
        // Unreal's "Add Pin" (Blend Poses by Int, Blend Multi, MetaSounds' Mixer): after Inputs, this group of pins repeated as
        // often as the node's kVariableInputCountProperty says -- one pin ("Pose 0", "Pose 1", ...) or several together ("In 0",
        // "Gain 0", "In 1", "Gain 1", ...) -- never fewer than MinVariableInputs times. Empty: none.
        std::vector<NodePinDesc> VariableInputs;
        // The literal a new group's pins start with, by the group pin's name ("Gain" -> 1.0): seeded as "<name> <n>" properties.
        std::unordered_map<std::string, NodeGraphValue> VariableInputLiterals;
        uint32_t MinVariableInputs = 2;
        // A node reading a graph parameter (kParameterReferenceProperty) reads parameters of this type only ("Float", "Int",
        // "Bool", "Trigger": GraphParameterTypeName); empty = any. The editor offers each parameter with the reader of its type.
        std::string ParameterType;
        // For an int property that picks one of several options (an oscillator's waveform, a filter's type): their names, by value.
        std::unordered_map<std::string, std::vector<std::string>> PropertyEnums;
        // Pins that depend on the node itself (MetaSounds' graph node: the graph it plays decides its inputs and outputs). Set: it
        // gives this node's input and output pins from its properties, in place of Inputs, VariableInputs and Outputs (and the
        // literals its value inputs start with).
        std::function<void(const std::unordered_map<std::string, NodeGraphValue>& properties, struct NodePinList& out)> NodePins;
    };

    // How many copies of its type's VariableInput a node has (an int property; the editor's + / - pins change it).
    inline constexpr const char* kVariableInputCountProperty = "InputCount";

    // How many times the node repeats its type's VariableInputs group.
    inline uint32_t VariableInputCount(const NodeTypeDesc& type, const std::unordered_map<std::string, NodeGraphValue>& properties)
    {
        if (type.VariableInputs.empty())
            return 0;
        const auto found = properties.find(kVariableInputCountProperty);
        const int32_t* count = found != properties.end() ? std::get_if<int32_t>(&found->second) : nullptr;
        return std::max<uint32_t>(type.MinVariableInputs, count && *count > 0 ? static_cast<uint32_t>(*count) : 0u);
    }

    // A node's input pins: its type's fixed ones, then its variable groups.
    inline uint32_t InputPinCount(const NodeTypeDesc& type, const std::unordered_map<std::string, NodeGraphValue>& properties)
    {
        return static_cast<uint32_t>(type.Inputs.size() + type.VariableInputs.size() * VariableInputCount(type, properties));
    }

    inline NodePinDesc InputPin(const NodeTypeDesc& type, uint32_t index)
    {
        if (index < type.Inputs.size())
            return type.Inputs[index];
        const uint32_t variable = index - static_cast<uint32_t>(type.Inputs.size());
        const uint32_t groupSize = static_cast<uint32_t>(type.VariableInputs.size());
        const NodePinDesc& pin = type.VariableInputs[variable % groupSize];
        return { pin.Name + " " + std::to_string(variable / groupSize), pin.Type };
    }

    // A node's pins with NodeTypeDesc::NodePins taken into account: the editor and the compiler's link checks use these.
    struct NodePinList
    {
        std::vector<NodePinDesc> Inputs;
        std::vector<NodePinDesc> Outputs;
        std::vector<std::pair<std::string, NodeGraphValue>> Literals; // NodePins: the value inputs' starting literals, by pin name
    };
    inline NodePinList ResolveNodePins(const NodeTypeDesc& type, const std::unordered_map<std::string, NodeGraphValue>& properties)
    {
        NodePinList pins;
        if (type.NodePins)
        {
            type.NodePins(properties, pins);
            return pins;
        }
        const uint32_t count = InputPinCount(type, properties);
        pins.Inputs.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
            pins.Inputs.push_back(InputPin(type, index));
        pins.Outputs = type.Outputs;
        return pins;
    }

    // Unreal's reroute node (a knot): a bend point in a wire, one input and one output of the wire's type ("Type", a string property:
    // set from the wire it is put on, or the first wire connected to an untyped one). Only the editor sees it: the compiler takes
    // every reroute out (what feeds it feeds what it fed) before compiling, so no domain runs one.
    inline constexpr const char* kRerouteNodeType = "Reroute";
    inline constexpr const char* kRerouteTypeProperty = "Type";
    // Registers the Reroute node type in `domain` (each graph domain calls it with its node types).
    void RegisterRerouteNodeType(const std::string& domain);

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
