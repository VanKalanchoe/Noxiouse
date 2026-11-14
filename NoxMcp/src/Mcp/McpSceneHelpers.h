#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "McpHost.h"
#include "NoxCore/Scene/Entity.h"

namespace Nox
{
    struct RelationshipComponent;
}

// Shared by the MCP tool files of both hosts: ids, entity summaries, paging.
namespace Nox::Mcp
{
    std::string Lower(std::string_view text);
    std::vector<std::string> Words(std::string_view text); // lower case, split at spaces
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
    // An entity by uuid (a digit string) or by name (the first in a stable order; case ignored); none when neither matches.
    Entity FindByIdOrName(Scene& scene, const std::string& text);

    // The scene shown, or an error answered when there is none.
    Ref<Scene> RequireScene(const HostContext& context, ToolDone& done);
    // Seconds on a steady clock.
    double Now();
}
