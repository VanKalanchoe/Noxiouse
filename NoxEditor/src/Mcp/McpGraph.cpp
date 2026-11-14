#include "McpGraph.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "McpValidation.h"
#include "NoxCore/NodeGraph/NodeType.h"

namespace Nox::Mcp
{
    namespace
    {
        constexpr const char* CompareNames[] = { "Equal", "NotEqual", "Greater", "GreaterEqual", "Less", "LessEqual", "IsTrue", "IsFalse" };

        bool IsWhole(const Json& value)
        {
            return value.is_number_integer() || value.is_number_unsigned() ||
                   (value.is_number_float() && value.get<double>() == static_cast<double>(static_cast<int64_t>(value.get<double>())));
        }

        Json PinsJson(const std::vector<NodePinDesc>& pins)
        {
            Json list = Json::array();
            for (const NodePinDesc& pin : pins)
                list.push_back(Json{ { "name", pin.Name }, { "type", pin.Type } });
            return list;
        }

        // A pin of `pins` as JSON: its name, or its index when another pin has the same name.
        Json PinRef(const std::vector<NodePinDesc>& pins, uint32_t index)
        {
            if (index >= pins.size())
                return index;
            const std::string& name = pins[index].Name;
            const auto same = std::count_if(pins.begin(), pins.end(), [&](const NodePinDesc& pin) { return pin.Name == name; });
            return same == 1 ? Json(name) : Json(index);
        }

        std::string FindPin(const std::vector<NodePinDesc>& pins, const Json& ref, const char* side, uint32_t node, uint32_t& index)
        {
            std::string names;
            for (const NodePinDesc& pin : pins)
                names += (names.empty() ? "" : ", ") + std::format("\"{}\" ({})", pin.Name, pin.Type);
            if (ref.is_number_unsigned() || ref.is_number_integer())
            {
                const int64_t at = ref.get<int64_t>();
                if (at < 0 || at >= static_cast<int64_t>(pins.size()))
                    return std::format("node {} has no {} pin {} (its {} pins: {})", node, side, at, side, names.empty() ? "none" : names);
                index = static_cast<uint32_t>(at);
                return {};
            }
            if (!ref.is_string())
                return std::format("a link's {} pin must be a pin name or index", side);
            for (uint32_t i = 0; i < pins.size(); ++i)
                if (pins[i].Name == ref.get<std::string>())
                {
                    index = i;
                    return {};
                }
            return std::format("node {} has no {} pin \"{}\" (its {} pins: {})", node, side, ref.get<std::string>(), side, names.empty() ? "none" : names);
        }

        // The value type a node's property has: what its type declares (default properties, a literal pin, a variable group's
        // literal), else what it had before.
        const NodeGraphValue* ExpectedProperty(const NodeTypeDesc& type, const NodePinList& pins, const std::string& name, const GraphNode* before,
                                               NodeGraphValue& scratch)
        {
            for (const auto& [property, value] : type.DefaultProperties)
                if (property == name)
                    return &value;
            for (const auto& [pinName, value] : pins.Literals)
                if (pinName == name)
                    return &value;
            for (const NodePinDesc& pin : pins.Inputs)
                if (pin.Name == name)
                {
                    if (pin.Type == "Float") { scratch = 0.0f; return &scratch; }
                    if (pin.Type == "Int") { scratch = int32_t(0); return &scratch; }
                    if (pin.Type == "Bool") { scratch = false; return &scratch; }
                }
            for (const auto& [group, value] : type.VariableInputLiterals)
                if (name.rfind(group + " ", 0) == 0)
                    return &value;
            if (name == kVariableInputCountProperty)
            {
                scratch = int32_t(0);
                return &scratch;
            }
            if (before)
                if (auto found = before->Properties.find(name); found != before->Properties.end())
                    return &found->second;
            return nullptr;
        }

        Json GraphToJsonImpl(const NodeGraph& graph, bool root)
        {
            Json json = Json::object();
            if (root)
            {
                json["domain"] = graph.Domain;
                Json parameters = Json::array();
                for (const GraphParameter& parameter : graph.Parameters)
                {
                    Json item{ { "name", parameter.Name }, { "type", GraphParameterTypeName(parameter) } };
                    if (!parameter.IsTrigger)
                        item["default"] = GraphValueToJson(parameter.DefaultValue);
                    parameters.push_back(item);
                }
                json["parameters"] = parameters;
            }
            json["outputNode"] = graph.OutputNode == kInvalidGraphIndex ? Json() : Json(graph.OutputNode);

            std::map<uint32_t, NodePinList> pinsOf;
            Json nodes = Json::array();
            for (const GraphNode& node : graph.Nodes)
            {
                Json item{ { "id", node.Id }, { "type", node.TypeName }, { "position", Json::array({ node.EditorPosition.x, node.EditorPosition.y }) } };
                Json properties = Json::object();
                // (sorted: the map's order is not stable)
                std::vector<std::string> names;
                for (const auto& [name, value] : node.Properties)
                    names.push_back(name);
                std::sort(names.begin(), names.end());
                for (const std::string& name : names)
                    properties[name] = GraphValueToJson(node.Properties.at(name));
                item["properties"] = properties;
                if (!node.SubGraphs.empty())
                {
                    Json states = Json::array();
                    for (const NodeSubGraph& state : node.SubGraphs)
                        states.push_back(Json{ { "id", state.Id }, { "name", state.Name },
                                               { "position", Json::array({ state.EditorPosition.x, state.EditorPosition.y }) },
                                               { "graph", GraphToJsonImpl(state.Graph, false) } });
                    item["states"] = states;
                }
                if (!node.Transitions.empty())
                {
                    Json transitions = Json::array();
                    for (const NodeTransition& transition : node.Transitions)
                    {
                        Json rules = Json::array();
                        for (const TransitionRule& rule : transition.Rules)
                        {
                            const int compare = static_cast<int>(rule.Compare);
                            Json ruleJson{ { "parameter", rule.Parameter }, { "compare", compare >= 0 && compare < 8 ? CompareNames[compare] : "Greater" } };
                            if (rule.Compare != TransitionCompare::IsTrue && rule.Compare != TransitionCompare::IsFalse)
                                ruleJson["value"] = GraphValueToJson(rule.Value);
                            rules.push_back(ruleJson);
                        }
                        transitions.push_back(Json{ { "from", transition.FromState }, { "to", transition.ToState }, { "duration", transition.Duration },
                                                    { "easeInOut", transition.EaseInOut }, { "atStateEnd", transition.AtStateEnd }, { "rules", rules } });
                    }
                    item["transitions"] = transitions;
                }
                nodes.push_back(item);
                if (const NodeTypeDesc* type = NodeTypeRegistry::Find(graph.Domain, node.TypeName))
                    pinsOf[node.Id] = ResolveNodePins(*type, node.Properties);
            }
            json["nodes"] = nodes;

            Json links = Json::array();
            for (const GraphLink& link : graph.Links)
            {
                const auto from = pinsOf.find(link.FromNode), to = pinsOf.find(link.ToNode);
                Json item{ { "from", Json{ { "node", link.FromNode }, { "pin", from != pinsOf.end() ? PinRef(from->second.Outputs, link.FromPin) : Json(link.FromPin) } } },
                           { "to", Json{ { "node", link.ToNode }, { "pin", to != pinsOf.end() ? PinRef(to->second.Inputs, link.ToPin) : Json(link.ToPin) } } } };
                if (!link.Route.empty())
                    item["route"] = link.Route;
                links.push_back(item);
            }
            json["links"] = links;
            return json;
        }

        std::string ReadPosition(const Json& value, glm::vec2& out)
        {
            if (value.is_null())
                return {};
            if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number())
                return "'position' must be [x, y]";
            out = { value[0].get<float>(), value[1].get<float>() };
            return {};
        }

        std::string GraphFromJsonImpl(const Json& value, const std::string& domain, const std::set<std::string>& parameterNames, NodeGraph& graph,
                                      const NodeGraph* previous, const std::string& where)
        {
            auto at = [&](const std::string& problem) { return where.empty() ? problem : std::format("{}: {}", where, problem); };
            if (!value.is_object())
                return at("a graph must be an object { nodes, links, outputNode }");
            for (auto it = value.begin(); it != value.end(); ++it)
                if (it.key() != "domain" && it.key() != "parameters" && it.key() != "outputNode" && it.key() != "nodes" && it.key() != "links")
                    return at(std::format("unknown field '{}' (domain, parameters, outputNode, nodes, links)", it.key()));
            graph.Domain = domain;
            graph.Nodes.clear();
            graph.Links.clear();

            // Nodes.
            std::map<uint32_t, size_t> indexOf;
            std::map<uint32_t, NodePinList> pinsOf;
            uint32_t highest = 0;
            for (const Json& item : value.value("nodes", Json::array()))
            {
                if (!item.is_object() || !item.contains("id") || !item.contains("type"))
                    return at("each node needs 'id' and 'type'");
                for (auto it = item.begin(); it != item.end(); ++it)
                    if (it.key() != "id" && it.key() != "type" && it.key() != "position" && it.key() != "properties" && it.key() != "states" &&
                        it.key() != "transitions")
                        return at(std::format("a node has no field '{}' (id, type, position, properties, states, transitions)", it.key()));
                if (!item["id"].is_number_unsigned() && !(item["id"].is_number_integer() && item["id"].get<int64_t>() > 0))
                    return at("a node's 'id' must be a whole number above 0");
                GraphNode node;
                node.Id = item["id"].get<uint32_t>();
                if (node.Id == 0 || node.Id == kInvalidGraphIndex)
                    return at(std::format("node id {} is not allowed", node.Id));
                if (indexOf.contains(node.Id))
                    return at(std::format("two nodes have id {}", node.Id));
                node.TypeName = item["type"].is_string() ? item["type"].get<std::string>() : std::string();
                const NodeTypeDesc* type = NodeTypeRegistry::Find(domain, node.TypeName);
                if (!type)
                    return at(std::format("node {}: {} has no node type \"{}\" (list_graph_node_types lists them)", node.Id, domain, node.TypeName));
                if (std::string problem = ReadPosition(item.value("position", Json()), node.EditorPosition); !problem.empty())
                    return at(std::format("node {}: {}", node.Id, problem));

                const GraphNode* before = nullptr;
                if (previous)
                    for (const GraphNode& old : previous->Nodes)
                        if (old.Id == node.Id && old.TypeName == node.TypeName)
                            before = &old;
                // Properties: the count of a variable pin group first (the other pins' names depend on it).
                const Json properties = item.value("properties", Json::object());
                if (!properties.is_object())
                    return at(std::format("node {}: 'properties' must be an object", node.Id));
                // What it starts from: the node as it is (properties left out stay), or a new node of the type (its defaults, as the
                // editor seeds them).
                if (before)
                    node.Properties = before->Properties;
                else
                    for (const auto& [name, defaultValue] : type->DefaultProperties)
                        node.Properties[name] = defaultValue;
                if (properties.contains(kVariableInputCountProperty))
                {
                    NodeGraphValue count;
                    const NodeGraphValue expected = int32_t(0);
                    if (std::string problem = GraphValueFromJson(properties[kVariableInputCountProperty], &expected, count); !problem.empty())
                        return at(std::format("node {}: '{}' {}", node.Id, kVariableInputCountProperty, problem));
                    node.Properties[kVariableInputCountProperty] = count;
                }
                NodePinList pins = ResolveNodePins(*type, node.Properties);
                for (auto it = properties.begin(); it != properties.end(); ++it)
                {
                    if (it.key() == kVariableInputCountProperty)
                        continue;
                    NodeGraphValue scratch;
                    const NodeGraphValue* expected = ExpectedProperty(*type, pins, it.key(), before, scratch);
                    NodeGraphValue read;
                    if (std::string problem = GraphValueFromJson(it.value(), expected, read); !problem.empty())
                        return at(std::format("node {}: property '{}' {}", node.Id, it.key(), problem));
                    node.Properties[it.key()] = read;
                }
                // Literals of a variable pin group's pins not given ("Gain 2" of a third input): what the editor starts them with.
                const uint32_t groups = VariableInputCount(*type, node.Properties);
                for (uint32_t group = 0; group < groups; ++group)
                    for (const auto& [pinName, literal] : type->VariableInputLiterals)
                        node.Properties.try_emplace(std::format("{} {}", pinName, group), literal);
                pins = ResolveNodePins(*type, node.Properties); // (a graph node's pins depend on its graph property)
                for (const auto& [pinName, literal] : pins.Literals)
                    node.Properties.try_emplace(pinName, literal);

                // States and transitions (a state machine).
                if (item.contains("states") || item.contains("transitions"))
                {
                    if (!type->OwnsSubGraphs)
                        return at(std::format("node {}: a \"{}\" has no states", node.Id, node.TypeName));
                    std::set<uint32_t> stateIds;
                    for (const Json& stateJson : item.value("states", Json::array()))
                    {
                        if (!stateJson.is_object() || !stateJson.contains("id"))
                            return at(std::format("node {}: each state needs 'id' (and 'name', 'graph')", node.Id));
                        NodeSubGraph state;
                        state.Id = stateJson["id"].get<uint32_t>();
                        if (!stateIds.insert(state.Id).second)
                            return at(std::format("node {}: two states have id {}", node.Id, state.Id));
                        state.Name = stateJson.value("name", std::string());
                        if (std::string problem = ReadPosition(stateJson.value("position", Json()), state.EditorPosition); !problem.empty())
                            return at(std::format("node {} state {}: {}", node.Id, state.Id, problem));
                        const NodeGraph* oldState = nullptr;
                        if (before)
                            for (const NodeSubGraph& old : before->SubGraphs)
                                if (old.Id == state.Id)
                                    oldState = &old.Graph;
                        if (std::string problem = GraphFromJsonImpl(stateJson.value("graph", Json::object()), domain, parameterNames, state.Graph, oldState,
                                                                    std::format("{}node {} state \"{}\"", where.empty() ? "" : where + ", ", node.Id, state.Name));
                            !problem.empty())
                            return problem;
                        node.SubGraphs.push_back(std::move(state));
                    }
                    for (const Json& transitionJson : item.value("transitions", Json::array()))
                    {
                        if (!transitionJson.is_object() || !transitionJson.contains("from") || !transitionJson.contains("to"))
                            return at(std::format("node {}: each transition needs 'from' and 'to' (state ids)", node.Id));
                        NodeTransition transition;
                        transition.FromState = transitionJson["from"].get<uint32_t>();
                        transition.ToState = transitionJson["to"].get<uint32_t>();
                        if (!stateIds.contains(transition.FromState) || !stateIds.contains(transition.ToState))
                            return at(std::format("node {}: a transition {} -> {} names a state that is not there", node.Id, transition.FromState, transition.ToState));
                        transition.Duration = transitionJson.value("duration", 0.2f);
                        transition.EaseInOut = transitionJson.value("easeInOut", true);
                        transition.AtStateEnd = transitionJson.value("atStateEnd", false);
                        for (const Json& ruleJson : transitionJson.value("rules", Json::array()))
                        {
                            TransitionRule rule;
                            rule.Parameter = ruleJson.value("parameter", std::string());
                            if (!parameterNames.contains(rule.Parameter))
                                return at(std::format("node {}: a transition rule reads \"{}\", which is not a parameter of the graph", node.Id, rule.Parameter));
                            const std::string compare = ruleJson.value("compare", std::string("Greater"));
                            const auto found = std::find(std::begin(CompareNames), std::end(CompareNames), compare);
                            if (found == std::end(CompareNames))
                                return at(std::format("node {}: compare \"{}\" is none of Equal, NotEqual, Greater, GreaterEqual, Less, LessEqual, IsTrue, IsFalse",
                                                      node.Id, compare));
                            rule.Compare = static_cast<TransitionCompare>(found - std::begin(CompareNames));
                            if (ruleJson.contains("value"))
                                if (std::string problem = GraphValueFromJson(ruleJson["value"], nullptr, rule.Value); !problem.empty())
                                    return at(std::format("node {}: a rule's value {}", node.Id, problem));
                            transition.Rules.push_back(std::move(rule));
                        }
                        node.Transitions.push_back(std::move(transition));
                    }
                }

                highest = std::max(highest, node.Id);
                indexOf[node.Id] = graph.Nodes.size();
                pinsOf[node.Id] = pins;
                graph.Nodes.push_back(std::move(node));
            }
            graph.NextNodeId = highest + 1;

            // Links, by pin name (or index).
            for (const Json& item : value.value("links", Json::array()))
            {
                if (!item.is_object() || !item.contains("from") || !item.contains("to") || !item["from"].is_object() || !item["to"].is_object())
                    return at("each link needs 'from' and 'to': { node, pin }");
                GraphLink link;
                const Json& from = item["from"];
                const Json& to = item["to"];
                if (!from.contains("node") || !to.contains("node"))
                    return at("a link end needs 'node' and 'pin'");
                link.FromNode = from["node"].get<uint32_t>();
                link.ToNode = to["node"].get<uint32_t>();
                if (!pinsOf.contains(link.FromNode) || !pinsOf.contains(link.ToNode))
                    return at(std::format("a link {} -> {} names a node that is not there", link.FromNode, link.ToNode));
                if (std::string problem = FindPin(pinsOf[link.FromNode].Outputs, from.value("pin", Json(0)), "output", link.FromNode, link.FromPin); !problem.empty())
                    return at(problem);
                if (std::string problem = FindPin(pinsOf[link.ToNode].Inputs, to.value("pin", Json(0)), "input", link.ToNode, link.ToPin); !problem.empty())
                    return at(problem);
                const bool taken = std::any_of(graph.Links.begin(), graph.Links.end(),
                                               [&](const GraphLink& other) { return other.ToNode == link.ToNode && other.ToPin == link.ToPin; });
                if (taken)
                    return at(std::format("two links go into node {}'s input \"{}\" (an input takes one)", link.ToNode,
                                          pinsOf[link.ToNode].Inputs[link.ToPin].Name));
                if (item.contains("route") && item["route"].is_array())
                    for (const Json& coordinate : item["route"])
                        if (coordinate.is_number())
                            link.Route.push_back(coordinate.get<float>());
                graph.Links.push_back(std::move(link));
            }

            // The output node.
            const Json output = value.value("outputNode", Json());
            if (output.is_null())
                graph.OutputNode = kInvalidGraphIndex;
            else
            {
                if (!output.is_number_unsigned() && !output.is_number_integer())
                    return at("'outputNode' must be a node id");
                graph.OutputNode = output.get<uint32_t>();
                if (!indexOf.contains(graph.OutputNode))
                    return at(std::format("'outputNode' {} is not a node of the graph", graph.OutputNode));
            }
            return {};
        }
    }

    Json GraphValueToJson(const NodeGraphValue& value)
    {
        return std::visit([](const auto& v) -> Json
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int32_t> || std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, float>)
                return static_cast<double>(v);
            else if constexpr (std::is_same_v<T, glm::vec2>)
                return Json::array({ v.x, v.y });
            else if constexpr (std::is_same_v<T, uint64_t>)
                return std::to_string(v); // (an asset id: digits, as every MCP tool writes ids)
            else
            {
                Json keys = Json::array();
                for (const glm::vec2& key : v.Keys)
                    keys.push_back(Json::array({ key.x, key.y }));
                return Json{ { "Curve", keys }, { "Smooth", v.Smooth } };
            }
        }, value);
    }

    std::string GraphValueFromJson(const Json& json, const NodeGraphValue* expected, NodeGraphValue& value)
    {
        auto curve = [&]() -> std::string
        {
            const Json keys = json.is_object() ? json.value("Curve", Json()) : json;
            if (!keys.is_array())
                return "must be a curve: { \"Curve\": [[x, y], ...], \"Smooth\": false }";
            NodeGraphCurve result;
            for (const Json& key : keys)
            {
                if (!key.is_array() || key.size() != 2 || !key[0].is_number() || !key[1].is_number())
                    return "a curve's keys must be [x, y] pairs";
                result.Keys.emplace_back(key[0].get<float>(), key[1].get<float>());
            }
            std::sort(result.Keys.begin(), result.Keys.end(), [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
            result.Smooth = json.is_object() && json.value("Smooth", false);
            value = std::move(result);
            return {};
        };
        if (expected)
        {
            return std::visit([&](const auto& like) -> std::string
            {
                using T = std::decay_t<decltype(like)>;
                if constexpr (std::is_same_v<T, bool>)
                {
                    if (!json.is_boolean())
                        return "must be true or false";
                    value = json.get<bool>();
                }
                else if constexpr (std::is_same_v<T, int32_t>)
                {
                    if (!json.is_number() || !IsWhole(json))
                        return "must be a whole number";
                    value = static_cast<int32_t>(json.get<int64_t>());
                }
                else if constexpr (std::is_same_v<T, float>)
                {
                    if (!json.is_number())
                        return "must be a number";
                    value = json.get<float>();
                }
                else if constexpr (std::is_same_v<T, glm::vec2>)
                {
                    if (!json.is_array() || json.size() != 2 || !json[0].is_number() || !json[1].is_number())
                        return "must be [x, y]";
                    value = glm::vec2(json[0].get<float>(), json[1].get<float>());
                }
                else if constexpr (std::is_same_v<T, uint64_t>)
                {
                    uint64_t id = 0;
                    if (json.is_string() && (IsIdText(json) || json.get<std::string>() == "0"))
                        id = std::stoull(json.get<std::string>());
                    else if (json.is_number_unsigned() || (json.is_number_integer() && json.get<int64_t>() >= 0))
                    {
                        if (json.get<uint64_t>() > (1ull << 53))
                            return "is an id: write it as a string of digits (a JSON number loses its last digits)";
                        id = json.get<uint64_t>();
                    }
                    else
                        return "must be an asset id (a string of digits) or \"0\"";
                    value = id;
                }
                else if constexpr (std::is_same_v<T, std::string>)
                {
                    if (!json.is_string())
                        return "must be a string";
                    value = json.get<std::string>();
                }
                else
                    return curve();
                return {};
            }, *expected);
        }
        // Nothing declared: by the JSON's kind.
        if (json.is_boolean())
            value = json.get<bool>();
        else if (json.is_number_integer() || json.is_number_unsigned())
        {
            if (json.is_number_unsigned() && json.get<uint64_t>() > static_cast<uint64_t>(INT32_MAX))
                return "is too large for a whole number (an id is written as a string of digits)";
            value = static_cast<int32_t>(json.get<int64_t>());
        }
        else if (json.is_number_float())
            value = json.get<float>();
        else if (json.is_array() && json.size() == 2 && json[0].is_number() && json[1].is_number())
            value = glm::vec2(json[0].get<float>(), json[1].get<float>());
        else if (IsIdText(json))
            value = static_cast<uint64_t>(std::stoull(json.get<std::string>()));
        else if (json.is_string())
            value = json.get<std::string>();
        else if (json.is_object() && json.contains("Curve"))
            return curve();
        else
            return "must be a bool, number, [x, y], string or curve";
        return {};
    }

    Json GraphToJson(const NodeGraph& graph)
    {
        return GraphToJsonImpl(graph, true);
    }

    std::string GraphFromJson(const Json& value, const std::string& domain, NodeGraph& graph, const NodeGraph* previous)
    {
        if (!value.is_object())
            return "the graph must be an object (get_graph shows the shape)";
        if (value.contains("domain") && value["domain"] != domain)
            return std::format("the graph is a {} (its domain cannot change)", domain);
        NodeGraph result;
        std::set<std::string> parameterNames;
        for (const Json& item : value.value("parameters", Json::array()))
        {
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string() || item["name"].get<std::string>().empty())
                return "each parameter needs a 'name'";
            GraphParameter parameter;
            parameter.Name = item["name"].get<std::string>();
            if (!parameterNames.insert(parameter.Name).second)
                return std::format("two parameters are named \"{}\"", parameter.Name);
            const std::string type = item.value("type", std::string("Float"));
            if (type == "Trigger")
                parameter.IsTrigger = true;
            else
            {
                NodeGraphValue expected = type == "Bool" ? NodeGraphValue(false) : type == "Int" ? NodeGraphValue(int32_t(0)) : NodeGraphValue(0.0f);
                if (type != "Bool" && type != "Int" && type != "Float")
                    return std::format("parameter \"{}\": type \"{}\" is none of Float, Int, Bool, Trigger", parameter.Name, type);
                parameter.DefaultValue = expected;
                if (item.contains("default"))
                    if (std::string problem = GraphValueFromJson(item["default"], &expected, parameter.DefaultValue); !problem.empty())
                        return std::format("parameter \"{}\": 'default' {}", parameter.Name, problem);
            }
            result.Parameters.push_back(std::move(parameter));
        }
        try
        {
            if (std::string problem = GraphFromJsonImpl(value, domain, parameterNames, result, previous, {}); !problem.empty())
                return problem;
        }
        catch (const Json::exception& error)
        {
            return std::format("a field has the wrong kind ({})", error.what()); // (a node id as text, a pin index as an object ...)
        }
        graph = std::move(result);
        return {};
    }

    Json NodeTypesJson(const std::string& domain)
    {
        std::vector<const NodeTypeDesc*> types;
        for (const NodeTypeDesc& type : NodeTypeRegistry::All())
            if (type.Domain == domain)
                types.push_back(&type);
        std::sort(types.begin(), types.end(), [](const NodeTypeDesc* a, const NodeTypeDesc* b)
        {
            return a->Category != b->Category ? a->Category < b->Category : a->TypeName < b->TypeName;
        });
        Json list = Json::array();
        for (const NodeTypeDesc* type : types)
        {
            Json item{ { "type", type->TypeName }, { "category", type->Category }, { "inputs", PinsJson(type->Inputs) }, { "outputs", PinsJson(type->Outputs) } };
            Json properties = Json::object();
            for (const auto& [name, value] : type->DefaultProperties)
                properties[name] = GraphValueToJson(value);
            item["properties"] = properties;
            if (!type->VariableInputs.empty())
            {
                item["variableInputs"] = PinsJson(type->VariableInputs);
                item["variableInputsNote"] = std::format("repeated '{}' times (at least {}): pins named \"<name> 0\", \"<name> 1\" ...",
                                                         kVariableInputCountProperty, type->MinVariableInputs);
            }
            if (!type->PropertyEnums.empty())
                item["enums"] = type->PropertyEnums;
            if (!type->PropertyRanges.empty())
            {
                Json ranges = Json::object();
                for (const auto& [name, range] : type->PropertyRanges)
                    ranges[name] = Json::array({ range.first, range.second });
                item["ranges"] = ranges;
            }
            if (!type->PropertyAssetTypeHints.empty())
                item["assetProperties"] = type->PropertyAssetTypeHints; // (an asset id of that kind)
            if (type->OwnsSubGraphs)
                item["hasStates"] = true;
            if (!type->ParameterType.empty())
                item["readsParameterOfType"] = type->ParameterType;
            if (type->NodePins)
                item["pinsDependOnProperties"] = true;
            list.push_back(item);
        }
        return list;
    }
}
