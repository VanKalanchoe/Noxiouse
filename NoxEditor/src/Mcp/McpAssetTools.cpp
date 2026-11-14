#include "McpEditorTools.h"
#include "McpMaterial.h"
#include "McpToolHelpers.h"
#include "McpValidation.h"

#include <chrono>
#include <format>
#include <fstream>
#include <set>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/MaterialSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneSerializer.h"

// MCP4: scripts and assets -- build the C# scripts and read their errors as data, materials as JSON, prefabs from entities, files
// brought into the project. (instantiate_prefab is with the scene edits: McpEditTools.)
namespace Nox::Mcp
{
    namespace
    {
        constexpr double MaxWaitSeconds = 50.0; // (under the server's 60 s call time-out)

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        Json DiagnosticJson(const ScriptDiagnostic& diagnostic)
        {
            Json json{ { "severity", diagnostic.Severity }, { "code", diagnostic.Code }, { "message", diagnostic.Message } };
            if (!diagnostic.File.empty())
            {
                json["file"] = diagnostic.File;
                json["line"] = diagnostic.Line;
                json["column"] = diagnostic.Column;
            }
            return json;
        }

        Json BuildJson(const ScriptBuildInfo& info)
        {
            Json errors = Json::array(), warnings = Json::array();
            for (const ScriptDiagnostic& diagnostic : info.Diagnostics)
            {
                Json& list = diagnostic.Severity == "error" ? errors : warnings;
                if (list.size() < 100)
                    list.push_back(DiagnosticJson(diagnostic));
            }
            Json json{ { "status", info.Status }, { "building", info.Building || info.Pending } };
            if (info.Number == 0)
            {
                json["lastBuild"] = nullptr; // (none since the editor started)
                return json;
            }
            json["lastBuild"] = Json{ { "number", info.Number }, { "succeeded", info.Succeeded }, { "reloaded", info.Reloaded },
                                      { "seconds", info.Seconds }, { "errors", errors }, { "warnings", warnings }, { "log", info.Log } };
            return json;
        }

        std::shared_ptr<EditorAssetManager> Assets()
        {
            Ref<Project> project = Project::GetActive();
            return project ? project->GetEditorAssetManager() : nullptr;
        }

        // Texture fields may name a texture by uuid: its path, as the Material Editor stores it (the source file when it has one).
        // Every texture path that is new must name a texture asset.
        std::string ResolveTextures(const MaterialData& before, MaterialData& material)
        {
            auto manager = Assets();
            const auto previous = MaterialTexturePaths(before);
            Json values = MaterialToJson(material);
            bool rewritten = false;
            for (const auto& [field, path] : MaterialTexturePaths(material))
            {
                const bool unchanged = std::any_of(previous.begin(), previous.end(), [&](const auto& p) { return p.first == field && p.second == path; });
                if (unchanged)
                    continue;
                std::string error;
                const AssetHandle texture = ResolveAsset(Json(path), AssetType::Texture2D, error);
                if (texture == 0)
                    return std::format("'{}': {}", field, error);
                const AssetMetadata metadata = manager->GetMetadata(texture);
                const std::string stored = metadata.SourceFilePath.empty() ? metadata.FilePath.generic_string() : metadata.SourceFilePath.generic_string();
                if (stored != path)
                {
                    Json* slot = &values;
                    const size_t slash = field.find('/');
                    slot = slash == std::string::npos ? &values[field] : &values[field.substr(0, slash)][field.substr(slash + 1)];
                    *slot = stored;
                    rewritten = true;
                }
            }
            return rewritten ? MaterialFromJson(values, material) : std::string();
        }

        Json MaterialAnswer(AssetHandle handle, const MaterialData& material)
        {
            return Json{ { "uuid", IdText(static_cast<uint64_t>(handle)) }, { "path", AssetPath(handle) }, { "values", MaterialToJson(material) } };
        }

        // The fields of `after` that differ from `before` (top level), with both values.
        void ChangedFields(const Json& before, const Json& after, Json& changed, Json& previous)
        {
            changed = Json::object();
            previous = Json::object();
            for (auto it = after.begin(); it != after.end(); ++it)
                if (!before.contains(it.key()) || before[it.key()] != it.value())
                {
                    changed[it.key()] = it.value();
                    previous[it.key()] = before.contains(it.key()) ? before[it.key()] : Json();
                }
        }

        // What a .gltf file needs next to it: its buffers' and images' relative files.
        std::vector<std::filesystem::path> GltfCompanions(const std::filesystem::path& file, std::string& error)
        {
            std::vector<std::filesystem::path> files;
            std::ifstream stream(file);
            Json gltf;
            try
            {
                gltf = Json::parse(stream);
            }
            catch (const std::exception& parseError)
            {
                error = std::format("{} is not valid glTF JSON ({})", file.filename().string(), parseError.what());
                return {};
            }
            for (const char* list : { "buffers", "images" })
                for (const Json& item : gltf.value(list, Json::array()))
                {
                    const std::string uri = item.value("uri", std::string());
                    if (uri.empty() || uri.rfind("data:", 0) == 0)
                        continue; // (embedded)
                    // (glTF URIs are percent-encoded: spaces as %20)
                    std::string decoded;
                    for (size_t i = 0; i < uri.size(); ++i)
                    {
                        if (uri[i] == '%' && i + 2 < uri.size())
                        {
                            decoded += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
                            i += 2;
                        }
                        else
                            decoded += uri[i];
                    }
                    const std::filesystem::path relative = std::filesystem::path(decoded).lexically_normal();
                    if (relative.is_absolute() || relative.has_root_name() || (!relative.empty() && *relative.begin() == ".."))
                    {
                        error = std::format("{} refers to '{}' outside its folder: copy the model into the asset folder yourself and import it from there",
                                            file.filename().string(), uri);
                        return {};
                    }
                    files.push_back(relative);
                }
            return files;
        }
    }

    std::vector<Tool> CreateAssetTools(const EditorContext& context)
    {
        std::vector<Tool> tools;

        // ------------------------------------------------------------------------------------------------------- build_scripts
        {
            Tool tool;
            tool.Name = "build_scripts";
            tool.Title = "Build the C# scripts";
            tool.Description = "Builds the project's C# scripts (Scripts > Build and Reload) and, when they compile, reloads them in the editor. "
                               "Answers when the build is done: succeeded, reloaded, and the compiler's errors and warnings as file / line / "
                               "column / code / message. A build already running is waited for. A build longer than 'timeout' answers "
                               "building: true -- get_build_status then.";
            tool.InputSchema = Schema().Number("timeout", "Wait at most this long, seconds (default 45)", false, 1.0, MaxWaitSeconds);
            tool.Access = ConsoleAccess::Execute; // (MSBuild runs the project's build)
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                if (!context.ScriptBuild || !context.RequestScriptBuild)
                {
                    done(ToolResult::Error("script builds are not available"));
                    return;
                }
                const ScriptBuildInfo before = context.ScriptBuild();
                if (!before.HasProject)
                {
                    done(ToolResult::Error("the project has no C# scripts project (Assets/Scripts/*.csproj)"));
                    return;
                }
                // A build already running is the one to wait for; else a new one. (A running build started before a file the caller
                // just saved may miss it: the auto build that file change triggers comes after, and is waited for too.)
                if (!before.Building && !before.Pending)
                    context.RequestScriptBuild();
                const uint64_t startNumber = before.Number;
                const double start = Now();
                const double timeout = arguments.value("timeout", 45.0);
                auto idleFrames = std::make_shared<int>(0);
                context.EveryFrame([context, done, startNumber, start, timeout, idleFrames]()
                {
                    const ScriptBuildInfo info = context.ScriptBuild();
                    if (info.Number > startNumber && !info.Building && !info.Pending)
                    {
                        Json result = BuildJson(info);
                        ToolResult answer = ToolResult::Data(result);
                        answer.IsError = !info.Succeeded;
                        done(std::move(answer));
                        return true;
                    }
                    // Nothing started and nothing finished: the builder refused (its status says why).
                    if (!info.Building && !info.Pending && info.Number == startNumber && ++*idleFrames > 3)
                    {
                        done(ToolResult::Error(std::format("the build did not start: {}", info.Status)));
                        return true;
                    }
                    if (Now() - start > timeout)
                    {
                        Json result = BuildJson(info);
                        result["note"] = "still building: call get_build_status in a while";
                        done(ToolResult::Data(result));
                        return true;
                    }
                    return false;
                });
            };
            tools.push_back(std::move(tool));
        }

        // --------------------------------------------------------------------------------------------------- get_build_status
        {
            Tool tool;
            tool.Name = "get_build_status";
            tool.Title = "Script build status";
            tool.Description = "Whether the C# scripts are building, and the last build's result: succeeded, reloaded, errors and warnings "
                               "(file / line / column / code / message), the full output's file.";
            tool.InputSchema = Schema();
            tool.Handler = [context](const Json&, ToolDone done)
            {
                if (!context.ScriptBuild)
                {
                    done(ToolResult::Error("script builds are not available"));
                    return;
                }
                done(ToolResult::Data(BuildJson(context.ScriptBuild())));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------- get_material
        {
            Tool tool;
            tool.Name = "get_material";
            tool.Title = "Read a material";
            tool.Description = "A material asset's values (BaseColorFactor, MetallicFactor, RoughnessFactor, EmissiveFactor, the texture "
                               "paths, Mode, the clear coat / sheen / iridescence ... lobes) by its uuid or path. Entities use materials "
                               "through MaterialComponent (MaterialAssets: one per submesh) or their mesh's own.";
            tool.InputSchema = Schema().String("material", "The material's uuid or path (e.g. \"Materials/Red.nmat\")", true);
            tool.Handler = [](const Json& arguments, ToolDone done)
            {
                std::string error;
                const AssetHandle handle = ResolveAsset(arguments["material"], AssetType::Material, error);
                if (handle == 0)
                {
                    done(ToolResult::Error(error));
                    return;
                }
                Ref<Material> material = AssetManager::GetAsset<Material>(handle);
                if (!material)
                {
                    done(ToolResult::Error(std::format("the material {} could not be loaded (read_log)", AssetPath(handle))));
                    return;
                }
                done(ToolResult::Data(MaterialAnswer(handle, material->GetData())));
            };
            tools.push_back(std::move(tool));
        }

        // ---------------------------------------------------------------------------------------------------- create_material
        {
            Tool tool;
            tool.Name = "create_material";
            tool.Title = "Create a material";
            tool.Description = "Creates a material asset (.nmat) in 'folder' with 'values' over the defaults (white, rough, not metallic), "
                               "e.g. { \"BaseColorFactor\": [1, 0, 0, 1], \"RoughnessFactor\": 0.3 }. Answers its uuid: put it on an entity "
                               "with patch_component (MaterialComponent, MaterialAssets [uuid]).";
            tool.InputSchema = Schema()
                                   .String("name", "Its name (also the file's: <name>.nmat)", true)
                                   .String("folder", "The folder under the asset folder (default \"Materials\")")
                                   .Build();
            tool.InputSchema["properties"]["values"] = Json{ { "type", "object" }, { "description", "Fields over the defaults (get_material shows them all)" } };
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                auto manager = Assets();
                if (!manager)
                {
                    done(ToolResult::Error("no project is open"));
                    return;
                }
                const std::string name = arguments["name"].get<std::string>();
                if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string::npos)
                {
                    done(ToolResult::Error("'name' must be a file name (no \\ / : * ? \" < > |)"));
                    return;
                }
                std::string error;
                const std::filesystem::path folder = AssetRelativePath(arguments.value("folder", std::string("Materials")), error);
                if (folder.empty())
                {
                    done(ToolResult::Error(error));
                    return;
                }
                const std::filesystem::path relative = folder / (name + ".nmat");
                const std::filesystem::path absolute = Project::GetActiveAssetDirectory() / relative;
                std::error_code exists;
                if (std::filesystem::exists(absolute, exists))
                {
                    done(ToolResult::Error(std::format("{} exists already (set_material changes it)", relative.generic_string())));
                    return;
                }
                MaterialData material;
                material.Name = name;
                if (arguments.contains("values"))
                {
                    if (std::string problem = CheckAgainstExample(MaterialToJson(MaterialData{}), arguments["values"]); !problem.empty())
                    {
                        done(ToolResult::Error(problem));
                        return;
                    }
                    if (std::string problem = MaterialFromJson(arguments["values"], material); !problem.empty())
                    {
                        done(ToolResult::Error(problem));
                        return;
                    }
                    if (std::string problem = ResolveTextures(MaterialData{}, material); !problem.empty())
                    {
                        done(ToolResult::Error(problem));
                        return;
                    }
                }
                if (!MaterialSerializer::Serialize(absolute, material))
                {
                    done(ToolResult::Error(std::format("{} could not be written", absolute.generic_string())));
                    return;
                }
                const AssetHandle handle = manager->RegisterExistingFile(relative, AssetType::Material);
                if (handle == 0)
                {
                    done(ToolResult::Error(std::format("{} was written but could not be registered (read_log)", relative.generic_string())));
                    return;
                }
                if (context.AssetsChanged)
                    context.AssetsChanged();
                NOX_CORE_INFO("MCP: created the material {}", relative.generic_string());
                done(ToolResult::Data(MaterialAnswer(handle, material)));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------- set_material
        {
            Tool tool;
            tool.Name = "set_material";
            tool.Title = "Change a material";
            tool.Description = "Changes a material asset's values: 'values' (the fields to set) or 'operations' (JSON Patch on get_material's "
                               "values). Checked first (field names and kinds; a texture by uuid or path must be a texture asset), saved to "
                               "its file, and drawn with the new values at once -- every entity using it changes. Not an undo step (as in the "
                               "Material Editor): the answer's 'previous' holds the old values to set back.";
            tool.InputSchema = Schema().String("material", "The material's uuid or path", true).Build();
            tool.InputSchema["properties"]["values"] = Json{ { "type", "object" }, { "description", "The fields to set" } };
            tool.InputSchema["properties"]["operations"] = Json{ { "type", "array" }, { "description", "JSON Patch: [{ op, path, value }]" } };
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                std::string error;
                const AssetHandle handle = ResolveAsset(arguments["material"], AssetType::Material, error);
                if (handle == 0)
                {
                    done(ToolResult::Error(error));
                    return;
                }
                if (arguments.contains("values") == arguments.contains("operations"))
                {
                    done(ToolResult::Error("give 'values' or 'operations' (one of them)"));
                    return;
                }
                Ref<Material> asset = AssetManager::GetAsset<Material>(handle);
                if (!asset)
                {
                    done(ToolResult::Error(std::format("the material {} could not be loaded (read_log)", AssetPath(handle))));
                    return;
                }
                const MaterialData before = asset->GetData();
                const Json current = MaterialToJson(before);
                Json values = arguments.value("values", Json::object());
                if (arguments.contains("operations"))
                {
                    Json patched;
                    if (std::string problem = ApplyPatch(current, arguments["operations"], patched); !problem.empty())
                    {
                        done(ToolResult::Error(problem));
                        return;
                    }
                    values = patched;
                }
                if (std::string problem = CheckAgainstExample(current, values); !problem.empty())
                {
                    done(ToolResult::Error(problem));
                    return;
                }
                MaterialData material = before;
                if (std::string problem = MaterialFromJson(values, material); !problem.empty())
                {
                    done(ToolResult::Error(problem));
                    return;
                }
                if (std::string problem = ResolveTextures(before, material); !problem.empty())
                {
                    done(ToolResult::Error(problem));
                    return;
                }
                const std::filesystem::path file = Project::GetActiveAssetDirectory() / AssetPath(handle);
                if (!MaterialSerializer::Serialize(file, material))
                {
                    done(ToolResult::Error(std::format("{} could not be written; nothing changed", file.generic_string())));
                    return;
                }
                asset->GetData() = material;
                Assets()->NoteSourceWritten(handle); // (the file watcher's event for this write: no reimport)
                if (context.MaterialChanged)
                    context.MaterialChanged(handle);
                Json changed, previous;
                ChangedFields(current, MaterialToJson(material), changed, previous);
                NOX_CORE_INFO("MCP: changed the material {} ({} fields)", AssetPath(handle), changed.size());
                done(ToolResult::Data(Json{ { "uuid", IdText(static_cast<uint64_t>(handle)) }, { "path", AssetPath(handle) },
                                            { "changed", changed }, { "previous", previous } }));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------ create_prefab
        {
            Tool tool;
            tool.Name = "create_prefab";
            tool.Title = "Make a prefab from entities";
            tool.Description = "Saves entities (with their children) as a prefab asset (.nprefab) in 'folder', as dragging them into the Content "
                               "Browser does; the first entity is the pivot. The entities stay as they are; instantiate_prefab places copies. "
                               "Edit mode.";
            tool.InputSchema = Schema()
                                   .Array("entities", "The entities' uuids (the first is the pivot)", "string", true)
                                   .String("name", "The prefab's name (default: the first entity's)")
                                   .String("folder", "The folder under the asset folder (default \"Prefabs\")");
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                ToolDone answer = done;
                Ref<Scene> scene = RequireScene(context, answer);
                if (!scene)
                    return;
                if (context.Mode && context.Mode() != "edit")
                {
                    done(ToolResult::Error("prefabs are made in Edit mode (stop first)"));
                    return;
                }
                auto manager = Assets();
                std::vector<Entity> entities;
                for (const Json& id : arguments["entities"])
                {
                    uint64_t uuid = 0;
                    Entity entity = ParseId(id, uuid) ? scene->GetEntityByUUID(UUID(uuid)) : Entity();
                    if (!entity)
                    {
                        done(ToolResult::Error(std::format("no entity with uuid {}", id.dump())));
                        return;
                    }
                    entities.push_back(entity);
                }
                if (entities.empty() || !manager)
                {
                    done(ToolResult::Error(entities.empty() ? "'entities' is empty" : "no project is open"));
                    return;
                }
                std::string name = arguments.value("name", entities.front().GetName());
                for (char& character : name)
                    if (std::string_view("\\/:*?\"<>|").find(character) != std::string_view::npos)
                        character = '_';
                if (name.empty())
                    name = "New Prefab";
                std::string error;
                const std::filesystem::path folder = AssetRelativePath(arguments.value("folder", std::string("Prefabs")), error);
                if (folder.empty())
                {
                    done(ToolResult::Error(error));
                    return;
                }
                const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
                std::filesystem::path relative = folder / (name + ".nprefab");
                std::error_code exists;
                for (int suffix = 2; std::filesystem::exists(assetDirectory / relative, exists); ++suffix)
                    relative = folder / std::format("{} {}.nprefab", name, suffix);
                std::filesystem::create_directories((assetDirectory / relative).parent_path(), exists);
                if (!SceneSerializer::SerializePrefab(*scene, entities, relative.stem().string(), assetDirectory / relative))
                {
                    done(ToolResult::Error(std::format("{} could not be written (read_log)", relative.generic_string())));
                    return;
                }
                const AssetHandle handle = manager->RegisterExistingFile(relative, AssetType::Prefab);
                if (context.AssetsChanged)
                    context.AssetsChanged();
                NOX_CORE_INFO("MCP: created the prefab {}", relative.generic_string());
                done(ToolResult::Data(Json{ { "uuid", IdText(static_cast<uint64_t>(handle)) }, { "path", relative.generic_string() },
                                            { "entities", entities.size() } }));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------- import_asset
        {
            Tool tool;
            tool.Name = "import_asset";
            tool.Title = "Bring a file into the project";
            tool.Description = "Imports a file from this computer into the project's asset folder and registers it: a glTF model (.gltf with "
                               "its buffers and images, or .glb) is copied and cooked into meshes, materials, a skeleton and animations (big "
                               "models finish cooking in the background); a texture (.png, .jpg, .ktx2), a sound (.wav, .ogg, .flac, .mp3) or a "
                               "font (.ttf, .otf) is copied as it is. A file already under the asset folder is registered where it is. Answers "
                               "the assets it created.";
            tool.InputSchema = Schema()
                                   .String("source", "The file: an absolute path on this computer, or a path under the asset folder", true)
                                   .String("folder", "Where it goes under the asset folder (default \"Imported\"; a model gets a folder of its own in it)")
                                   .Number("import_scale", "Models: a uniform scale (a model made in centimetres but saved as metres: 0.01)", false, 1e-6, 1e6)
                                   .Boolean("animations", "Models: also import its animation clips (default true)");
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                auto manager = Assets();
                if (!manager)
                {
                    done(ToolResult::Error("no project is open"));
                    return;
                }
                const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
                std::filesystem::path source(arguments["source"].get<std::string>());
                std::error_code fileError;
                if (!source.is_absolute())
                    source = assetDirectory / source;
                source = std::filesystem::weakly_canonical(source, fileError);
                if (fileError || !std::filesystem::is_regular_file(source, fileError))
                {
                    done(ToolResult::Error(std::format("no file at '{}'", arguments["source"].get<std::string>())));
                    return;
                }
                std::string extension = Lower(source.extension().string());
                const AssetType type = EditorAssetManager::GetAssetTypeFromExtension(extension);
                if (type != AssetType::MeshSource && type != AssetType::Texture2D && type != AssetType::AudioClip && type != AssetType::Font)
                {
                    done(ToolResult::Error(std::format("'{}' files are not imported this way (models .gltf / .glb, textures .png / .jpg / .ktx2, "
                                                       "sounds .wav / .ogg / .flac / .mp3, fonts .ttf / .otf)", extension)));
                    return;
                }
                std::string error;
                const std::filesystem::path folder = AssetRelativePath(arguments.value("folder", std::string("Imported")), error);
                if (folder.empty())
                {
                    done(ToolResult::Error(error));
                    return;
                }

                // Inside the asset folder already: imported where it is.
                const std::filesystem::path root = std::filesystem::weakly_canonical(assetDirectory, fileError);
                std::filesystem::path relativeSource = source.lexically_relative(root);
                const bool inside = !relativeSource.empty() && *relativeSource.begin() != ".." && !relativeSource.is_absolute();

                std::set<AssetHandle> before;
                for (const auto& [handle, metadata] : manager->GetAssetRegistry())
                    before.insert(handle);

                if (!inside)
                {
                    // A model gets a folder of its own (its buffers and images keep their relative places in it).
                    const std::filesystem::path targetFolder = type == AssetType::MeshSource ? folder / source.stem() : folder;
                    std::vector<std::filesystem::path> files{ source.filename() };
                    if (extension == ".gltf")
                    {
                        std::vector<std::filesystem::path> companions = GltfCompanions(source, error);
                        if (!error.empty())
                        {
                            done(ToolResult::Error(error));
                            return;
                        }
                        files.insert(files.end(), companions.begin(), companions.end());
                    }
                    if (std::filesystem::exists(assetDirectory / targetFolder / source.filename(), fileError))
                    {
                        done(ToolResult::Error(std::format("{} exists already: choose another 'folder'",
                                                           (targetFolder / source.filename()).generic_string())));
                        return;
                    }
                    for (const std::filesystem::path& file : files)
                    {
                        const std::filesystem::path from = source.parent_path() / file, to = assetDirectory / targetFolder / file;
                        std::filesystem::create_directories(to.parent_path(), fileError);
                        std::filesystem::copy_file(from, to, std::filesystem::copy_options::skip_existing, fileError);
                        if (fileError)
                        {
                            done(ToolResult::Error(std::format("could not copy {} ({})", from.generic_string(), fileError.message())));
                            return;
                        }
                    }
                    relativeSource = targetFolder / source.filename();
                }
                relativeSource = relativeSource.lexically_normal();

                const bool model = type == AssetType::MeshSource;
                if (model)
                {
                    MeshImportSettings settings;
                    settings.ImportScale = static_cast<float>(arguments.value("import_scale", 1.0));
                    settings.ImportAnimations = arguments.value("animations", true);
                    const std::filesystem::path destination = relativeSource.parent_path() / "Meshes" / (relativeSource.stem().string() + ".nmesh");
                    if (!manager->ImportModel(relativeSource, destination, settings))
                    {
                        done(ToolResult::Error(std::format("{} could not be imported (read_log tells why)", relativeSource.generic_string())));
                        return;
                    }
                }
                else if (!manager->FindHandleByPath(relativeSource, type))
                    manager->RegisterExistingFile(relativeSource, type);

                // The answer: what the registry gained. A model's separate meshes cook in the background (PublishSplitImports registers
                // them when that finished): answered then, or after the time-out with what there is so far.
                const double start = Now();
                context.EveryFrame([context, done, manager, before, relativeSource, type, model, start]()
                {
                    const bool cooking = model && manager->IsCookingImport(relativeSource);
                    const bool timedOut = Now() - start > 45.0;
                    if (cooking && !timedOut)
                        return false;
                    Json created = Json::array();
                    for (const auto& [handle, metadata] : manager->GetAssetRegistry())
                        if (!before.contains(handle))
                            created.push_back(Json{ { "uuid", IdText(static_cast<uint64_t>(handle)) }, { "type", std::string(AssetTypeToString(metadata.Type)) },
                                                    { "path", metadata.FilePath.generic_string() } });
                    if (created.empty())
                        if (AssetHandle existing = manager->FindHandleByPath(relativeSource, type))
                            created.push_back(Json{ { "uuid", IdText(static_cast<uint64_t>(existing)) }, { "type", std::string(AssetTypeToString(type)) },
                                                    { "path", relativeSource.generic_string() }, { "already", true } });
                    if (context.AssetsChanged)
                        context.AssetsChanged();
                    NOX_CORE_INFO("MCP: imported {} ({} assets{})", relativeSource.generic_string(), created.size(), cooking ? ", still cooking" : "");
                    Json result{ { "source", relativeSource.generic_string() }, { "assets", created } };
                    if (cooking)
                        result["note"] = "a big model: its meshes are still cooking in the background -- list_assets again in a while";
                    else if (model)
                        result["note"] = "its textures are registered (and cooked) the first time the mesh loads: when it is placed in the scene";
                    done(ToolResult::Data(result));
                    return true;
                });
            };
            tools.push_back(std::move(tool));
        }

        return tools;
    }
}
