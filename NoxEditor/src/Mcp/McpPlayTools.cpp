#include "McpEditorTools.h"
#include "McpToolHelpers.h"
#include "McpClient.h"
#include "McpProjectFiles.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <thread>
#include <memory>

#include "NoxCore/Core/LogBuffer.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/EntityBounds.h"
#include "NoxCore/Scene/Scene.h"

// MCP3: play and verify -- start and stop Play / Simulate, look with the editor camera (waiting, input and pause are the shared game
// tools: McpGameTools, NoxMcp). Tools that take time answer later through `done`, polled once a frame (HostContext::EveryFrame).
namespace Nox::Mcp
{
    namespace
    {
        Json Vector(const glm::dvec3& value)
        {
            return Json::array({ value.x, value.y, value.z });
        }

        bool ReadVector(const Json& arguments, const char* name, glm::dvec3& out, std::string& error)
        {
            const Json& value = arguments[name];
            if (!value.is_array() || value.size() != 3 || !std::all_of(value.begin(), value.end(), [](const Json& item) { return item.is_number(); }))
            {
                error = std::format("'{}' must be 3 numbers [x, y, z] (world units)", name);
                return false;
            }
            out = { value[0].get<double>(), value[1].get<double>(), value[2].get<double>() };
            return true;
        }

        Json CameraJson(const ViewportCamera& camera)
        {
            return Json{ { "position", Vector(camera.Position) }, { "focalPoint", Vector(camera.FocalPoint) }, { "distance", camera.Distance },
                         { "pitchDegrees", glm::degrees(camera.Pitch) }, { "yawDegrees", glm::degrees(camera.Yaw) },
                         { "forward", Json::array({ camera.Forward.x, camera.Forward.y, camera.Forward.z }) },
                         { "orthographic", camera.Orthographic } };
        }

        std::string Mode(const EditorContext& context)
        {
            return context.Mode ? context.Mode() : std::string("edit");
        }

        Json RuntimesJson(const EditorContext& context)
        {
            Json list = Json::array();
            if (context.Runtimes)
                for (const RuntimeEndpoint& runtime : context.Runtimes())
                    list.push_back(Json{ { "name", runtime.Name }, { "player", runtime.Player }, { "server", runtime.Player == 0 },
                                         { "mcpPort", runtime.Port }, { "running", runtime.Running } });
            return list;
        }
    }

    std::vector<Tool> CreatePlayTools(const EditorContext& context)
    {
        std::vector<Tool> tools;

        // ---------------------------------------------------------------------------------------------------------------- play
        {
            Tool tool;
            tool.Name = "play";
            tool.Title = "Play";
            tool.Description = "Starts Play (the game runs its scripts in the viewport), as the Play button does. net_mode: standalone (default), "
                               "listen_server (the editor is the server and plays; the other players are NoxRuntime windows) or client (a "
                               "dedicated server is started and the editor joins it). Multiplayer saves the scene first (the other players load "
                               "it from disk). Answers once the game runs; its log starts at 'logFrom' (read_log since). stop ends it.";
            tool.InputSchema = Schema()
                                   .Enum("net_mode", "standalone (default), listen_server or client", { "standalone", "listen_server", "client" })
                                   .Integer("players", "Multiplayer: how many players, the editor's included (default 2)", false, 1, 8)
                                   .Enum("network_emulation", "Multiplayer: lag and loss on every connection (default off)", { "off", "average", "bad" })
                                   .Boolean("debug_log", "Multiplayer: the network debug trace in every player's log (default false)")
                                   .Boolean("player_mcp", "Multiplayer: start the other players (and the dedicated server) with MCP servers of "
                                                          "their own, reached with runtime_call (default: the Play settings' choice)");
            tool.Access = ConsoleAccess::Execute; // (runs the project's scripts; multiplayer starts processes)
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                if (!context.Play)
                {
                    done(ToolResult::Error("Play is not available"));
                    return;
                }
                if (Mode(context) == "play")
                {
                    done(ToolResult::Error("already in Play (stop first)"));
                    return;
                }
                PlayOptions options;
                options.NetMode = arguments.value("net_mode", std::string("standalone"));
                options.Players = arguments.value("players", 2);
                options.Emulation = arguments.value("network_emulation", std::string("off"));
                options.DebugLog = arguments.value("debug_log", false);
                if (arguments.contains("player_mcp"))
                    options.PlayerMcp = arguments["player_mcp"].get<bool>();
                const uint64_t logFrom = LogBuffer::Get().GetLatestSequence();
                std::string error;
                if (!context.Play(options, error))
                {
                    done(ToolResult::Error(error.empty() ? std::string("Play did not start (read_log tells why)") : error));
                    return;
                }
                // Answered once the game runs (a client: once its server let it in).
                const double start = Now();
                context.EveryFrame([context, done, logFrom, start, options]()
                {
                    auto answer = [&](bool running, const std::string& note)
                    {
                        Json result{ { "mode", Mode(context) }, { "running", running }, { "netMode", options.NetMode }, { "logFrom", logFrom } };
                        if (context.NetStatus && !context.NetStatus().empty())
                            result["multiplayer"] = context.NetStatus();
                        if (Json runtimes = RuntimesJson(context); !runtimes.empty())
                            result["runtimes"] = runtimes; // (runtime_call reaches them)
                        if (!note.empty())
                            result["note"] = note;
                        done(ToolResult::Data(result));
                    };
                    if (Mode(context) != "play")
                    {
                        done(ToolResult::Error(std::format("Play ended right away; read_log since {} tells why", logFrom)));
                        return true;
                    }
                    if (!context.PlayRunning || context.PlayRunning())
                    {
                        answer(true, {});
                        return true;
                    }
                    if (Now() - start > 40.0)
                    {
                        answer(false, "still waiting for the dedicated server: wait_for mode play, or read_log");
                        return true;
                    }
                    return false;
                });
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------------ simulate
        {
            Tool tool;
            tool.Name = "simulate";
            tool.Title = "Simulate";
            tool.Description = "Starts Simulate: physics and animation run, scripts do not, and the editor camera stays (the Simulate button). "
                               "stop ends it.";
            tool.InputSchema = Schema();
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json&, ToolDone done)
            {
                std::string error;
                if (!context.Simulate || !context.Simulate(error))
                {
                    done(ToolResult::Error(error.empty() ? std::string("Simulate did not start") : error));
                    return;
                }
                done(ToolResult::Data(Json{ { "mode", Mode(context) } }));
            };
            tools.push_back(std::move(tool));
        }

        // ---------------------------------------------------------------------------------------------------------------- stop
        {
            Tool tool;
            tool.Name = "stop";
            tool.Title = "Stop";
            tool.Description = "Ends Play or Simulate: the scene goes back to how it was before (changes made while playing are gone); the "
                               "other players' windows and the dedicated server close.";
            tool.InputSchema = Schema();
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json&, ToolDone done)
            {
                std::string error;
                if (!context.Stop || !context.Stop(error))
                {
                    done(ToolResult::Error(error.empty() ? std::string("not playing") : error));
                    return;
                }
                done(ToolResult::Data(Json{ { "mode", Mode(context) } }));
            };
            tools.push_back(std::move(tool));
        }

        // -------------------------------------------------------------------------------------------------------- focus_entity
        {
            Tool tool;
            tool.Name = "focus_entity";
            tool.Title = "Frame an entity";
            tool.Description = "Points the editor camera at an entity (name or uuid) so it fills the view, as the F key does -- then "
                               "screenshot. Edit and Simulate (in Play the viewport shows the game's own camera).";
            tool.InputSchema = Schema().String("entity", "The entity's name or uuid", true);
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                ToolDone answer = done;
                Ref<Scene> scene = RequireScene(context, answer);
                if (!scene)
                    return;
                if (Mode(context) == "play")
                {
                    done(ToolResult::Error("in Play the viewport shows the game's camera, not the editor camera (stop first, or use simulate)"));
                    return;
                }
                Entity found = FindByIdOrName(*scene, arguments["entity"].get<std::string>());
                if (!found)
                {
                    done(ToolResult::Error(std::format("no entity named '{}' (find_entities lists them)", arguments["entity"].get<std::string>())));
                    return;
                }
                // Framed by its size (what is drawn under it): an entity just placed may not have one yet -- its mesh or its model's
                // parts are still loading -- so this waits for it a little (5 s at most), then frames what there is.
                const UUID id = found.GetUUID();
                const double start = Now();
                context.EveryFrame([context, done, id, start]()
                {
                    Ref<Scene> current = context.ActiveScene ? context.ActiveScene() : nullptr;
                    Entity entity = current ? current->GetEntityByUUID(id) : Entity();
                    if (!entity)
                    {
                        done(ToolResult::Error("the entity is gone"));
                        return true;
                    }
                    const WorldBounds bounds = ComputeWorldBounds(*current, entity);
                    if (!bounds.Valid && Now() - start < 5.0)
                        return false;
                    context.FocusEntity(entity);
                    Json result = context.GetCamera ? CameraJson(context.GetCamera()) : Json::object();
                    result["entity"] = EntitySummary(*current, entity);
                    if (bounds.Valid)
                        result["framedSize"] = Vector(bounds.Max - bounds.Min); // (world units: the box it framed)
                    else
                        result["note"] = "it has nothing drawn (no mesh, or still loading): its position was framed at the closest distance";
                    done(ToolResult::Data(result));
                    return true;
                });
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------- get_viewport_camera
        {
            Tool tool;
            tool.Name = "get_viewport_camera";
            tool.Title = "Editor camera";
            tool.Description = "Where the editor camera of the active viewport is and looks: position, focal point, distance, pitch / yaw "
                               "in degrees (pitch positive looks down, yaw 0 looks along -Z), forward. World units (centimetres in "
                               "Facerun). In Play the viewport shows the game's camera instead.";
            tool.InputSchema = Schema();
            tool.Handler = [context](const Json&, ToolDone done)
            {
                if (!context.GetCamera)
                {
                    done(ToolResult::Error("no editor camera"));
                    return;
                }
                Json result = CameraJson(context.GetCamera());
                result["shownNow"] = Mode(context) == "play" ? "the game's camera (Play)" : "this camera";
                done(ToolResult::Data(result));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------- set_viewport_camera
        {
            Tool tool;
            tool.Name = "set_viewport_camera";
            tool.Title = "Move the editor camera";
            tool.Description = "Moves the editor camera: 'position' and 'look_at' (world units), or 'focal_point' with 'distance', "
                               "'pitch' and 'yaw' in degrees (any left out keep their value). Then screenshot.";
            tool.InputSchema = Schema().Build();
            for (const char* name : { "position", "look_at", "focal_point" })
                tool.InputSchema["properties"][name] = Json{ { "type", "array" }, { "items", Json{ { "type", "number" } } },
                                                             { "description", "[x, y, z] in world units" } };
            tool.InputSchema["properties"]["distance"] = Json{ { "type", "number" }, { "minimum", 0.0 }, { "description", "From the focal point" } };
            tool.InputSchema["properties"]["pitch"] = Json{ { "type", "number" }, { "minimum", -89.9 }, { "maximum", 89.9 },
                                                            { "description", "Degrees, positive looks down" } };
            tool.InputSchema["properties"]["yaw"] = Json{ { "type", "number" }, { "description", "Degrees, 0 looks along -Z, 90 along +X" } };
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                if (!context.GetCamera || !context.SetCamera)
                {
                    done(ToolResult::Error("no editor camera"));
                    return;
                }
                ViewportCamera camera = context.GetCamera();
                glm::dvec3 focal = camera.FocalPoint;
                double distance = camera.Distance;
                float pitch = camera.Pitch, yaw = camera.Yaw;
                std::string error;
                const bool lookFrom = arguments.contains("position"), lookAt = arguments.contains("look_at");
                if (lookFrom || lookAt)
                {
                    if (arguments.contains("focal_point") || arguments.contains("pitch") || arguments.contains("yaw") || arguments.contains("distance"))
                    {
                        done(ToolResult::Error("give 'position' / 'look_at', or 'focal_point' / 'distance' / 'pitch' / 'yaw', not both"));
                        return;
                    }
                    glm::dvec3 position = camera.Position, target = camera.FocalPoint;
                    if ((lookFrom && !ReadVector(arguments, "position", position, error)) || (lookAt && !ReadVector(arguments, "look_at", target, error)))
                    {
                        done(ToolResult::Error(error));
                        return;
                    }
                    const glm::dvec3 offset = target - position;
                    const double length = glm::length(offset);
                    if (length < 1e-6)
                    {
                        done(ToolResult::Error("'position' and 'look_at' are the same point"));
                        return;
                    }
                    const glm::dvec3 direction = offset / length;
                    // (the inverse of EditorCamera's forward: rotate(quat(-pitch, -yaw, 0), -Z))
                    pitch = static_cast<float>(std::asin(std::clamp(-direction.y, -1.0, 1.0)));
                    yaw = static_cast<float>(std::atan2(direction.x, -direction.z));
                    focal = target;
                    distance = length;
                }
                else
                {
                    if (arguments.contains("focal_point") && !ReadVector(arguments, "focal_point", focal, error))
                    {
                        done(ToolResult::Error(error));
                        return;
                    }
                    distance = arguments.value("distance", distance);
                    pitch = arguments.contains("pitch") ? glm::radians(arguments["pitch"].get<float>()) : pitch;
                    yaw = arguments.contains("yaw") ? glm::radians(arguments["yaw"].get<float>()) : yaw;
                }
                context.SetCamera(focal, distance, pitch, yaw);
                Json result = CameraJson(context.GetCamera());
                if (Mode(context) == "play")
                    result["note"] = "in Play the viewport shows the game's camera: this shows after stop";
                done(ToolResult::Data(result));
            };
            tools.push_back(std::move(tool));
        }

        // -------------------------------------------------------------------------------------------------------- runtime_call
        {
            Tool tool;
            tool.Name = "runtime_call";
            tool.Title = "Use a tool of another player's game";
            tool.Description = "Multiplayer Play with player MCP servers (play player_mcp true, or the Play settings' 'Players' MCP servers'): "
                               "each other player's NoxRuntime window and the dedicated server run an MCP server of their own; this calls "
                               "one of their tools and answers with its result (screenshots included). 'player': 2..8, or 0 for the "
                               "dedicated server; 'tool' left out lists that game's tools. Their tools: get_runtime_status, read_log, "
                               "get_hierarchy, find_entities, get_entity, screenshot, inject_input, wait_for, pause, list_commands, "
                               "run_command, quit. get_editor_status / play list the games ('runtimes').";
            tool.InputSchema = Schema()
                                   .Integer("player", "Which game: 2..8 (a player window) or 0 (the dedicated server)", true, 0, 8)
                                   .String("tool", "The tool to call (left out: list its tools)");
            tool.InputSchema["properties"]["arguments"] = Json{ { "type", "object" }, { "description", "The tool's arguments" } };
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                const int player = arguments["player"].get<int>();
                const std::vector<RuntimeEndpoint> runtimes = context.Runtimes ? context.Runtimes() : std::vector<RuntimeEndpoint>();
                const auto found = std::find_if(runtimes.begin(), runtimes.end(), [&](const RuntimeEndpoint& runtime) { return runtime.Player == player; });
                if (found == runtimes.end())
                {
                    std::string known;
                    for (const RuntimeEndpoint& runtime : runtimes)
                        known += std::format("{}{} ({})", known.empty() ? "" : ", ", runtime.Player, runtime.Name);
                    done(ToolResult::Error(runtimes.empty()
                                               ? std::string("no game with an MCP server: start multiplayer Play with player_mcp true (play), or turn on "
                                                             "'Players' MCP servers' in the Play settings")
                                               : std::format("no game {} with an MCP server (these are: {})", player, known)));
                    return;
                }
                if (!found->Running)
                {
                    done(ToolResult::Error(std::format("{} is not running anymore (its window was closed?)", found->Name)));
                    return;
                }
                std::string token;
                if (!LoadOrCreateToken(NoxFolder(), false, token))
                {
                    done(ToolResult::Error("the project's MCP token could not be read (.nox/mcp-token)"));
                    return;
                }
                const bool list = !arguments.contains("tool") || arguments["tool"].get<std::string>().empty();
                const std::string method = list ? "tools/list" : "tools/call";
                Json params = Json::object();
                if (!list)
                    params = Json{ { "name", arguments["tool"].get<std::string>() }, { "arguments", arguments.value("arguments", Json::object()) } };
                // The call runs on a thread of its own (the game answers between its frames; this editor keeps drawing), polled here.
                // Detached, not std::async: a future's destructor would wait for the game's answer if the server stopped meanwhile.
                // 55 s: the game's longest tool (wait_for, 50 s) answers within this server's own 60 s.
                struct Pending
                {
                    std::mutex Mutex;
                    bool Ready = false;
                    ClientReply Reply;
                };
                auto pending = std::make_shared<Pending>();
                std::thread([pending, port = found->Port, token, method, params]()
                {
                    ClientReply reply = CallServer(port, token, method, params, 55);
                    std::lock_guard lock(pending->Mutex);
                    pending->Reply = std::move(reply);
                    pending->Ready = true;
                }).detach();
                const std::string name = found->Name;
                context.EveryFrame([pending, done, list, name]()
                {
                    ClientReply answer;
                    {
                        std::lock_guard lock(pending->Mutex);
                        if (!pending->Ready)
                            return false;
                        answer = std::move(pending->Reply);
                    }
                    if (!answer.Error.empty())
                    {
                        done(ToolResult::Error(std::format("{}: {}", name, answer.Error)));
                        return true;
                    }
                    if (list)
                    {
                        Json tools = Json::array();
                        for (const Json& item : answer.Result.value("tools", Json::array()))
                            tools.push_back(Json{ { "name", item.value("name", std::string()) }, { "description", item.value("description", std::string()) },
                                                  { "inputSchema", item.value("inputSchema", Json::object()) } });
                        done(ToolResult::Data(Json{ { "game", name }, { "tools", tools } }));
                        return true;
                    }
                    done(ToolResultFromJson(answer.Result));
                    return true;
                });
            };
            tools.push_back(std::move(tool));
        }

        return tools;
    }
}
