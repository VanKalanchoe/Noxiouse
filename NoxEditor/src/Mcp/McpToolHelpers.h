#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "McpEditorTools.h"
#include "McpValidation.h"

namespace Nox
{
    struct RelationshipComponent;
}

// Shared by the MCP tool files (McpEditorTools, McpEditTools): ids, the scene serializer's YAML as JSON and back, entity summaries.
namespace Nox::Mcp
{
    std::string Lower(std::string_view text);
    const RelationshipComponent* RelationshipOf(Entity entity);

    // Ids as strings: 64-bit UUIDs and asset handles do not fit a JSON number exactly (JavaScript clients round them).
    inline std::string IdText(uint64_t id) { return std::to_string(id); }
    bool ParseId(const Json& value, uint64_t& id);

    // The components an entity has, by the scene file's names ("TransformComponent", "ScriptComponent" ...).
    std::vector<std::string> ComponentNames(Scene& scene, Entity entity);
    Json EntitySummary(Scene& scene, Entity entity);
    std::string PathOf(Scene& scene, Entity entity); // "Root / Parent / Name"
    void SortEntities(std::vector<Entity>& entities); // by name, then id: a stable order to page through
    Json Page(const std::vector<Json>& items, size_t offset, size_t limit);

    // The scene the editor shows, or an error answered when there is none.
    Ref<Scene> RequireScene(const EditorContext& context, ToolDone& done);
}
