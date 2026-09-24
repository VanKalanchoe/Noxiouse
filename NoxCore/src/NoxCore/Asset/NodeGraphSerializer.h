#pragma once

#include <filesystem>

#include <yaml-cpp/yaml.h>

#include "NoxCore/NodeGraph/NodeGraph.h"

namespace Nox
{
    // NODE GRAPH SERIALIZER (.nanimgraph, and any future *graph domain reusing NodeGraph)
    // Plain YAML, matching SceneSerializer's style -- authored graphs are meant to be human-diffable and
    // hand-editable, unlike the binary .nanim/.nmesh cooked formats.
    class NodeGraphSerializer
    {
    public:
        static bool Serialize(const std::filesystem::path& path, const NodeGraph& graph);
        static bool Deserialize(const std::filesystem::path& path, NodeGraph& graph);

        // A single NodeGraphValue, tagged by alternative so it round-trips to the exact type it was. Exposed so
        // anything else serializing a NodeGraphValue (e.g. SceneSerializer's AnimatorComponent graph-mode
        // parameter overrides) uses the same encoding instead of a second copy of it.
        static void EmitValue(YAML::Emitter& out, const NodeGraphValue& value);
        static bool ReadValue(const YAML::Node& node, NodeGraphValue& value);
    };
}
