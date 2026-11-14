#pragma once
#include <string>

#include "McpProtocol.h"
#include "NoxCore/NodeGraph/NodeGraph.h"

// Node graphs (.nanimgraph animation graphs, .naudiograph audio graphs) as JSON for the MCP tools get_graph / set_graph /
// list_graph_node_types. Friendlier than the file's tagged YAML: links name their pins ("Pose", "Alpha"), property values are plain
// JSON typed by the node type's own declarations (its default properties, its Float / Int / Bool pins), state machine states nest
// their graphs, transition rules name their comparison. Every reference is checked when reading (node types of the domain, node ids,
// pin names, state ids, parameter names); the compiler's own checks (pin types, cycles) come after. Plain data (tested in NoxToolsTest).
//
//   { "domain": "AnimationGraph", "outputNode": 1,
//     "parameters": [ { "name": "Speed", "type": "Float", "default": 0.0 } ],
//     "nodes": [ { "id": 1, "type": "Output Pose", "position": [400, 0], "properties": {} },
//                { "id": 2, "type": "Clip", "position": [0, 0], "properties": { "Clip": "17644891489966042489", "Loop": true } } ],
//     "links": [ { "from": { "node": 2, "pin": "Pose" }, "to": { "node": 1, "pin": "Pose" } } ] }
//
// A state machine node adds "states": [ { "id", "name", "position", "graph": { nodes, links, outputNode } } ] and "transitions":
// [ { "from", "to", "duration", "easeInOut", "atStateEnd", "rules": [ { "parameter", "compare", "value" } ] } ].
namespace Nox::Mcp
{
    Json GraphToJson(const NodeGraph& graph);
    // Reads `value` into `graph` (replacing it) for `domain`; "" or the first problem found. `previous` (optional): the graph as it is,
    // whose property value types are kept where the node type declares none.
    std::string GraphFromJson(const Json& value, const std::string& domain, NodeGraph& graph, const NodeGraph* previous = nullptr);
    // The node types a domain offers: pins (by name and type), properties with their defaults, enum names, ranges, asset kinds.
    Json NodeTypesJson(const std::string& domain);

    // A graph value as JSON (ids as digit strings, curves as { Curve: [[x, y] ...], Smooth }) and back, to the alternative of
    // `expected` when given (else inferred: bool, whole number -> Int, number -> Float, [x, y] -> Vec2, id text -> U64, text).
    Json GraphValueToJson(const NodeGraphValue& value);
    std::string GraphValueFromJson(const Json& json, const NodeGraphValue* expected, NodeGraphValue& value);
}
