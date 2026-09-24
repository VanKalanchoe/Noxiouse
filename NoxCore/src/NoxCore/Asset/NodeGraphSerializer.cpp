#include "NodeGraphSerializer.h"

#include <fstream>
#include <type_traits>

#include <yaml-cpp/yaml.h>

#include "NoxCore/Core/Log.h"

namespace YAML
{
    template <>
    struct convert<glm::vec2>
    {
        static Node encode(const glm::vec2& rhs)
        {
            Node node;
            node.push_back(rhs.x);
            node.push_back(rhs.y);
            node.SetStyle(EmitterStyle::Flow);
            return node;
        }

        static bool decode(const Node& node, glm::vec2& rhs)
        {
            if (!node.IsSequence() || node.size() != 2)
                return false;

            rhs.x = node[0].as<float>();
            rhs.y = node[1].as<float>();
            return true;
        }
    };
}

namespace Nox
{
    // Without this, `out << someGlmVec2` doesn't go through YAML::convert<glm::vec2> above at all -- Emitter's
    // operator<< has no overload for glm::vec2, so ADL falls through to GLM's own templated stream operator
    // (found via the vec2 argument, matching any stream-like type including Emitter&) and silently writes
    // "vec2(x, y)" text instead of a YAML sequence, which then fails to parse back on load. An exact, non-
    // template overload in this same namespace wins over that ADL candidate. SceneSerializer.cpp needs the
    // identical fix for the identical reason and already has its own copy -- `static` here (internal linkage)
    // so the two definitions don't collide at link time; ordinary unqualified lookup still finds this one from
    // anywhere in this translation unit, which is all it needs to do.
    static YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec2& v)
    {
        out << YAML::Flow;
        out << YAML::BeginSeq << v.x << v.y << YAML::EndSeq;
        return out;
    }

    // NodeGraphValue's alternatives, one short tag each, so a value round-trips to the exact variant
    // alternative it was -- a bare scalar like "1" is ambiguous between Int/U64/Float/Bool without this.
    void NodeGraphSerializer::EmitValue(YAML::Emitter& out, const NodeGraphValue& value)
    {
        out << YAML::BeginMap;
        std::visit([&out](auto&& v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
                out << YAML::Key << "Bool" << YAML::Value << v;
            else if constexpr (std::is_same_v<T, int32_t>)
                out << YAML::Key << "Int" << YAML::Value << v;
            else if constexpr (std::is_same_v<T, float>)
                out << YAML::Key << "Float" << YAML::Value << v;
            else if constexpr (std::is_same_v<T, glm::vec2>)
                out << YAML::Key << "Vec2" << YAML::Value << v;
            else if constexpr (std::is_same_v<T, uint64_t>)
                out << YAML::Key << "U64" << YAML::Value << v;
            else if constexpr (std::is_same_v<T, std::string>)
                out << YAML::Key << "Str" << YAML::Value << v;
        }, value);
        out << YAML::EndMap;
    }

    bool NodeGraphSerializer::ReadValue(const YAML::Node& node, NodeGraphValue& value)
    {
        if (node["Bool"]) { value = node["Bool"].as<bool>(); return true; }
        if (node["Int"]) { value = node["Int"].as<int32_t>(); return true; }
        if (node["Float"]) { value = node["Float"].as<float>(); return true; }
        if (node["Vec2"]) { value = node["Vec2"].as<glm::vec2>(); return true; }
        if (node["U64"]) { value = node["U64"].as<uint64_t>(); return true; }
        if (node["Str"]) { value = node["Str"].as<std::string>(); return true; }
        return false;
    }

    bool NodeGraphSerializer::Serialize(const std::filesystem::path& path, const NodeGraph& graph)
    {
        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Domain" << YAML::Value << graph.Domain;
        out << YAML::Key << "OutputNode" << YAML::Value << graph.OutputNode;
        out << YAML::Key << "NextNodeId" << YAML::Value << graph.NextNodeId;

        out << YAML::Key << "Nodes" << YAML::Value << YAML::BeginSeq;
        for (const GraphNode& node : graph.Nodes)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Id" << YAML::Value << node.Id;
            out << YAML::Key << "Type" << YAML::Value << node.TypeName;
            out << YAML::Key << "Position" << YAML::Value << node.EditorPosition;

            out << YAML::Key << "Properties" << YAML::Value << YAML::BeginSeq;
            for (const auto& [name, value] : node.Properties)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Name" << YAML::Value << name;
                out << YAML::Key << "Value";
                EmitValue(out, value);
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;

            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::Key << "Links" << YAML::Value << YAML::BeginSeq;
        for (const GraphLink& link : graph.Links)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "FromNode" << YAML::Value << link.FromNode;
            out << YAML::Key << "FromPin" << YAML::Value << link.FromPin;
            out << YAML::Key << "ToNode" << YAML::Value << link.ToNode;
            out << YAML::Key << "ToPin" << YAML::Value << link.ToPin;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::Key << "Parameters" << YAML::Value << YAML::BeginSeq;
        for (const GraphParameter& param : graph.Parameters)
        {
            out << YAML::BeginMap;
            out << YAML::Key << "Name" << YAML::Value << param.Name;
            out << YAML::Key << "Default";
            EmitValue(out, param.DefaultValue);
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;

        out << YAML::EndMap;

        std::ofstream fout(path);
        if (!fout.is_open())
        {
            NOX_CORE_ERROR("NodeGraphSerializer::Serialize - could not open {} for writing", path.string());
            return false;
        }
        fout << out.c_str();
        return true;
    }

    bool NodeGraphSerializer::Deserialize(const std::filesystem::path& path, NodeGraph& graph)
    {
        YAML::Node data;
        try
        {
            data = YAML::LoadFile(path.string());
        }
        catch (const YAML::Exception& e)
        {
            NOX_CORE_ERROR("NodeGraphSerializer::Deserialize - failed to parse {}: {}", path.string(), e.what());
            return false;
        }

        // Every .as<T>() below can throw YAML::TypedBadConversion (e.g. a corrupted or hand-edited field) --
        // caught the same way as the LoadFile parse above, so a bad file logs an error instead of crashing
        // whatever triggered the load (an auto-reimport off the asset watcher, in particular, runs with nothing
        // upstream expecting an exception to escape).
        try
        {
            graph = NodeGraph{};
            if (data["Domain"])
                graph.Domain = data["Domain"].as<std::string>();
            if (data["OutputNode"])
                graph.OutputNode = data["OutputNode"].as<uint32_t>();
            if (data["NextNodeId"])
                graph.NextNodeId = data["NextNodeId"].as<uint32_t>();

            if (auto nodes = data["Nodes"])
            {
                for (const auto& nodeData : nodes)
                {
                    GraphNode node;
                    node.Id = nodeData["Id"].as<uint32_t>();
                    node.TypeName = nodeData["Type"].as<std::string>();
                    if (nodeData["Position"])
                        node.EditorPosition = nodeData["Position"].as<glm::vec2>();

                    if (auto props = nodeData["Properties"])
                    {
                        for (const auto& propData : props)
                        {
                            std::string name = propData["Name"].as<std::string>();
                            NodeGraphValue value;
                            if (propData["Value"] && ReadValue(propData["Value"], value))
                                node.Properties[name] = value;
                        }
                    }
                    graph.Nodes.push_back(std::move(node));
                }
            }

            if (auto links = data["Links"])
            {
                for (const auto& linkData : links)
                {
                    GraphLink link;
                    link.FromNode = linkData["FromNode"].as<uint32_t>();
                    link.FromPin = linkData["FromPin"].as<uint32_t>();
                    link.ToNode = linkData["ToNode"].as<uint32_t>();
                    link.ToPin = linkData["ToPin"].as<uint32_t>();
                    graph.Links.push_back(link);
                }
            }

            if (auto parameters = data["Parameters"])
            {
                for (const auto& paramData : parameters)
                {
                    GraphParameter param;
                    param.Name = paramData["Name"].as<std::string>();
                    if (paramData["Default"])
                        ReadValue(paramData["Default"], param.DefaultValue);
                    graph.Parameters.push_back(std::move(param));
                }
            }
        }
        catch (const YAML::Exception& e)
        {
            NOX_CORE_ERROR("NodeGraphSerializer::Deserialize - malformed data in {}: {}", path.string(), e.what());
            graph = NodeGraph{};
            return false;
        }

        return true;
    }
}
