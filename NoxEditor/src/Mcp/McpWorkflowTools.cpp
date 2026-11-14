#include "McpEditorTools.h"
#include "McpGraph.h"
#include "McpToolHelpers.h"

#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <regex>

#ifndef _WIN32
#include <sys/wait.h> // (WIFEXITED: std::system's status)
#endif

#include "NoxCore/Animation/AnimationGraphNodes.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Audio/AudioGraph.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/LogBuffer.h"
#include "NoxCore/NodeGraph/GraphCompiler.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"
#include "NoxCore/Project/Project.h"

// MCP4 part 2: graphs as JSON (animation and audio graphs), the engine's test programs, packaging.
namespace Nox::Mcp
{
    namespace
    {
        constexpr double MaxWaitSeconds = 50.0; // (under the server's 60 s call time-out)

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // A graph asset (animation or audio) by uuid or path.
        AssetHandle ResolveGraph(const Json& value, std::string& error)
        {
            std::string animationError, audioError;
            if (AssetHandle handle = ResolveAsset(value, AssetType::AnimationGraph, animationError))
                return handle;
            if (AssetHandle handle = ResolveAsset(value, AssetType::AudioGraph, audioError))
                return handle;
            error = animationError.find("not a") != std::string::npos ? animationError + " (graphs are AnimationGraph or AudioGraph assets)" : animationError;
            return 0;
        }

        // The warnings and errors the log got after `since` (the graph compiler explains a refusal there).
        Json LogProblemsSince(uint64_t since)
        {
            std::vector<LogEntry> entries;
            LogBuffer::Get().CopySince(since, entries);
            Json problems = Json::array();
            for (const LogEntry& entry : entries)
                if (entry.Level >= spdlog::level::warn && problems.size() < 20)
                    problems.push_back(entry.Text);
            return problems;
        }

        // ---- the test programs
        struct TestSuite
        {
            const char* Name;       // as the tool takes it
            const char* Executable; // next to NoxEditor
            const char* What;
        };
        constexpr TestSuite Suites[] = {
            { "tools", "NoxToolsTest", "the console, the MCP protocol, component checks, materials, graphs, build diagnostics" },
            { "network", "NoxNetTest", "sessions, replication, prediction, relevancy, lag compensation (--offline: no sockets)" },
        };

        struct TestRun
        {
            std::future<int> Process;
            std::filesystem::path Log;
            double Started = 0.0;
            Json Result; // the last finished run (null: none)
        };
        std::map<std::string, std::shared_ptr<TestRun>>& TestRuns()
        {
            static std::map<std::string, std::shared_ptr<TestRun>> runs;
            return runs;
        }

        Json ReadTestLog(const std::filesystem::path& log, int exitCode, double seconds)
        {
            std::ifstream file(log);
            std::string line;
            int passed = 0, checks = -1, failed = -1;
            Json failures = Json::array();
            static const std::regex summary(R"((\d+) checks, (\d+) failed)");
            while (std::getline(file, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.rfind("ok ", 0) == 0)
                    ++passed;
                else if (line.rfind("FAIL", 0) == 0 && failures.size() < 50)
                    failures.push_back(line);
                std::smatch match;
                if (std::regex_search(line, match, summary))
                {
                    checks = std::stoi(match[1].str());
                    failed = std::stoi(match[2].str());
                }
            }
            Json result{ { "exitCode", exitCode }, { "seconds", seconds }, { "log", log.generic_string() }, { "checksPassed", passed } };
            if (checks >= 0)
            {
                result["checks"] = checks;
                result["failed"] = failed;
            }
            result["failures"] = failures;
            result["passed"] = checks >= 0 && failed == 0 && exitCode == 0;
            if (checks < 0)
                result["note"] = "the program ended without its summary line (it crashed or could not start): see the log";
            return result;
        }

        PackageReport& LastPackage()
        {
            static PackageReport report;
            return report;
        }
        bool& HasLastPackage()
        {
            static bool has = false;
            return has;
        }
        bool& LastPackageWasServer()
        {
            static bool server = false;
            return server;
        }
        Json PackageJson(const PackageReport& report, bool server)
        {
            Json json{ { "success", report.Success }, { "server", server } };
            if (!report.Success)
                json["error"] = report.Error;
            else
            {
                json["executable"] = report.Executable.generic_string();
                json["assets"] = report.Assets;
                json["files"] = report.Files;
                json["megabytes"] = static_cast<double>(report.Bytes) / (1024.0 * 1024.0);
                json["texturesCooked"] = report.Cooked;
            }
            json["warnings"] = report.Warnings;
            return json;
        }
    }

    std::vector<Tool> CreateWorkflowTools(const EditorContext& context)
    {
        std::vector<Tool> tools;

        // -------------------------------------------------------------------------------------------------- list_graph_node_types
        {
            Tool tool;
            tool.Name = "list_graph_node_types";
            tool.Title = "Graph node types";
            tool.Description = "The node types an animation or audio graph can use: each type's input and output pins (name and type), its "
                               "properties with their defaults, enum names, value ranges and asset kinds. Read it before writing a graph with "
                               "set_graph.";
            tool.InputSchema = Schema()
                                   .Enum("domain", "AnimationGraph or AudioGraph (or give 'graph')", { kAnimationGraphDomain, kAudioGraphDomain })
                                   .String("graph", "A graph asset (uuid or path): its domain")
                                   .String("filter", "Only types whose name or category contains this (case ignored)");
            tool.Handler = [](const Json& arguments, ToolDone done)
            {
                std::string domain = arguments.value("domain", std::string());
                if (arguments.contains("graph"))
                {
                    std::string error;
                    const AssetHandle handle = ResolveGraph(arguments["graph"], error);
                    if (handle == 0)
                    {
                        done(ToolResult::Error(error));
                        return;
                    }
                    Ref<NodeGraphAsset> asset = AssetManager::GetAsset<NodeGraphAsset>(handle);
                    domain = asset ? asset->Graph.Domain : domain;
                }
                if (domain.empty())
                {
                    done(ToolResult::Error("give 'domain' (AnimationGraph or AudioGraph) or 'graph'"));
                    return;
                }
                Json types = NodeTypesJson(domain);
                if (const std::string filter = Lower(arguments.value("filter", std::string())); !filter.empty())
                {
                    Json kept = Json::array();
                    for (const Json& type : types)
                        if (Lower(type["type"].get<std::string>()).find(filter) != std::string::npos ||
                            Lower(type["category"].get<std::string>()).find(filter) != std::string::npos)
                            kept.push_back(type);
                    types = kept;
                }
                done(ToolResult::Data(Json{ { "domain", domain }, { "types", types } }));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------------- get_graph
        {
            Tool tool;
            tool.Name = "get_graph";
            tool.Title = "Read a graph";
            tool.Description = "An animation or audio graph (uuid or path) as JSON: parameters, nodes (id, type, position, properties; a state "
                               "machine's states with their own graphs and its transitions with rules), links by pin name, the output node. "
                               "Change it and give it back to set_graph.";
            tool.InputSchema = Schema().String("graph", "The graph asset: uuid or path (e.g. \"Animations/UAL_Locomotion.nanimgraph\")", true);
            tool.Handler = [](const Json& arguments, ToolDone done)
            {
                std::string error;
                const AssetHandle handle = ResolveGraph(arguments["graph"], error);
                if (handle == 0)
                {
                    done(ToolResult::Error(error));
                    return;
                }
                Ref<NodeGraphAsset> asset = AssetManager::GetAsset<NodeGraphAsset>(handle);
                if (!asset)
                {
                    done(ToolResult::Error(std::format("the graph {} could not be loaded (read_log)", AssetPath(handle))));
                    return;
                }
                done(ToolResult::Data(Json{ { "uuid", IdText(static_cast<uint64_t>(handle)) }, { "path", AssetPath(handle) },
                                            { "compiles", asset->Compiled.IsValid() }, { "graph", GraphToJson(asset->Graph) } }));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------------- set_graph
        {
            Tool tool;
            tool.Name = "set_graph";
            tool.Title = "Write a graph";
            tool.Description = "Replaces an animation or audio graph with 'value' (get_graph's shape). Checked first -- node types of the domain, "
                               "node ids, pins by name, state ids, parameter names, property kinds -- then compiled (pin types, cycles, the output "
                               "node); refused with the reasons when it does not, and nothing changes. Applied through the graph's editor window "
                               "(opened if it is not): one undo step there (Ctrl+Z), saved, and running animators and sounds use it at once. "
                               "Properties a node already had stay when left out; a new node starts with its type's defaults.";
            tool.InputSchema = Schema().String("graph", "The graph asset: uuid or path", true).Build();
            tool.InputSchema["properties"]["value"] = Json{ { "type", "object" }, { "description", "The whole graph, as get_graph gives it" } };
            tool.InputSchema["required"].push_back("value");
            tool.Access = ConsoleAccess::Write;
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                std::string error;
                const AssetHandle handle = ResolveGraph(arguments["graph"], error);
                if (handle == 0)
                {
                    done(ToolResult::Error(error));
                    return;
                }
                Ref<NodeGraphAsset> asset = AssetManager::GetAsset<NodeGraphAsset>(handle);
                if (!asset || !context.ApplyGraph)
                {
                    done(ToolResult::Error(std::format("the graph {} could not be loaded (read_log)", AssetPath(handle))));
                    return;
                }
                NodeGraph graph;
                if (std::string problem = GraphFromJson(arguments["value"], asset->Graph.Domain, graph, &asset->Graph); !problem.empty())
                {
                    done(ToolResult::Error(problem));
                    return;
                }
                // The compiler's own checks; it says why in the log.
                const uint64_t logBefore = LogBuffer::Get().GetLatestSequence();
                const CompiledGraph compiled = GraphCompiler::Compile(graph);
                const Json problems = LogProblemsSince(logBefore);
                if (!compiled.IsValid())
                {
                    std::string reasons;
                    for (const Json& line : problems)
                        reasons += "\n- " + line.get<std::string>();
                    done(ToolResult::Error(std::format("the graph does not compile; nothing changed.{}", reasons.empty() ? std::string(" (no reason logged)") : reasons)));
                    return;
                }
                if (std::string problem = context.ApplyGraph(handle, std::move(graph)); !problem.empty())
                {
                    done(ToolResult::Error(problem));
                    return;
                }
                NOX_CORE_INFO("MCP: replaced the graph {}", AssetPath(handle));
                Json result{ { "uuid", IdText(static_cast<uint64_t>(handle)) }, { "path", AssetPath(handle) }, { "nodes", asset->Graph.Nodes.size() },
                             { "links", asset->Graph.Links.size() }, { "undo", "Ctrl+Z in the graph's editor window" } };
                if (!problems.empty())
                    result["compilerWarnings"] = problems;
                done(ToolResult::Data(result));
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------------- run_tests
        {
            Tool tool;
            tool.Name = "run_tests";
            tool.Title = "Run the engine's tests";
            tool.Description = "Runs one of the engine's test programs (built with -DNOX_BUILD_TESTS=ON, next to the editor) and answers its "
                               "result: checks passed and failed, each failure's line, the log file. 'tools': the console, MCP, component and "
                               "material checks, build diagnostics, graphs. 'network': sessions, replication, prediction, lag compensation "
                               "('offline' skips the socket tests). A run longer than 'timeout' answers running: get_test_results later.";
            tool.InputSchema = Schema()
                                   .Enum("suite", "tools or network", { "tools", "network" }, true)
                                   .Boolean("offline", "network: skip the socket tests (default false)")
                                   .Number("timeout", "Wait at most this long, seconds (default 45)", false, 1.0, MaxWaitSeconds);
            tool.Access = ConsoleAccess::Execute; // (runs a program)
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                const std::string name = arguments["suite"].get<std::string>();
                const TestSuite* suite = nullptr;
                for (const TestSuite& candidate : Suites)
                    if (name == candidate.Name)
                        suite = &candidate;
#ifdef _WIN32
                const std::string executableName = std::string(suite->Executable) + ".exe";
#else
                const std::string executableName = suite->Executable;
#endif
                const std::filesystem::path directory = std::filesystem::path(Application::GetExecutableRootPath());
                const std::filesystem::path executable = directory / executableName;
                std::error_code exists;
                if (!std::filesystem::exists(executable, exists))
                {
                    done(ToolResult::Error(std::format("{} is not built (configure CMake with -DNOX_BUILD_TESTS=ON and build it; it goes next to the editor: {})",
                                                       executableName, directory.generic_string())));
                    return;
                }
                auto& run = TestRuns()[name];
                if (!run)
                    run = std::make_shared<TestRun>();
                if (!run->Process.valid())
                {
                    // Started here: its output into <Project>/.nox/test-logs/<suite>.log, run from its own folder (its own log file lands there).
                    const std::filesystem::path logs = Project::GetActive() ? Project::GetActiveProjectDirectory() / ".nox" / "test-logs"
                                                                            : directory / "test-logs";
                    std::filesystem::create_directories(logs, exists);
                    run->Log = logs / (name + ".log");
                    const bool offline = name == "network" && arguments.value("offline", false);
#ifdef _WIN32
                    std::string command = std::format("cd /d \"{}\" && \"{}\"{} > \"{}\" 2>&1", directory.string(), executable.string(), offline ? " --offline" : "",
                                                      run->Log.string());
                    command = "\"" + command + "\""; // (cmd /c drops the outer quotes)
#else
                    std::string command = std::format("cd \"{}\" && \"{}\"{} > \"{}\" 2>&1", directory.string(), executable.string(), offline ? " --offline" : "",
                                                      run->Log.string());
#endif
                    run->Started = Now();
                    run->Process = std::async(std::launch::async, [command]() { return std::system(command.c_str()); });
                    NOX_CORE_INFO("MCP: running {}{}", executableName, offline ? " --offline" : "");
                }
                const double start = Now();
                const double timeout = arguments.value("timeout", 45.0);
                std::shared_ptr<TestRun> shared = run;
                context.EveryFrame([done, shared, start, timeout, name]()
                {
                    if (shared->Process.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    {
                        int status = shared->Process.get();
#ifndef _WIN32
                        status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
                        shared->Result = ReadTestLog(shared->Log, status, Now() - shared->Started);
                        shared->Result["suite"] = name;
                        ToolResult answer = ToolResult::Data(shared->Result);
                        answer.IsError = !shared->Result["passed"].get<bool>();
                        done(std::move(answer));
                        return true;
                    }
                    if (Now() - start > timeout)
                    {
                        done(ToolResult::Data(Json{ { "suite", name }, { "running", true }, { "seconds", Now() - shared->Started },
                                                    { "note", "still running: get_test_results in a while" } }));
                        return true;
                    }
                    return false;
                });
            };
            tools.push_back(std::move(tool));
        }

        // ------------------------------------------------------------------------------------------------------ get_test_results
        {
            Tool tool;
            tool.Name = "get_test_results";
            tool.Title = "Test results";
            tool.Description = "The last result of each test program run with run_tests (passed, checks, failures, log), and whether one is "
                               "running.";
            tool.InputSchema = Schema();
            tool.Handler = [](const Json&, ToolDone done)
            {
                Json results = Json::object();
                for (const TestSuite& suite : Suites)
                {
                    Json entry{ { "what", suite.What } };
                    auto found = TestRuns().find(suite.Name);
                    if (found != TestRuns().end() && found->second)
                    {
                        TestRun& run = *found->second;
                        if (run.Process.valid() && run.Process.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                        {
                            int status = run.Process.get();
#ifndef _WIN32
                            status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
                            run.Result = ReadTestLog(run.Log, status, Now() - run.Started);
                        }
                        entry["running"] = run.Process.valid();
                        entry["last"] = run.Result;
                    }
                    else
                        entry["last"] = nullptr;
                    results[suite.Name] = entry;
                }
                done(ToolResult::Data(results));
            };
            tools.push_back(std::move(tool));
        }

        // ---------------------------------------------------------------------------------------------------------- package_game
        {
            Tool tool;
            tool.Name = "package_game";
            tool.Title = "Package the game";
            tool.Description = "File > Package Game (or Package Server with 'server'): saves the scene, then writes a folder that runs the game "
                               "on its own -- the runtime named after the project, the engine's files, and only the assets the scenes need, "
                               "cooked. The editor waits while it works (as with the dialog); answers the executable, size and warnings. A big "
                               "project can outlast the call's minute: then get_package_result reads the result.";
            tool.InputSchema = Schema()
                                   .String("output", "The output folder: an absolute path outside the project", true)
                                   .Boolean("server", "A dedicated server instead of the game (no window, GPU or sound; default false)")
                                   .Boolean("all_scenes", "Every scene, not only the start scene (default false)")
                                   .Boolean("pack_files", "The assets in one Content.npak (default true)")
                                   .Boolean("precompiled_shaders_only", "Without the shader compiler (default true)")
                                   .Boolean("self_contained_dotnet", "Copy the .NET runtime in (about 70 MB; default false)");
            tool.Access = ConsoleAccess::Execute;
            tool.Handler = [context](const Json& arguments, ToolDone done)
            {
                if (!context.Package || !Project::GetActive())
                {
                    done(ToolResult::Error("no project is open"));
                    return;
                }
                if (context.Mode && context.Mode() != "edit")
                {
                    done(ToolResult::Error("packaging is done in Edit mode (stop first)"));
                    return;
                }
                const std::filesystem::path output(arguments["output"].get<std::string>());
                if (!output.is_absolute())
                {
                    done(ToolResult::Error("'output' must be an absolute path (a folder outside the project)"));
                    return;
                }
                std::error_code error;
                const std::filesystem::path project = std::filesystem::weakly_canonical(Project::GetActiveProjectDirectory(), error);
                const std::filesystem::path target = std::filesystem::weakly_canonical(output, error);
                const std::filesystem::path relative = target.lexically_relative(project);
                if (!relative.empty() && *relative.begin() != ".." && !relative.is_absolute())
                {
                    done(ToolResult::Error("'output' is inside the project: choose a folder outside it (the package would be packaged into itself)"));
                    return;
                }
                PackageSettings settings;
                settings.OutputDirectory = output;
                settings.Server = arguments.value("server", false);
                settings.AllScenes = arguments.value("all_scenes", false);
                settings.PackFiles = arguments.value("pack_files", true);
                settings.PrecompiledShadersOnly = arguments.value("precompiled_shaders_only", true);
                settings.SelfContainedDotNet = arguments.value("self_contained_dotnet", false);
                NOX_CORE_INFO("MCP: packaging the {} into {}", settings.Server ? "dedicated server" : "game", output.generic_string());
                LastPackage() = context.Package(settings);
                HasLastPackage() = true;
                LastPackageWasServer() = settings.Server;
                Json result = PackageJson(LastPackage(), settings.Server);
                ToolResult answer = ToolResult::Data(result);
                answer.IsError = !LastPackage().Success;
                done(std::move(answer));
            };
            tools.push_back(std::move(tool));
        }

        // ---------------------------------------------------------------------------------------------------- get_package_result
        {
            Tool tool;
            tool.Name = "get_package_result";
            tool.Title = "Last package result";
            tool.Description = "The result of the last package_game: success, the executable, size, warnings or the error.";
            tool.InputSchema = Schema();
            tool.Handler = [](const Json&, ToolDone done)
            {
                if (!HasLastPackage())
                {
                    done(ToolResult::Data(Json{ { "last", nullptr } }));
                    return;
                }
                done(ToolResult::Data(Json{ { "last", PackageJson(LastPackage(), LastPackageWasServer()) } }));
            };
            tools.push_back(std::move(tool));
        }

        return tools;
    }
}
