#include "McpToolHelpers.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>

#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Project/Project.h"
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

    Ref<Scene> RequireScene(const EditorContext& context, ToolDone& done)
    {
        Ref<Scene> scene = context.ActiveScene ? context.ActiveScene() : nullptr;
        if (!scene)
            done(ToolResult::Error("no scene is open"));
        return scene;
    }


    namespace
    {
        // "StaticMesh" for AssetType::StaticMesh (AssetTypeToString's text without its "AssetType::").
        std::string ShortTypeName(AssetType type)
        {
            std::string name(AssetTypeToString(type));
            if (name.rfind("AssetType::", 0) == 0)
                name.erase(0, 11);
            return name;
        }
    }

    AssetHandle ResolveAsset(const Json& value, AssetType type, std::string& error)
    {
        const std::string typeName = ShortTypeName(type);
        Ref<Project> project = Project::GetActive();
        if (!project || !project->GetEditorAssetManager())
        {
            error = "no project is open";
            return 0;
        }
        auto manager = project->GetEditorAssetManager();
        uint64_t id = 0;
        if (ParseId(value, id))
        {
            const AssetHandle handle(id);
            if (!manager->IsAssetHandleValid(handle))
            {
                error = std::format("no asset with uuid {} (list_assets lists them)", id);
                return 0;
            }
            if (manager->GetAssetType(handle) != type)
            {
                error = std::format("asset {} is a {}, not a {}", id, ShortTypeName(manager->GetAssetType(handle)), typeName);
                return 0;
            }
            return handle;
        }
        if (!value.is_string() || value.get<std::string>().empty())
        {
            error = std::format("name the {} by its uuid (a digit string) or its path under the asset folder", typeName);
            return 0;
        }
        std::string problem;
        const std::filesystem::path path = AssetRelativePath(value.get<std::string>(), problem);
        if (path.empty())
        {
            error = problem;
            return 0;
        }
        const AssetHandle handle = manager->ResolvePath(path, type);
        if (handle == 0)
            error = std::format("no {} at '{}' (list_assets lists them)", typeName, path.generic_string());
        return handle;
    }

    std::string AssetPath(AssetHandle handle)
    {
        Ref<Project> project = Project::GetActive();
        if (!project || !project->GetEditorAssetManager() || !project->GetEditorAssetManager()->IsAssetHandleValid(handle))
            return {};
        return project->GetEditorAssetManager()->GetMetadata(handle).FilePath.generic_string();
    }

    std::filesystem::path AssetRelativePath(const std::string& text, std::string& error)
    {
        std::filesystem::path path = std::filesystem::path(text).lexically_normal();
        if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        {
            error = std::format("'{}' must be a path under the asset folder (e.g. \"Materials/Red.nmat\")", text);
            return {};
        }
        for (const auto& part : path)
            if (part == "..")
            {
                error = std::format("'{}' leaves the asset folder", text);
                return {};
            }
        return path;
    }
}
