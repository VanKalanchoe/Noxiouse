#include "McpValidation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

namespace Nox::Mcp
{
    bool IsIdText(const Json& value)
    {
        if (!value.is_string())
            return false;
        const std::string& text = value.get_ref<const std::string&>();
        return text.size() >= 16 && text.size() <= 20 && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
    }

    namespace
    {
        bool IsNumberVector(const Json& value)
        {
            return value.is_array() && !value.empty() && value.size() <= 4 &&
                   std::all_of(value.begin(), value.end(), [](const Json& item) { return item.is_number(); });
        }

        std::string Where(const std::string& path)
        {
            return path.empty() ? std::string("the value") : std::format("'{}'", path);
        }
    }

    std::string KindOf(const Json& value)
    {
        if (IsIdText(value))
            return "id";
        if (value.is_boolean())
            return "bool";
        if (value.is_number())
            return "number"; // (whole or not: the file does not tell -- see CheckAgainstExample)
        if (value.is_string())
            return "string";
        if (IsNumberVector(value))
            return std::format("vector{}", value.size());
        if (value.is_array())
            return "list";
        if (value.is_object())
            return "object";
        return "null";
    }

    std::string CheckAgainstExample(const Json& example, const Json& value, const std::string& path)
    {
        if (example.is_null())
            return {}; // (nothing known about it: anything goes, the engine decides)
        if (example.is_object())
        {
            if (!value.is_object())
                return std::format("{} must be an object", Where(path));
            for (auto it = value.begin(); it != value.end(); ++it)
            {
                if (!example.contains(it.key()))
                {
                    std::string valid;
                    for (auto field = example.begin(); field != example.end(); ++field)
                        valid += (valid.empty() ? "" : ", ") + field.key();
                    return std::format("unknown field '{}{}' (fields: {})", path.empty() ? "" : path + "/", it.key(), valid);
                }
                if (std::string problem = CheckAgainstExample(example[it.key()], it.value(), path.empty() ? it.key() : path + "/" + it.key());
                    !problem.empty())
                    return problem;
            }
            return {};
        }
        if (IsIdText(example) || (example.is_string() && example.get<std::string>() == "0"))
        {
            // An id field (also one that is 0: unset): a digit string, never a number (it would lose digits).
            if (value.is_string() && (IsIdText(value) || value.get<std::string>() == "0"))
                return {};
            if (value.is_number_integer() || value.is_number_unsigned())
            {
                const uint64_t number = value.get<uint64_t>();
                if (number == 0)
                    return {};
                return std::format("{} is an id: write it as a string (\"{}\") -- a JSON number loses its last digits", Where(path), number);
            }
            if (example.is_string() && !IsIdText(example) && value.is_string())
                return {}; // ("0" was a plain string field after all)
            return std::format("{} must be an id (a string of digits) or \"0\"", Where(path));
        }
        if (example.is_boolean())
            return value.is_boolean() ? std::string() : std::format("{} must be true or false", Where(path));
        if (example.is_number_integer() || example.is_number_unsigned())
        {
            // The scene file writes 5.0 as "5": a whole number there does not say whether the field takes fractions (a light's
            // intensity does, a sample count does not). Any number fits here; a field that only takes whole numbers refuses a
            // fraction when the serializer reads it, and the write is rolled back with that reason.
            if (value.is_number())
                return {};
            // An unset reference reads as 0 (a plain number in the file): it takes an id string (a mesh set where there was none).
            if (example.get<int64_t>() == 0 && IsIdText(value))
                return {};
            return std::format("{} must be a number", Where(path));
        }
        if (example.is_number())
            return value.is_number() ? std::string() : std::format("{} must be a number", Where(path));
        if (example.is_string())
            return value.is_string() ? std::string() : std::format("{} must be a string", Where(path));
        if (IsNumberVector(example))
        {
            if (!value.is_array() || value.size() != example.size() ||
                !std::all_of(value.begin(), value.end(), [](const Json& item) { return item.is_number(); }))
                return std::format("{} must be {} numbers", Where(path), example.size());
            return {};
        }
        if (example.is_array())
        {
            if (!value.is_array())
                return std::format("{} must be a list", Where(path));
            if (!example.empty())
                for (size_t index = 0; index < value.size(); ++index)
                    if (std::string problem = CheckAgainstExample(example[0], value[index], std::format("{}/{}", path, index)); !problem.empty())
                        return problem;
            return {};
        }
        return {};
    }

    std::string ApplyPatch(const Json& current, const Json& operations, Json& result)
    {
        if (!operations.is_array() || operations.empty())
            return "'operations' must be a non-empty list of { op, path, value }";
        for (const Json& operation : operations)
        {
            if (!operation.is_object() || !operation.contains("op") || !operation["op"].is_string() || !operation.contains("path") ||
                !operation["path"].is_string())
                return "each operation needs 'op' and 'path' (strings)";
            const std::string op = operation["op"].get<std::string>();
            if (op != "add" && op != "remove" && op != "replace" && op != "test")
                return std::format("operation '{}' is not allowed (add, remove, replace, test)", op);
            if ((op == "add" || op == "replace" || op == "test") && !operation.contains("value"))
                return std::format("'{}' at '{}' needs a value", op, operation["path"].get<std::string>());
            const std::string path = operation["path"].get<std::string>();
            if (path.empty() || path[0] != '/')
                return std::format("path '{}' must start with '/' (e.g. \"/Translation\")", path);
        }
        try
        {
            result = current.patch(operations);
        }
        catch (const std::exception& error)
        {
            return std::format("the patch does not apply: {}", error.what());
        }
        return {};
    }

    Json Differences(const Json& requested, const Json& stored, const std::string& path)
    {
        Json differences = Json::array();
        auto add = [&](const std::string& where, const Json& wanted, const Json& got)
        {
            differences.push_back(Json{ { "path", where.empty() ? std::string("/") : "/" + where }, { "requested", wanted }, { "stored", got } });
        };
        if (requested.is_object() && stored.is_object())
        {
            for (auto it = requested.begin(); it != requested.end(); ++it)
            {
                const std::string where = path.empty() ? it.key() : path + "/" + it.key();
                if (!stored.contains(it.key()))
                {
                    add(where, it.value(), nullptr);
                    continue;
                }
                for (Json& difference : Differences(it.value(), stored[it.key()], where))
                    differences.push_back(std::move(difference));
            }
            return differences;
        }
        if (requested.is_array() && stored.is_array() && requested.size() == stored.size())
        {
            for (size_t index = 0; index < requested.size(); ++index)
                for (Json& difference : Differences(requested[index], stored[index], std::format("{}/{}", path, index)))
                    differences.push_back(std::move(difference));
            return differences;
        }
        if (requested.is_number() && stored.is_number())
        {
            const double a = requested.get<double>(), b = stored.get<double>();
            if (std::abs(a - b) > 1e-5 * std::max({ 1.0, std::abs(a), std::abs(b) }))
                add(path, requested, stored);
            return differences;
        }
        // ("123" stored for 123 asked, an id string for the same number: the same value)
        if (requested.is_number_integer() && stored.is_string() && stored.get<std::string>() == std::to_string(requested.get<int64_t>()))
            return differences;
        if (requested != stored)
            add(path, requested, stored);
        return differences;
    }

    Json Merge(const Json& base, const Json& overlay)
    {
        if (!base.is_object() || !overlay.is_object())
            return overlay.is_null() ? base : overlay;
        Json merged = base;
        for (auto it = overlay.begin(); it != overlay.end(); ++it)
            merged[it.key()] = merged.contains(it.key()) ? Merge(merged[it.key()], it.value()) : it.value();
        return merged;
    }

    Json SchemaFromExample(const Json& example)
    {
        if (!example.is_object())
            return Json{ { "kind", KindOf(example) }, { "default", example } };
        Json schema = Json::object();
        for (auto it = example.begin(); it != example.end(); ++it)
            schema[it.key()] = it.value().is_object() ? Json{ { "kind", "object" }, { "fields", SchemaFromExample(it.value()) } }
                                                      : Json{ { "kind", KindOf(it.value()) }, { "default", it.value() } };
        return schema;
    }

    Json ScalarToJson(const std::string& text)
    {
        if (text == "true" || text == "false")
            return text == "true";
        if (text == "~" || text == "null")
            return nullptr;
        if (!text.empty())
        {
            char* end = nullptr;
            const long long whole = std::strtoll(text.c_str(), &end, 10);
            if (end == text.c_str() + text.size())
                return text.size() < 16 ? Json(whole) : Json(text); // (ids stay text: see IdText)
            const double number = std::strtod(text.c_str(), &end);
            if (end == text.c_str() + text.size())
                return number;
        }
        return text;
    }

    Json NodeToJson(const YAML::Node& node)
    {
        switch (node.Type())
        {
        case YAML::NodeType::Map:
        {
            Json object = Json::object();
            for (const auto& pair : node)
                object[pair.first.as<std::string>()] = NodeToJson(pair.second);
            return object;
        }
        case YAML::NodeType::Sequence:
        {
            Json array = Json::array();
            for (const auto& item : node)
                array.push_back(NodeToJson(item));
            return array;
        }
        case YAML::NodeType::Scalar:
            return ScalarToJson(node.Scalar());
        default:
            return nullptr;
        }
    }

    YAML::Node JsonToYaml(const Json& value)
    {
        YAML::Node node;
        switch (value.type())
        {
        case Json::value_t::object:
            node = YAML::Node(YAML::NodeType::Map);
            for (auto it = value.begin(); it != value.end(); ++it)
                node[it.key()] = JsonToYaml(it.value());
            return node;
        case Json::value_t::array:
            node = YAML::Node(YAML::NodeType::Sequence);
            for (const Json& item : value)
                node.push_back(JsonToYaml(item));
            return node;
        case Json::value_t::string:
            return YAML::Node(value.get<std::string>());
        case Json::value_t::boolean:
            return YAML::Node(value.get<bool>() ? "true" : "false");
        case Json::value_t::number_integer:
            return YAML::Node(std::to_string(value.get<int64_t>()));
        case Json::value_t::number_unsigned:
            return YAML::Node(std::to_string(value.get<uint64_t>()));
        case Json::value_t::number_float:
            return YAML::Node(std::format("{:.17g}", value.get<double>())); // (doubles: world positions keep every digit)
        default:
            return YAML::Node(YAML::NodeType::Null);
        }
    }
}
