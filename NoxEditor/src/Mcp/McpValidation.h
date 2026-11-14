#pragma once
#include <string>

#include <yaml-cpp/yaml.h>

#include "McpProtocol.h"

// Component edits over MCP, the parts that are plain JSON (testable without the editor: NoxToolsTest). A component's "example" is
// its values as the scene file stores them -- its defaults merged with its current values -- and stands for its schema: field names,
// and each field's kind (number, whole number, bool, string, id, vector of N numbers, list, object).
namespace Nox::Mcp
{
    // An id as the tools write it: a string of 16 or more digits (a UUID or an asset handle).
    bool IsIdText(const Json& value);

    // The first way `value` does not fit `example` ("" when it fits): unknown fields (with the valid ones listed), a different kind,
    // a fraction for a whole number, a vector of another length, an id that is not a digit string.
    std::string CheckAgainstExample(const Json& example, const Json& value, const std::string& path = {});

    // Applies JSON-Patch operations (RFC 6902: add, remove, replace, test; paths like "/Translation" or "/Color/0") to `current`
    // into `result`; "" or why it could not.
    std::string ApplyPatch(const Json& current, const Json& operations, Json& result);

    // Where what was stored differs from what was asked (numbers within a relative 1e-5): [{ path, requested, stored }] -- the engine
    // clamped a value, or did not know an enum name.
    Json Differences(const Json& requested, const Json& stored, const std::string& path = {});

    // `base` with `overlay`'s fields on top, recursively for objects (a component's defaults under its current values).
    Json Merge(const Json& base, const Json& overlay);

    // The JSON kind of a field, for schemas and messages: "number", "integer", "bool", "string", "id", "vector3", "list", "object".
    std::string KindOf(const Json& value);
    // A component's schema from its example: { field: { kind, default } } (objects nested).
    Json SchemaFromExample(const Json& example);

    // The scene serializer's YAML as JSON and back. A scalar becomes a boolean, a number (ids -- 16 digits or more -- stay text) or the
    // text; JSON numbers go back with every digit (17 significant: double world positions).
    Json ScalarToJson(const std::string& text);
    Json NodeToJson(const YAML::Node& node);
    YAML::Node JsonToYaml(const Json& value);
}
