#include "McpEditorTools.h"
#include "McpInputPlayer.h"
#include "McpToolHelpers.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <memory>

#include <SDL3/SDL_keyboard.h>

#include "NoxCore/Core/Input.h"
#include "NoxCore/Core/LogBuffer.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Scene.h"

// MCP3: play and verify -- start and stop Play / Simulate, wait for something to happen, drive the game with input, look with the
// editor camera. Tools that take time answer later through `done`, polled once a frame (EditorContext::EveryFrame).
namespace Nox::Mcp
{
    namespace
    {
        constexpr double MaxWaitSeconds = 50.0; // (under the server's 60 s call time-out)

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        struct KeyName
        {
            const char* Name;
            int Scancode;
        };

        // The C# KeyCode names (NoxScriptCore/Source/Input.cs): the same numbers as SDL's scancodes.
        const std::vector<KeyName>& KeyTable()
        {
            static const std::vector<KeyName> table = []()
            {
                std::vector<KeyName> keys;
                static const char* letters[] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                                 "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };
                for (int i = 0; i < 26; ++i)
                    keys.push_back({ letters[i], SDL_SCANCODE_A + i });
                static const char* digits[] = { "Alpha1", "Alpha2", "Alpha3", "Alpha4", "Alpha5", "Alpha6", "Alpha7", "Alpha8", "Alpha9", "Alpha0" };
                for (int i = 0; i < 10; ++i)
                    keys.push_back({ digits[i], SDL_SCANCODE_1 + i });
                static const char* functions[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
                for (int i = 0; i < 12; ++i)
                    keys.push_back({ functions[i], SDL_SCANCODE_F1 + i });
                for (const KeyName& key : std::initializer_list<KeyName>{
                         { "Return", SDL_SCANCODE_RETURN }, { "Escape", SDL_SCANCODE_ESCAPE }, { "Backspace", SDL_SCANCODE_BACKSPACE },
                         { "Tab", SDL_SCANCODE_TAB }, { "Space", SDL_SCANCODE_SPACE }, { "Right", SDL_SCANCODE_RIGHT }, { "Left", SDL_SCANCODE_LEFT },
                         { "Down", SDL_SCANCODE_DOWN }, { "Up", SDL_SCANCODE_UP }, { "LeftControl", SDL_SCANCODE_LCTRL },
                         { "LeftShift", SDL_SCANCODE_LSHIFT }, { "LeftAlt", SDL_SCANCODE_LALT }, { "RightControl", SDL_SCANCODE_RCTRL },
                         { "RightShift", SDL_SCANCODE_RSHIFT }, { "RightAlt", SDL_SCANCODE_RALT } })
                    keys.push_back(key);
                return keys;
            }();
            return table;
        }

        int MouseButton(const std::string& name)
        {
            const std::string lower = Lower(name);
            return lower == "left" ? 1 : lower == "middle" ? 2 : lower == "right" ? 3 : 0;
        }

        // An entity by uuid (a digit string) or by name (the first in a stable order; case ignored).
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

        // inject_input's holds, shared by its calls: one poll a frame while any is going.
        struct InputDriver
        {
            InputPlayer Player;
            std::map<uint64_t, std::pair<ToolDone, Json>> Waiting; // the call to answer when the hold ends, and what it held
            bool Polling = false;

            static void Apply(const InputPlayer::Frame& frame)
            {
                for (const auto& [key, down] : frame.Keys)
                    Input::SetInjectedKey(static_cast<SDL_Scancode>(key), down);
                for (const auto& [button, down] : frame.Buttons)
                    Input::SetInjectedMouseButton(button, down);
                if (frame.MoveX != 0.0f || frame.MoveY != 0.0f)
                    Input::AddInjectedMouseMotion(frame.MoveX, frame.MoveY);
                if (frame.Scroll != 0.0f)
                    Input::AddInjectedScroll(frame.Scroll);
            }
        };
    }

    int KeyScancode(const std::string& name)
    {
        const std::string lower = Lower(name);
        for (const KeyName& key : KeyTable())
            if (Lower(key.Name) == lower)
                return key.Scancode;
        // Short names people write.
        static const std::map<std::string, int> aliases{
            { "enter", SDL_SCANCODE_RETURN }, { "esc", SDL_SCANCODE_ESCAPE }, { "shift", SDL_SCANCODE_LSHIFT }, { "ctrl", SDL_SCANCODE_LCTRL },
            { "control", SDL_SCANCODE_LCTRL }, { "alt", SDL_SCANCODE_LALT }, { "leftctrl", SDL_SCANCODE_LCTRL }, { "rightctrl", SDL_SCANCODE_RCTRL },
            { "0", SDL_SCANCODE_0 }, { "1", SDL_SCANCODE_1 }, { "2", SDL_SCANCODE_2 }, { "3", SDL_SCANCODE_3 }, { "4", SDL_SCANCODE_4 },
            { "5", SDL_SCANCODE_5 }, { "6", SDL_SCANCODE_6 }, { "7", SDL_SCANCODE_7 }, { "8", SDL_SCANCODE_8 }, { "9", SDL_SCANCODE_9 } };
        if (auto it = aliases.find(lower); it != aliases.end())
            return it->second;
        // SDL's own names ("Left Shift", "Keypad 4" ...).
        const SDL_Scancode scancode = SDL_GetScancodeFromName(name.c_str());
        return scancode == SDL_SCANCODE_UNKNOWN ? 0 : static_cast<int>(scancode);
    }

    std::vector<std::string> KeyNames()
    {
        std::vector<std::string> names;
        for (const KeyName& key : KeyTable())
            names.push_back(key.Name);
        return names;
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
                                   .Boolean("debug_log", "Multiplayer: the network debug trace in every player's log (default false)");
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

        // --------------------------------------------------------------------------------------------------------------- pause
        {
            Tool tool;
            tool.Name = "pause";
            tool.Title = "Pause or step";
            tool.Description = "Pauses or resumes Play / Simulate; 'step' advances a paused game by that many frames (then look with "
                               "screenshot or get_entity).";
            tool.InputSchema = Schema()
                                   .Boolean("paused", "true pauses, false resumes", true)
                                   .Integer("step", "While paused: advance this many frames", false, 0, 600);
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                std::string error;
                if (!context.Pause || !context.Pause(arguments["paused"].get<bool>(), arguments.value("step", 0), error))
                {
                    done(ToolResult::Error(error.empty() ? std::string("not playing") : error));
                    return;
                }
                done(ToolResult::Data(Json{ { "mode", Mode(context) }, { "paused", context.Paused && context.Paused() } }));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------------ wait_for
        {
            Tool tool;
            tool.Name = "wait_for";
            tool.Title = "Wait for something";
            tool.Description = "Waits while the editor runs: 'seconds' alone waits that long; otherwise until every condition given holds "
                               "(checked each frame) or 'timeout' passes -- a log line with all the words of 'log' (written after 'since', "
                               "default: from now), the editor reaching 'mode', an entity (name or uuid) existing or gone. Answers met "
                               "true / false and what it saw. At most 50 s per call.";
            tool.InputSchema = Schema()
                                   .Number("seconds", "Just wait this long", false, 0.0, MaxWaitSeconds)
                                   .String("log", "Words a new log line must all contain (case ignored)")
                                   .Integer("since", "The log lines after this sequence count (default: the newest now)", false, 0)
                                   .Enum("log_level", "The lowest level that counts (default verbose)", { "verbose", "info", "warning", "error" })
                                   .Enum("mode", "The editor's mode to reach", { "edit", "play", "simulate" })
                                   .String("entity", "An entity's name or uuid")
                                   .Enum("entity_state", "exists (default) or gone", { "exists", "gone" })
                                   .Number("timeout", "Give up after this long (default 10 s)", false, 0.0, MaxWaitSeconds);
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                const bool hasLog = arguments.contains("log"), hasMode = arguments.contains("mode"), hasEntity = arguments.contains("entity");
                const double start = Now();
                if (!hasLog && !hasMode && !hasEntity)
                {
                    if (!arguments.contains("seconds"))
                    {
                        done(ToolResult::Error("give 'seconds', or a condition: 'log', 'mode' or 'entity'"));
                        return;
                    }
                    const double seconds = arguments["seconds"].get<double>();
                    context.EveryFrame([context, done, start, seconds]()
                    {
                        if (Now() - start < seconds)
                            return false;
                        done(ToolResult::Data(Json{ { "waited", Now() - start }, { "mode", Mode(context) } }));
                        return true;
                    });
                    return;
                }
                std::vector<std::string> words;
                {
                    std::string word;
                    for (char c : Lower(arguments.value("log", std::string())) + " ")
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
                }
                const std::string levelName = arguments.value("log_level", std::string("verbose"));
                const int minimum = levelName == "error" ? 3 : levelName == "warning" ? 2 : levelName == "info" ? 1 : 0;
                auto rank = [](spdlog::level::level_enum level)
                {
                    return level <= spdlog::level::debug ? 0 : level == spdlog::level::info ? 1 : level == spdlog::level::warn ? 2 : 3;
                };
                struct State
                {
                    uint64_t Cursor = 0;
                    Json LogLine; // the line that matched
                };
                auto state = std::make_shared<State>();
                state->Cursor = arguments.contains("since") ? arguments["since"].get<uint64_t>() : LogBuffer::Get().GetLatestSequence();
                const std::string wantedMode = arguments.value("mode", std::string());
                const std::string entityName = arguments.value("entity", std::string());
                const bool wantGone = arguments.value("entity_state", std::string("exists")) == "gone";
                const double timeout = arguments.value("timeout", 10.0);

                context.EveryFrame([=]()
                {
                    // The log: new lines since the last look, until one matches (then it stays met).
                    if (hasLog && state->LogLine.is_null())
                    {
                        std::vector<LogEntry> entries;
                        state->Cursor = std::max(state->Cursor, LogBuffer::Get().CopySince(state->Cursor, entries));
                        for (const LogEntry& entry : entries)
                        {
                            if (rank(entry.Level) < minimum)
                                continue;
                            const std::string text = Lower(entry.Text);
                            if (std::all_of(words.begin(), words.end(), [&](const std::string& w) { return text.find(w) != std::string::npos; }))
                            {
                                state->LogLine = Json{ { "sequence", entry.Sequence }, { "text", entry.Text } };
                                break;
                            }
                        }
                    }
                    const std::string mode = Mode(context);
                    const bool modeMet = !hasMode || (mode == wantedMode && (mode != "play" || !context.PlayRunning || context.PlayRunning()));
                    Json entityJson;
                    bool entityMet = true;
                    if (hasEntity)
                    {
                        Ref<Scene> scene = context.ActiveScene ? context.ActiveScene() : nullptr;
                        Entity entity = scene ? FindByIdOrName(*scene, entityName) : Entity();
                        entityMet = wantGone ? !entity : static_cast<bool>(entity);
                        if (entity)
                            entityJson = EntitySummary(*scene, entity);
                    }
                    const bool met = (!hasLog || !state->LogLine.is_null()) && modeMet && entityMet;
                    const double waited = Now() - start;
                    if (!met && waited < timeout)
                        return false;
                    Json result{ { "met", met }, { "waited", waited }, { "mode", mode } };
                    if (hasLog)
                    {
                        result["logLine"] = state->LogLine;
                        result["logChecked"] = state->Cursor; // (read_log since this goes on from here)
                    }
                    if (hasEntity)
                        result["entity"] = entityJson;
                    done(ToolResult::Data(result));
                    return true;
                });
            };
            tools.push_back(std::move(tool));
        }

        // -------------------------------------------------------------------------------------------------------- inject_input
        {
            auto driver = std::make_shared<InputDriver>();
            Tool tool;
            tool.Name = "inject_input";
            tool.Title = "Press keys and move the mouse in the game";
            tool.Description = "In Play: holds keys and mouse buttons for 'duration' seconds and moves the mouse / wheel over that time, as "
                               "the game's scripts read them (C# Input) -- the editor window does not have to be in front. Keys by the C# "
                               "KeyCode names: W, A, S, D, Space, LeftShift, LeftControl, Return, Escape, Alpha1, F1, Up ... Answers when "
                               "it lets go; calls that overlap combine (W for 2 s while another call taps Space). Then look with "
                               "screenshot / get_entity.";
            tool.InputSchema = Schema()
                                   .Array("keys", "Keys to hold, e.g. [\"W\", \"LeftShift\"]", "string")
                                   .Array("mouse_buttons", "left, right, middle", "string")
                                   .Array("mouse_move", "[dx, dy]: pixels moved in all over the duration (x right, y down)", "number")
                                   .Number("scroll", "Wheel notches in all (positive: away from you)")
                                   .Number("duration", "How long to hold, seconds (default 0.1; 0: one frame)", false, 0.0, 10.0);
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context, driver](const Json& arguments, ToolDone done)
            {
                if (Mode(context) != "play" || (context.PlayRunning && !context.PlayRunning()))
                {
                    done(ToolResult::Error("input goes to the game: start Play first (play)"));
                    return;
                }
                InputPlayer::Hold hold;
                Json held = Json::object();
                for (const Json& key : arguments.value("keys", Json::array()))
                {
                    const int scancode = KeyScancode(key.get<std::string>());
                    if (scancode == 0)
                    {
                        std::string names;
                        for (const std::string& name : KeyNames())
                            names += (names.empty() ? "" : ", ") + name;
                        done(ToolResult::Error(std::format("unknown key '{}' (keys: {})", key.get<std::string>(), names)));
                        return;
                    }
                    hold.Keys.push_back(scancode);
                }
                for (const Json& button : arguments.value("mouse_buttons", Json::array()))
                {
                    const int index = MouseButton(button.get<std::string>());
                    if (index == 0)
                    {
                        done(ToolResult::Error(std::format("unknown mouse button '{}' (left, right, middle)", button.get<std::string>())));
                        return;
                    }
                    hold.Buttons.push_back(index);
                }
                if (arguments.contains("mouse_move"))
                {
                    const Json& move = arguments["mouse_move"];
                    if (move.size() != 2)
                    {
                        done(ToolResult::Error("'mouse_move' must be [dx, dy]"));
                        return;
                    }
                    hold.MoveX = move[0].get<float>();
                    hold.MoveY = move[1].get<float>();
                }
                hold.Scroll = arguments.value("scroll", 0.0f);
                hold.Duration = arguments.value("duration", 0.1);
                if (hold.Keys.empty() && hold.Buttons.empty() && hold.MoveX == 0.0f && hold.MoveY == 0.0f && hold.Scroll == 0.0f)
                {
                    done(ToolResult::Error("nothing to do: give keys, mouse_buttons, mouse_move or scroll"));
                    return;
                }
                for (const char* name : { "keys", "mouse_buttons", "mouse_move", "scroll" })
                    if (arguments.contains(name))
                        held[name] = arguments[name];
                held["duration"] = hold.Duration;
                const uint64_t id = driver->Player.Add(std::move(hold));
                driver->Waiting[id] = { done, held };
                if (driver->Polling)
                    return;
                driver->Polling = true;
                context.EveryFrame([context, driver]()
                {
                    const bool playing = Mode(context) == "play";
                    const InputPlayer::Frame frame = playing ? driver->Player.Advance(Now()) : driver->Player.Clear();
                    InputDriver::Apply(frame);
                    for (uint64_t finished : frame.Finished)
                    {
                        auto it = driver->Waiting.find(finished);
                        if (it == driver->Waiting.end())
                            continue;
                        if (playing)
                            it->second.first(ToolResult::Data(Json{ { "released", it->second.second } }));
                        else
                            it->second.first(ToolResult::Error("Play ended before the input was let go"));
                        driver->Waiting.erase(it);
                    }
                    if (!driver->Player.Empty())
                        return false;
                    driver->Polling = false;
                    return true;
                });
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
                Entity entity = FindByIdOrName(*scene, arguments["entity"].get<std::string>());
                if (!entity)
                {
                    done(ToolResult::Error(std::format("no entity named '{}' (find_entities lists them)", arguments["entity"].get<std::string>())));
                    return;
                }
                context.FocusEntity(entity);
                Json result = context.GetCamera ? CameraJson(context.GetCamera()) : Json::object();
                result["entity"] = EntitySummary(*scene, entity);
                done(ToolResult::Data(result));
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

        return tools;
    }
}
