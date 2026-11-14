#include "McpEditorTools.h"
#include "McpToolHelpers.h"

#include <algorithm>
#include <format>

#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Core/LogBuffer.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Scene.h"

namespace Nox::Mcp
{
    std::vector<Tool> CreateEditorTools(const EditorContext& context)
    {
        std::vector<Tool> tools;

        // ---------------------------------------------------------------------------------------------------- get_editor_status
        {
            Tool tool;
            tool.Name = "get_editor_status";
            tool.Title = "Editor status";
            tool.Description = "The open project and scene, the editor's mode (edit / play / simulate), the selection, frame rate, the "
                               "multiplayer session (and the games it started with MCP servers: runtime_call), the newest log line's sequence (read_log reads from there). Call it first.";
            tool.InputSchema = Schema();
            tool.Handler = [context](const Json&, ToolDone done)
            {
                Ref<Scene> scene = context.ActiveScene ? context.ActiveScene() : nullptr;
                size_t entities = 0;
                if (scene)
                    for ([[maybe_unused]] auto handle : scene->GetAllEntitiesWith<IDComponent>())
                        ++entities;
                Ref<Project> project = Project::GetActive();
                Json status{ { "project", project ? project->GetConfig().Name : std::string() },
                             { "projectDirectory", project ? Project::GetActiveProjectDirectory().generic_string() : std::string() },
                             { "scene", context.ScenePath ? context.ScenePath() : std::string() },
                             { "mode", context.Mode ? context.Mode() : std::string("edit") },
                             { "entities", entities },
                             { "selected", context.Selection ? context.Selection().size() : 0 },
                             { "framesPerSecond", context.FramesPerSecond ? context.FramesPerSecond() : 0.0f },
                             { "multiplayer", context.NetStatus ? context.NetStatus() : std::string() },
                             { "newestLogSequence", LogBuffer::Get().GetLatestSequence() },
                             { "grantedAccess", ToString(context.GrantedAccess ? context.GrantedAccess() : ConsoleAccess::Read) } };
                // MCP5: the games multiplayer Play started with MCP servers (runtime_call reaches them).
                if (context.Runtimes)
                {
                    Json runtimes = Json::array();
                    for (const RuntimeEndpoint& runtime : context.Runtimes())
                        runtimes.push_back(Json{ { "name", runtime.Name }, { "player", runtime.Player }, { "mcpPort", runtime.Port },
                                                 { "running", runtime.Running }, { "mcp", runtime.McpState } });
                    if (!runtimes.empty())
                        status["runtimes"] = runtimes;
                }
                done(ToolResult::Data(status));
            };
            tools.push_back(std::move(tool));
        }

        // ---------------------------------------------------------------------------------------------------------- get_selection
        {
            Tool tool;
            tool.Name = "get_selection";
            tool.Title = "Selected entities";
            tool.Description = "The entities selected in the editor (the user's current focus), with their component names.";
            tool.InputSchema = Schema();
            tool.Handler = [context](const Json&, ToolDone done)
            {
                Ref<Scene> scene = RequireScene(context, done);
                if (!scene)
                    return;
                Json selected = Json::array();
                if (context.Selection)
                    for (Entity entity : context.Selection())
                        if (entity)
                            selected.push_back(EntitySummary(*scene, entity));
                done(ToolResult::Data(Json{ { "entities", selected } }));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------------ list_assets
        {
            Tool tool;
            tool.Name = "list_assets";
            tool.Title = "List assets";
            tool.Description = "The project's assets (handle, type, path under the asset folder), sorted by path, filtered by 'type' (e.g. "
                               "Scene, Prefab, Mesh, Texture2D, Material, AnimationGraph, AudioClip) and / or part of the path; paged.";
            tool.InputSchema = Schema()
                                   .String("type", "The asset type, e.g. Texture2D, StaticMesh, Mesh, Material, AnimationSequence, AnimationGraph, AudioGraph, Prefab, Scene (case ignored)")
                                   .String("path", "Part of the path (case ignored)")
                                   .Integer("offset", "Skip this many (default 0)", false, 0)
                                   .Integer("limit", "At most this many (default 100)", false, 1, 1000);
            tool.Handler = [](const Json& arguments, ToolDone done)
            {
                Ref<Project> project = Project::GetActive();
                std::shared_ptr<EditorAssetManager> assets = project ? project->GetEditorAssetManager() : nullptr;
                if (!assets)
                {
                    done(ToolResult::Error("no project is open"));
                    return;
                }
                // ("AnimationSequence" or "AssetType::AnimationSequence", case ignored)
                std::string type = Lower(arguments.value("type", std::string()));
                if (type.rfind("assettype::", 0) == 0)
                    type.erase(0, 11);
                const std::string path = Lower(arguments.value("path", std::string()));
                std::vector<Json> items;
                for (const auto& [handle, metadata] : assets->GetAssetRegistry())
                {
                    const std::string typeName = ShortTypeName(metadata.Type);
                    const std::string file = metadata.FilePath.generic_string();
                    if (!type.empty() && Lower(typeName) != type)
                        continue;
                    if (!path.empty() && Lower(file).find(path) == std::string::npos)
                        continue;
                    items.push_back(Json{ { "handle", IdText(static_cast<uint64_t>(handle)) }, { "type", typeName }, { "path", file } });
                }
                std::sort(items.begin(), items.end(), [](const Json& a, const Json& b) { return a["path"].get<std::string>() < b["path"].get<std::string>(); });
                const size_t offset = arguments.value("offset", 0), limit = arguments.value("limit", 100);
                done(ToolResult::Data(Json{ { "assets", Page(items, offset, limit) }, { "total", items.size() }, { "offset", offset } }));
            };
            tools.push_back(std::move(tool));
        }

        return tools;
    }
}
