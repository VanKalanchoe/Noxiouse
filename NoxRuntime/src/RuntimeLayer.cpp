#include "RuntimeLayer.h"
#include "NoxCore/Audio/AudioMixer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Network/NetClient.h"
#include "NoxCore/Network/NetProject.h"
#include "NoxCore/Network/NetReplication.h"
#include "NoxCore/Network/NetServer.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scripting/ScriptEngine.h"

namespace Nox
{
    namespace
    {
        std::filesystem::path FirstProjectIn(const std::filesystem::path& directory)
        {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(directory, error))
            {
                if (entry.is_regular_file(error) && entry.path().extension() == ".nproj")
                    return entry.path();
            }
            return {};
        }

        void Quit()
        {
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
        }

        // The network emulation from the command line: a preset (--net-emulation=average), then single values over it
        // (--net-lag=50 --net-jitter=10 --net-loss=2: milliseconds, milliseconds, percent; each direction).
        Net::NetEmulation ParseEmulation(const ApplicationCommandLineArgs& args)
        {
            Net::NetEmulation emulation;
            if (const char* preset = args.Value("--net-emulation"); preset && !Net::NetEmulation::FromPreset(preset, emulation))
                NOX_CORE_WARN("NoxRuntime: unknown --net-emulation '{}' (off, average, bad)", preset);
            if (const char* lag = args.Value("--net-lag"))
                emulation.LagMs = static_cast<float>(std::atof(lag));
            if (const char* jitter = args.Value("--net-jitter"))
                emulation.JitterMs = static_cast<float>(std::atof(jitter));
            if (const char* loss = args.Value("--net-loss"))
                emulation.LossPercent = static_cast<float>(std::atof(loss));
            return emulation;
        }

        // A connection quality (0..1) as a percentage; the transport reports -1 until it has measured it.
        std::string Quality(float quality)
        {
            return quality < 0.0f ? std::string("unknown") : std::format("{:.0f} %", quality * 100.0f);
        }

    }

    RuntimeLayer::RuntimeLayer()
        : Layer("RuntimeLayer")
    {
        m_Renderer = Application::Get().GetRenderer();
        m_Headless = Application::IsHeadless();

        const std::filesystem::path projectFile = FindProjectFile();
        if (projectFile.empty() || !Project::Load(projectFile, true))
        {
            NOX_CORE_ERROR("NoxRuntime: no project to play ({}). Usage: NoxRuntime [--server | --listen | --connect=<address>] [--project=]<project.nproj>",
                           projectFile.empty() ? std::string("none found") : projectFile.generic_string());
            Quit();
            return;
        }
        NOX_CORE_INFO("NoxRuntime: {} '{}'", m_Headless ? "serving" : "playing", projectFile.generic_string());
        m_ProjectName = Project::GetActive()->GetConfig().Name;
        if (m_Renderer)
        {
            // The project's quality preset ("Auto": the one this GPU should run, the fallback on smaller RT cards).
            m_Renderer->ApplyQualitySetting(Project::GetActive()->GetConfig().RenderQuality);
            m_Renderer->ApplyProjectRendererSettings(Project::GetActive()->GetConfig().Renderer);
        }
        AudioMixer::LoadProjectMixer(false); // its buses, effects and snapshots

        // The project's window mode (Borderless by default); "--windowed" on the command line keeps a window (testing).
        if (!m_Headless)
        {
            const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
            const bool windowed = args.HasSwitch("--windowed");
            if (!windowed)
                Application::Get().getWindow()->SetMode(ParseWindowMode(Project::GetActive()->GetConfig().WindowMode));
            // --window-pos=x,y / --window-size=w,h (pixels; the editor's multiplayer Play tiles its players' windows this way).
            SDL_Window* handle = Application::Get().getWindow() ? Application::Get().getWindow()->getHandle() : nullptr;
            int first = 0, second = 0;
            if (const char* size = args.Value("--window-size"); handle && windowed && size && std::sscanf(size, "%d,%d", &first, &second) == 2 &&
                first >= 320 && second >= 240)
                SDL_SetWindowSize(handle, first, second);
            if (const char* position = args.Value("--window-pos"); handle && windowed && position && std::sscanf(position, "%d,%d", &first, &second) == 2)
                SDL_SetWindowPosition(handle, first, second);
        }

        // The RuntimeAssetManager (Project::Load(path, true)): the cooked assets as they are, nothing on disk is ever written.
        if (!ScriptEngine::Initialize(SDL_GetBasePath()))
            NOX_CORE_ERROR("NoxRuntime: failed to initialize the .NET scripting backend (scripts will not run)");

        // --scene=Scenes/NetDemo.nox: another scene than the project's start scene (by its path under the asset directory).
        AssetHandle startScene = Project::GetActive()->GetConfig().StartScene;
        if (const char* scenePath = Application::Get().GetSpecification().CommandLineArgs.Value("--scene"))
        {
            const AssetHandle chosen = AssetManager::ResolvePath(std::filesystem::path(scenePath), AssetType::Scene);
            if (chosen != 0)
                startScene = chosen;
            else
                NOX_CORE_ERROR("NoxRuntime: --scene '{}' is not a scene under the asset directory; playing the start scene", scenePath);
        }
        if (startScene == 0)
        {
            NOX_CORE_ERROR("NoxRuntime: the project has no start scene (ProjectConfig StartScene)");
            Quit();
            return;
        }
        m_StartSceneHandle = startScene;
        DetermineNetRole();
        StartNetworking(); // (a server's replication exists before its scene's scripts start)
        StartScene(startScene);

        if (m_Headless)
        {
            // The console: lines typed into the server's terminal, handed to the main loop (UpdateServerConsole). The thread blocks
            // in the read; it is detached and ends with the process.
            m_Console = std::make_shared<ConsoleInput>();
            std::thread([console = m_Console]()
            {
                std::string line;
                while (std::getline(std::cin, line))
                {
                    std::lock_guard lock(console->Mutex);
                    console->Lines.push_back(line);
                }
            }).detach();
            NOX_CORE_INFO("Server console: the engine console's commands -- status, clients, kick <id> [reason], net.Emulation (netemu), "
                          "net.LagCompensation (lagcomp), net.DebugTrace (netdebug), quit (or Ctrl+C); 'help' lists every command");
        }
        Net::RegisterConsoleVariables(); // (also without a session: set before one starts)
        RegisterConsoleCommands();
        // --exec="cmd; cmd": console commands once the game is up (Unreal's -ExecCmds), e.g. --exec="net.DebugTrace 1".
        if (const char* exec = Application::Get().GetSpecification().CommandLineArgs.Value("--exec"))
            Console::Get().Execute(exec);
    }

    void RuntimeLayer::RegisterConsoleCommands()
    {
        Console& console = Console::Get();
        m_ConsoleCommands.push_back(console.RegisterCommand({ "quit", "quit", "Ends the game (a server tells its clients)", ConsoleAccess::Execute,
            [](const std::vector<std::string>&, ConsoleOutput& output)
            {
                output.Info("Quitting");
                Quit();
            } }));
        m_ConsoleCommands.push_back(console.RegisterCommand({ "exit", "exit", "Same as quit", ConsoleAccess::Execute,
            [](const std::vector<std::string>&, ConsoleOutput&) { Quit(); } }));
        if (m_NetRole != NetRole::DedicatedServer && m_NetRole != NetRole::ListenServer)
            return;
        m_ConsoleCommands.push_back(console.RegisterCommand({ "status", "status", "The server's tick rate, load, entities, clients and bandwidth",
            ConsoleAccess::Read, [this](const std::vector<std::string>&, ConsoleOutput&) { LogServerStatus(); } }));
        m_ConsoleCommands.push_back(console.RegisterCommand({ "clients", "clients", "The joined clients: id, name, address, ping",
            ConsoleAccess::Read, [this](const std::vector<std::string>&, ConsoleOutput&) { LogClients(); } }));
        m_ConsoleCommands.push_back(console.RegisterCommand({ "kick", "kick <client id> [reason]", "Disconnects a client", ConsoleAccess::Execute,
            [this](const std::vector<std::string>& arguments, ConsoleOutput& output)
            {
                if (arguments.empty() || !Server())
                {
                    output.Error("kick <client id> [reason]");
                    return;
                }
                const Net::ClientId client = static_cast<Net::ClientId>(std::strtoul(arguments[0].c_str(), nullptr, 10));
                std::string reason;
                for (size_t index = 1; index < arguments.size(); ++index)
                    reason += (index > 1 ? " " : "") + arguments[index];
                if (!Server()->Kick(client, reason.empty() ? "kicked by the server" : reason))
                    output.Error(std::format("No client {} (see 'clients')", arguments[0]));
            } }));
        // The server console's old names, kept: they run the engine's commands.
        m_ConsoleCommands.push_back(console.RegisterAlias("netemu", "net.Emulation", "netemu off|average|bad|<lag> <jitter> <loss>"));
        m_ConsoleCommands.push_back(console.RegisterAlias("lagcomp", "net.LagCompensation", "lagcomp on|off"));
        m_ConsoleCommands.push_back(console.RegisterAlias("netdebug", "net.DebugTrace", "netdebug on|off"));
    }

    RuntimeLayer::~RuntimeLayer()
    {
        NOX_CORE_INFO("RuntimeLayer Shutdown");

        // The sessions first: the clients hear "server shut down" / the server hears "left", while the game still exists.
        m_Session.reset();

        if (m_Scene)
            m_Scene->OnRuntimeStop();
        m_Scene = nullptr;
        ScriptEngine::Shutdown();

        Font::ReleaseDefault(); // the stats overlay's font, before the renderer goes (same as the editor)

        if (Project::GetActive() && Project::GetActive()->GetEditorAssetManager())
            Project::GetActive()->GetEditorAssetManager()->Shutdown();
    }

    void RuntimeLayer::OnUpdate(Timestep ts)
    {
        // Receive first (Unreal's TickDispatch): joins, leaves and the game's messages land before the simulation; a client that
        // joined switches to the server's scene here.
        UpdateNetworking(static_cast<float>(ts));
        if (!m_Scene || !Project::GetActive())
            return;

        // Same unused-asset sweep as the editor: a script destroying entities can leave meshes and textures unreferenced.
        auto assetManager = Project::GetActive()->GetEditorAssetManager();
        if (m_Scene->ConsumeAssetReferencesChanged() || assetManager->ConsumeLoadsSettledAfterSweep())
            m_UnloadUnusedAssetsRequested = true;
        // At most once a second (the editor's rule): a streamed world asks for it with nearly every cell and every load.
        if (m_UnloadUnusedAssetsRequested && std::chrono::steady_clock::now() - m_LastUnloadSweep >= std::chrono::seconds(1))
        {
            NOX_PROFILE_SCOPE("Unload Unused Assets");
            m_UnloadUnusedAssetsRequested = false;
            m_LastUnloadSweep = std::chrono::steady_clock::now();
            UnloadUnusedAssets();
        }

        if (m_Renderer)
        {
            // The window is the viewport: the renderer's output is the swapchain, the scene's cameras take its aspect.
            const NRI::Extent2D outputSize = m_Renderer->getOutputSize();
            if (outputSize.width > 0 && outputSize.height > 0)
            {
                m_Renderer->onViewportSizeChange(outputSize); // no-op while the size is unchanged
                m_Scene->OnViewportResize(outputSize.width, outputSize.height);
            }
            m_Scene->SetRenderer(m_Renderer);
            m_Scene->SetRenderer2D(m_Renderer->getRenderer2D());
        }
        {
            NOX_PROFILE_SCOPE("Scene Update");
            m_Scene->OnUpdateRuntime(ts);
        }

        {
            NOX_PROFILE_SCOPE("Asset Manager Update");
            assetManager->Update(); // publishes the background loads that finished
        }

        // The server's snapshots of this frame's results; the client's state that waited for scripts.
        if (m_Session)
            m_Session->AfterScene(static_cast<float>(ts));
        if (m_Headless)
            UpdateServerConsole(static_cast<float>(ts));
        // Send last (TickFlush): what this frame queued leaves now, not a few ms later.
        FlushNetworking();
    }

    void RuntimeLayer::UpdateServerConsole(float ts)
    {
        // The previous frame's work (this one is still running): the server's load, not the tick interval it sleeps out.
        const double work = Application::Get().GetLastFrameWorkTime();
        ++m_FramesSinceStatus;
        m_WorkSinceStatus += work;
        m_WorstWorkSinceStatus = std::max(m_WorstWorkSinceStatus, work);
        m_StatusTimer += ts;
        if (m_StatusTimer >= 10.0f)
            LogServerStatus();

        std::deque<std::string> lines;
        if (m_Console)
        {
            std::lock_guard lock(m_Console->Mutex);
            lines.swap(m_Console->Lines);
        }
        for (const std::string& line : lines)
            RunConsoleCommand(line);
    }

    void RuntimeLayer::RunConsoleCommand(const std::string& line)
    {
        // The engine console (its commands log what they print).
        Console::Get().Execute(line);
    }

    void RuntimeLayer::LogClients() const
    {
        if (!Server())
            return;
        const auto& clients = Server()->GetClients();
        NOX_CORE_INFO("Server: {}/{} clients", clients.size(), Server()->GetConfig().MaxClients);
        const double now = Net::NetNow();
        const std::vector<Net::NetReplication::ClientNetStats> replication =
            Replication() ? Replication()->GetClientStats() : std::vector<Net::NetReplication::ClientNetStats>{};
        for (const Net::NetClientInfo& client : clients)
        {
            const Net::ConnectionStats stats = Server()->GetStats(client.Id);
            auto relevancy = std::find_if(replication.begin(), replication.end(), [&](const auto& r) { return r.Client == client.Id; });
            NOX_CORE_INFO("  {} '{}' {}  ping {} ms, quality {} / {}, out {:.1f} KB/s, in {:.1f} KB/s, joined {:.0f} s ago", client.Id,
                          client.Name, client.Address, stats.PingMs, Quality(stats.QualityLocal), Quality(stats.QualityRemote),
                          stats.OutBytesPerSecond / 1024.0f, stats.InBytesPerSecond / 1024.0f, now - client.JoinedAt);
            if (relevancy != replication.end() && Replication())
                NOX_CORE_INFO("      relevant: {} of {} replicated entities, snapshots {:.1f} KB/s of {:.0f}, {} changes waited (last second), "
                              "{} streamed levels loaded", relevancy->Relevant, Replication()->GetReplicatedCount(),
                              relevancy->BytesPerSecond / 1024.0f, Replication()->GetClientRate() / 1024.0f, relevancy->Deferred,
                              relevancy->Levels);
        }
    }

    void RuntimeLayer::LogServerStatus()
    {
        if (!m_Scene)
            return;
        const uint64_t tick = m_Scene->GetTick();
        const float seconds = std::max(m_StatusTimer, 1e-3f);
        size_t entities = 0;
        for ([[maybe_unused]] auto entity : m_Scene->GetAllEntitiesWith<IDComponent>())
            ++entities;
        // Work per update against the tick's budget (16.7 ms at 60 Hz): the average is the server's load, the worst its spikes
        // (an update longer than the budget makes the next ones catch up with several ticks).
        const double budget = 1.0 / static_cast<double>(Application::Get().GetTickRate());
        const double averageWork = m_FramesSinceStatus > 0 ? m_WorkSinceStatus / m_FramesSinceStatus : 0.0;
        NOX_CORE_INFO("Server status: tick {} ({:.1f} ticks/s), {:.1f} updates/s, work {:.2f} ms average ({:.0f} % of the {:.1f} ms tick), "
                      "{:.2f} ms worst, {} entities, {}/{} clients, snapshots {:.1f} KB/s",
                      tick, static_cast<double>(tick - m_TickAtStatus) / seconds, static_cast<double>(m_FramesSinceStatus) / seconds,
                      averageWork * 1000.0, averageWork / budget * 100.0, budget * 1000.0, m_WorstWorkSinceStatus * 1000.0, entities,
                      Server() ? Server()->GetClients().size() : 0, Server() ? Server()->GetConfig().MaxClients : 0,
                      Replication() ? static_cast<double>(Replication()->GetLastSnapshotBytes()) * Replication()->GetSnapshotRate() / 1024.0 : 0.0);
        m_TickAtStatus = tick;
        m_StatusTimer = 0.0f;
        m_FramesSinceStatus = 0;
        m_WorkSinceStatus = 0.0;
        m_WorstWorkSinceStatus = 0.0;
    }

    void RuntimeLayer::OnEvent(Event& event)
    {
        // Alt+Enter: windowed <-> full screen. Esc: quit.
        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<KeyPressedEvent>([](KeyPressedEvent& pressed)
        {
            if (pressed.GetKeyCode() == SDL_SCANCODE_RETURN && (pressed.GetModifiers() & SDL_KMOD_ALT) != 0 && !pressed.IsRepeat())
            {
                Application::Get().getWindow()->ToggleFullscreen();
                return true;
            }
            if (pressed.GetKeyCode() == SDL_SCANCODE_ESCAPE && !pressed.IsRepeat())
            {
                Quit();
                return true;
            }
            return false;
        });
    }

    std::filesystem::path RuntimeLayer::FindProjectFile() const
    {
        const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
        if (const char* project = args.ProjectFile())
            return std::filesystem::path(project);

        // A packaged game keeps its project in Game/ next to the executable (File > Package Game in the editor).
        const std::filesystem::path workingDirectory = std::filesystem::current_path();
        const std::filesystem::path executableDirectory(SDL_GetBasePath());
        for (const std::filesystem::path& directory : { workingDirectory, executableDirectory, executableDirectory / "Game", workingDirectory / ".." / "Facerun" })
        {
            const std::filesystem::path project = FirstProjectIn(directory);
            if (!project.empty())
                return project;
        }
        return {};
    }

    void RuntimeLayer::StartScene(AssetHandle sceneHandle)
    {
        // A copy, like the editor's Play: the scene asset itself stays as it was loaded.
        Ref<Scene> sceneAsset = AssetManager::GetAsset<Scene>(sceneHandle);
        if (!sceneAsset)
        {
            NOX_CORE_ERROR("NoxRuntime: the start scene {} could not be loaded", static_cast<uint64_t>(sceneHandle));
            Quit();
            return;
        }

        if (m_Scene)
            m_Scene->OnRuntimeStop(); // (a client changing scenes: the one it played ends first)
        m_Scene = Scene::Copy(sceneAsset);
        m_SceneHandle = sceneHandle;
        // Its part in the game, before it starts (physics makes a client's replicated entities proxies).
        if (m_NetRole == NetRole::DedicatedServer || m_NetRole == NetRole::ListenServer)
            m_Scene->SetNetMode(Scene::NetMode::Server);
        else if (m_NetRole == NetRole::Client && Replication())
            m_Scene->SetNetMode(Scene::NetMode::Client);
        if (m_Renderer)
        {
            m_Scene->SetRenderer(m_Renderer);
            m_Scene->SetRenderer2D(m_Renderer->getRenderer2D());
            const NRI::Extent2D outputSize = m_Renderer->getOutputSize();
            m_Scene->OnViewportResize(outputSize.width, outputSize.height);
        }
        if (Replication())
            Replication()->SetScene(m_Scene.get());
        m_Scene->OnRuntimeStart();
        m_UnloadUnusedAssetsRequested = true;
#if NOX_PROFILE_STATS
        Profiler::Get().ResetStats();
#endif
    }

    void RuntimeLayer::UnloadUnusedAssets()
    {
        std::unordered_set<AssetHandle> referencedAssets;
        m_Scene->CollectAssetReferences(referencedAssets);
        Project::GetActive()->GetEditorAssetManager()->UnloadUnusedAssets(referencedAssets);
    }

    void RuntimeLayer::DetermineNetRole()
    {
        const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
        const char* connect = args.Value("--connect");
        if (m_Headless)
            m_NetRole = NetRole::DedicatedServer;
        else if (connect)
            m_NetRole = NetRole::Client;
        else if (args.HasSwitch("--listen"))
            m_NetRole = NetRole::ListenServer;
        if (connect && m_NetRole != NetRole::Client)
            NOX_CORE_WARN("NoxRuntime: --connect is ignored by a server");
    }

    void RuntimeLayer::StartNetworking()
    {
        const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
        if (m_NetRole == NetRole::Standalone)
        {
            NOX_CORE_INFO("NoxRuntime: standalone (no networking: --server, --listen or --connect <address> make a server or a client)");
            return;
        }
        NOX_CORE_INFO("NoxRuntime: {}", m_NetRole == NetRole::DedicatedServer ? "dedicated server"
                                        : m_NetRole == NetRole::ListenServer  ? "listen server"
                                                                              : "client");

        Net::NetSessionConfig config;
        config.Role = m_NetRole == NetRole::DedicatedServer ? Net::SessionRole::DedicatedServer
                    : m_NetRole == NetRole::ListenServer   ? Net::SessionRole::ListenServer
                                                           : Net::SessionRole::Client;
        config.Emulation = ParseEmulation(args);
        if (const char* value = args.Value("--port"))
        {
            const long number = std::strtol(value, nullptr, 10);
            if (number > 0 && number <= 65535)
                config.Port = static_cast<uint16_t>(number);
            else
                NOX_CORE_WARN("NoxRuntime: --port={} is not a port; using {}", value, config.Port);
        }
        if (const char* connect = args.Value("--connect"))
            config.Address = connect;
        if (const char* name = args.Value("--name"))
            config.PlayerName = name;
        if (const char* serverName = args.Value("--server-name"))
            config.ServerName = serverName;
        if (const char* maxClients = args.Value("--max-clients"))
            config.MaxClients = static_cast<uint32_t>((std::max)(1l, std::strtol(maxClients, nullptr, 10)));
        if (const char* rate = args.Value("--net-rate"))
            config.SnapshotRate = static_cast<float>(std::atof(rate));
        if (const char* rate = args.Value("--net-client-rate"))
            config.ClientRate = static_cast<float>(std::atof(rate));
        // --connect-retry=<seconds>: keep trying while the server is still starting (the editor's multiplayer Play launches both).
        if (const char* retry = args.Value("--connect-retry"))
            config.ConnectRetrySeconds = std::max(std::atof(retry), 0.0);
        // --net-debug: the network debug trace in the log (NetReplication::SetDebugTrace; the server console's `netdebug on|off`).
        config.DebugTrace = args.HasSwitch("--net-debug");

        m_Session = std::make_unique<Net::NetSession>(std::move(config));
        m_Session->FillWelcome = [this](Net::NetWelcome& welcome)
        {
            welcome.ServerTick = m_Scene ? m_Scene->GetTick() : 0;
            welcome.TickRate = static_cast<uint16_t>(Scene::TickRate);
            welcome.SceneHandle = static_cast<uint64_t>(m_SceneHandle);
        };
        m_Session->OnJoined = [this](AssetHandle scene) { return OnJoinedServer(scene); };
        m_Session->OnLeft = [this](Net::DisconnectReason reason, const std::string& text, bool wasJoined) { OnLeftServer(reason, text, wasJoined); };
        if (!m_Session->Start())
        {
            if (m_Session->IsServer())
            {
                NOX_CORE_ERROR("NoxRuntime: the server cannot accept clients (port {}); choose another one with --port=<number>",
                               m_Session->GetConfig().Port);
                m_Session.reset();
                if (m_NetRole == NetRole::DedicatedServer)
                    Quit(); // a dedicated server without its port has nothing to do
            }
            // (A client that cannot even start connecting -- a bad address -- stays on its start scene, saying why.)
        }
        UpdateWindowTitle();
    }

    void RuntimeLayer::UpdateNetworking(float ts)
    {
        if (m_Session)
            m_Session->Receive(ts);
        // The title once a second (the ping changes).
        m_TitleTimer += ts;
        if (m_TitleTimer >= 1.0f)
        {
            m_TitleTimer = 0.0f;
            UpdateWindowTitle();
        }
    }

    void RuntimeLayer::FlushNetworking()
    {
        if (m_Session)
            m_Session->Send();
    }

    bool RuntimeLayer::OnJoinedServer(AssetHandle scene)
    {
        // The server's scene (Unreal's travel to the server's map on join): a fresh copy, even when it is the one already playing,
        // so the client starts from the level as it was saved, like the server did. (The session's replication exists already.)
        if (!AssetManager::IsAssetHandleValid(scene))
            return false;
        StartScene(scene);
        UpdateWindowTitle();
        return true;
    }

    void RuntimeLayer::OnLeftServer(Net::DisconnectReason reason, const std::string& text, bool wasJoined)
    {
        // Back to the start scene (Unreal returns to the default map), standalone; nothing changes when the client never joined.
        if (wasJoined && m_StartSceneHandle != 0)
            StartScene(m_StartSceneHandle);
        UpdateWindowTitle();
    }

    void RuntimeLayer::UpdateWindowTitle()
    {
        if (m_Headless || m_NetRole == NetRole::Standalone)
            return;
        const std::string state = m_Session ? m_Session->Describe() : std::string("server not running (see the log)");
        if (const auto window = Application::Get().getWindow())
            SDL_SetWindowTitle(window->getHandle(), std::format("{} - {}", m_ProjectName, state).c_str());
    }
}
