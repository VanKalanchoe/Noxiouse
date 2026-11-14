#include "McpSceneHelpers.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>

#include <yaml-cpp/yaml.h>

#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneSerializer.h"

namespace Nox::Mcp
{
    std::string Lower(std::string_view text)
    {
        std::string lower(text);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower;
    }

    const RelationshipComponent* RelationshipOf(Entity entity)
    {
        return entity.HasComponent<RelationshipComponent>() ? &entity.GetComponent<RelationshipComponent>() : nullptr;
    }

    bool ParseId(const Json& value, uint64_t& id)
    {
        if (!value.is_string())
            return false;
        const std::string text = value.get<std::string>();
        char* end = nullptr;
        id = std::strtoull(text.c_str(), &end, 10);
        return !text.empty() && end == text.c_str() + text.size();
    }

    // The components an entity has, by the scene file's names ("TransformComponent", "ScriptComponent" ...).
    std::vector<std::string> ComponentNames(Scene& scene, Entity entity)
    {
        std::vector<std::string> names;
        const YAML::Node node = SceneSerializer::EntityToNode(scene, entity);
        if (node.IsMap())
            for (const auto& pair : node)
            {
                const std::string key = pair.first.as<std::string>();
                if (key != "Entity")
                    names.push_back(key);
            }
        return names;
    }

    Json EntitySummary(Scene& scene, Entity entity)
    {
        const auto* relationship = RelationshipOf(entity);
        return Json{ { "uuid", IdText(static_cast<uint64_t>(entity.GetUUID())) },
                     { "name", entity.GetName() },
                     { "children", relationship ? relationship->Children.size() : 0 },
                     { "components", ComponentNames(scene, entity) } };
    }

    // "Root / Parent / Name": where it is in the hierarchy.
    std::string PathOf(Scene& scene, Entity entity)
    {
        std::string path = entity.GetName();
        UUID parent = entity.HasComponent<RelationshipComponent>() ? entity.GetComponent<RelationshipComponent>().Parent : UUID(0);
        for (int depth = 0; parent != 0 && depth < 64; ++depth)
        {
            Entity up = scene.GetEntityByUUID(parent);
            if (!up)
                break;
            path = up.GetName() + " / " + path;
            parent = up.HasComponent<RelationshipComponent>() ? up.GetComponent<RelationshipComponent>().Parent : UUID(0);
        }
        return path;
    }

    // Sorted by name, then id: a stable order to page through.
    void SortEntities(std::vector<Entity>& entities)
    {
        std::sort(entities.begin(), entities.end(), [](Entity a, Entity b)
        {
            const std::string& nameA = a.GetName();
            const std::string& nameB = b.GetName();
            if (nameA != nameB)
                return nameA < nameB;
            return static_cast<uint64_t>(a.GetUUID()) < static_cast<uint64_t>(b.GetUUID());
        });
    }

    Json Page(const std::vector<Json>& items, size_t offset, size_t limit)
    {
        Json page = Json::array();
        for (size_t index = offset; index < items.size() && index < offset + limit; ++index)
            page.push_back(items[index]);
        return page;
    }

    Ref<Scene> RequireScene(const HostContext& context, ToolDone& done)
    {
        Ref<Scene> scene = context.ActiveScene ? context.ActiveScene() : nullptr;
        if (!scene)
            done(ToolResult::Error("no scene is open"));
        return scene;
    }

    std::vector<std::string> Words(std::string_view text)
    {
        std::vector<std::string> words;
        std::string word;
        for (char c : Lower(text) + " ")
        {
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                if (!word.empty())
                    words.push_back(word);
                word.clear();
            }
            else
                word += c;
        }
        return words;
    }

    Entity FindByIdOrName(Scene& scene, const std::string& text)
    {
        uint64_t id = 0;
        if (ParseId(Json(text), id))
            if (Entity entity = scene.GetEntityByUUID(UUID(id)))
                return entity;
        const std::string wanted = Lower(text);
        std::vector<Entity> matches;
        for (auto handle : scene.GetAllEntitiesWith<TagComponent>())
        {
            Entity entity{ handle, &scene };
            if (Lower(entity.GetComponent<TagComponent>().Tag) == wanted)
                matches.push_back(entity);
        }
        if (matches.empty())
            return {};
        SortEntities(matches);
        return matches.front();
    }

    double Now()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}
