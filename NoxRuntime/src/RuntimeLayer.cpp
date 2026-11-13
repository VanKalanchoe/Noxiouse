#include "RuntimeLayer.h"
#include "NoxCore/Audio/AudioMixer.h"

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

        std::string Describe(const Net::NetEmulation& emulation)
        {
            if (!emulation.IsActive())
                return "off";
            return std::format("{:.0f} ms lag, {:.0f} ms jitter, {:.1f} % loss (each direction)", emulation.LagMs, emulation.JitterMs,
                               emulation.LossPercent);
        }

        // A connection quality (0..1) as a percentage; the transport reports -1 until it has measured it.
        std::string Quality(float quality)
        {
            return quality < 0.0f ? std::string("unknown") : std::format("{:.0f} %", quality * 100.0f);
        }

        std::vector<std::string> SplitWords(const std::string& line)
        {
            std::vector<std::string> words;
            std::istringstream stream(line);
            for (std::string word; stream >> word;)
                words.push_back(word);
            return words;
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
        }

        // The RuntimeAssetManager (Project::Load(path, true)): the cooked assets as they are, nothing on disk is ever written.
        if (!ScriptEngine::Initialize(SDL_GetBasePath()))
            NOX_CORE_ERROR("NoxRuntime: failed to initialize the .NET scripting backend (scripts will not run)");

        // --scene=Scenes/NetDemo.nox: another scene than the project's start scene (by its path under the asset directory).
        AssetHandle startScene = Project::GetActive()->GetConfig().StartScene;
        if (const char* scenePath = Application::Get().GetSpecification().CommandLineArgs.Value("--scene"))
        {
            const AssetHandle chosen = AssetManager::FindHandleByPath(std::filesystem::path(scenePath), AssetType::Scene);
            if (chosen != 0)
                startScene = chosen;
            else
                NOX_CORE_ERROR("NoxRuntime: --scene '{}' is not a scene in the asset registry (open the project in the editor once so it "
                               "registers new files); playing the start scene", scenePath);
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
            NOX_CORE_INFO("Server console: status, clients, kick <id> [reason], netemu off|average|bad|<lag> <jitter> <loss>, quit (or Ctrl+C)");
        }
    }

    RuntimeLayer::~RuntimeLayer()
    {
        NOX_CORE_INFO("RuntimeLayer Shutdown");

        // The sessions first: the clients hear "server shut down" / the server hears "left", while the game still exists.
        m_Replication.reset();
        m_Client.reset();
        m_Server.reset();

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
        if (m_Replication)
            m_Replication->Update(static_cast<float>(ts));
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
        const std::vector<std::string> words = SplitWords(line);
        if (words.empty())
            return;
        const std::string& command = words[0];
        if (command == "quit" || command == "exit")
        {
            NOX_CORE_INFO("Server: quitting");
            Quit();
        }
        else if (command == "status")
            LogServerStatus();
        else if (command == "clients")
            LogClients();
        else if (command == "kick" && words.size() >= 2 && m_Server)
        {
            const Net::ClientId client = static_cast<Net::ClientId>(std::strtoul(words[1].c_str(), nullptr, 10));
            std::string reason;
            for (size_t index = 2; index < words.size(); ++index)
                reason += (index > 2 ? " " : "") + words[index];
            if (!m_Server->Kick(client, reason.empty() ? "kicked by the server" : reason))
                NOX_CORE_WARN("Server console: no client {} (see 'clients')", words[1]);
        }
        else if (command == "netemu" && words.size() >= 2 && m_Server)
        {
            Net::NetEmulation emulation;
            if (words.size() >= 4)
            {
                emulation.LagMs = static_cast<float>(std::atof(words[1].c_str()));
                emulation.JitterMs = static_cast<float>(std::atof(words[2].c_str()));
                emulation.LossPercent = static_cast<float>(std::atof(words[3].c_str()));
            }
            else if (!Net::NetEmulation::FromPreset(words[1], emulation))
            {
                NOX_CORE_WARN("Server console: netemu off | average | bad | <lag ms> <jitter ms> <loss %>");
                return;
            }
            m_Server->SetEmulation(emulation);
            NOX_CORE_INFO("Server: network emulation {}", Describe(emulation));
        }
        else
            NOX_CORE_WARN("Server console: unknown command '{}' (status, clients, kick <id> [reason], netemu ..., quit)", line);
    }

    void RuntimeLayer::LogClients() const
    {
        if (!m_Server)
            return;
        const auto& clients = m_Server->GetClients();
        NOX_CORE_INFO("Server: {}/{} clients", clients.size(), m_Server->GetConfig().MaxClients);
        const double now = Net::NetNow();
        for (const Net::NetClientInfo& client : clients)
        {
            const Net::ConnectionStats stats = m_Server->GetStats(client.Id);
            NOX_CORE_INFO("  {} '{}' {}  ping {} ms, quality {} / {}, out {:.1f} KB/s, in {:.1f} KB/s, joined {:.0f} s ago", client.Id,
                          client.Name, client.Address, stats.PingMs, Quality(stats.QualityLocal), Quality(stats.QualityRemote),
                          stats.OutBytesPerSecond / 1024.0f, stats.InBytesPerSecond / 1024.0f, now - client.JoinedAt);
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
                      m_Server ? m_Server->GetClients().size() : 0, m_Server ? m_Server->GetConfig().MaxClients : 0,
                      m_Replication ? static_cast<double>(m_Replication->GetLastSnapshotBytes()) * m_Replication->GetSnapshotRate() / 1024.0 : 0.0);
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
        else if (m_NetRole == NetRole::Client && m_Replication)
            m_Scene->SetNetMode(Scene::NetMode::Client);
        if (m_Renderer)
        {
            m_Scene->SetRenderer(m_Renderer);
            m_Scene->SetRenderer2D(m_Renderer->getRenderer2D());
            const NRI::Extent2D outputSize = m_Renderer->getOutputSize();
            m_Scene->OnViewportResize(outputSize.width, outputSize.height);
        }
        if (m_Replication)
            m_Replication->SetScene(m_Scene.get());
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
        const char* connect = args.Value("--connect");
        if (m_NetRole == NetRole::Standalone)
        {
            NOX_CORE_INFO("NoxRuntime: standalone (no networking: --server, --listen or --connect <address> make a server or a client)");
            return;
        }
        NOX_CORE_INFO("NoxRuntime: {}", m_NetRole == NetRole::DedicatedServer ? "dedicated server"
                                        : m_NetRole == NetRole::ListenServer  ? "listen server"
                                                                              : "client");

        const Net::NetEmulation emulation = ParseEmulation(args);
        const Net::NetCompatibility compatibility = Net::MakeProjectCompatibility();
        uint16_t port = Net::DefaultPort;
        if (const char* value = args.Value("--port"))
        {
            const long number = std::strtol(value, nullptr, 10);
            if (number > 0 && number <= 65535)
                port = static_cast<uint16_t>(number);
            else
                NOX_CORE_WARN("NoxRuntime: --port={} is not a port; using {}", value, port);
        }

        if (m_NetRole == NetRole::Client)
        {
            Net::NetClientConfig config;
            config.Compatibility = compatibility;
            const char* name = args.Value("--name");
            config.PlayerName = name ? name : "Player";
            m_Client = std::make_unique<Net::NetClient>(std::move(config));
            m_Client->SetEmulation(emulation);
            m_Client->OnConnected = [this](const Net::NetWelcome& welcome) { OnJoinedServer(welcome); };
            m_Client->OnDisconnected = [this](Net::DisconnectReason reason, const std::string& text) { OnLeftServer(reason, text); };
            if (emulation.IsActive())
                NOX_CORE_INFO("Net: network emulation {}", Describe(emulation));
            // A connection attempt that can't even start (a bad address) keeps the client on its start scene, saying why.
            m_Client->Connect(connect);
            UpdateWindowTitle();
            return;
        }

        Net::NetServerConfig config;
        config.Port = port;
        config.Compatibility = compatibility;
        const char* serverName = args.Value("--server-name");
        config.ServerName = serverName ? serverName : m_ProjectName + " server";
        if (const char* maxClients = args.Value("--max-clients"))
            config.MaxClients = static_cast<uint32_t>((std::max)(1l, std::strtol(maxClients, nullptr, 10)));
        m_Server = std::make_unique<Net::NetServer>(std::move(config));
        m_Server->FillWelcome = [this](Net::NetWelcome& welcome)
        {
            welcome.ServerTick = m_Scene ? m_Scene->GetTick() : 0;
            welcome.TickRate = static_cast<uint16_t>(Scene::TickRate);
            welcome.SceneHandle = static_cast<uint64_t>(m_SceneHandle);
        };
        if (!m_Server->AddTransport(Net::CreateTransport(false)))
        {
            NOX_CORE_ERROR("NoxRuntime: the server cannot accept clients (port {}); choose another one with --port=<number>", port);
            m_Server.reset();
            if (m_NetRole == NetRole::DedicatedServer)
                Quit(); // a dedicated server without its port has nothing to do
            return;
        }
        if (emulation.IsActive())
        {
            m_Server->SetEmulation(emulation);
            NOX_CORE_INFO("Net: network emulation {}", Describe(emulation));
        }
        m_Replication = std::make_unique<Net::NetReplication>(*m_Server);
        if (const char* rate = args.Value("--net-rate"))
            m_Replication->SetSnapshotRate(static_cast<float>(std::atof(rate)));
        NOX_CORE_INFO("Net: replicating at {:.0f} snapshots a second", m_Replication->GetSnapshotRate());
        NOX_CORE_INFO("Net: content checksum {:016x} ('{}', network protocol {})", compatibility.ContentChecksum, compatibility.ProjectName,
                      compatibility.Protocol);
        UpdateWindowTitle();
    }

    void RuntimeLayer::UpdateNetworking(float ts)
    {
        if (m_Server)
            m_Server->Update();
        if (m_Client)
            m_Client->Update();

        // The title once a second (the ping changes); a connected client's status line every 10 s.
        m_TitleTimer += ts;
        if (m_TitleTimer >= 1.0f)
        {
            m_TitleTimer = 0.0f;
            UpdateWindowTitle();
        }
        if (m_Client && m_Client->IsConnected())
        {
            m_ClientStatusTimer += ts;
            if (m_ClientStatusTimer >= 10.0f)
            {
                m_ClientStatusTimer = 0.0f;
                const Net::ConnectionStats stats = m_Client->GetStats();
                NOX_CORE_INFO("Client status: client {} on '{}', ping {} ms, quality {} / {}, out {:.1f} KB/s, in {:.1f} KB/s",
                              m_Client->GetWelcome().Client, m_Client->GetWelcome().ServerName, stats.PingMs, Quality(stats.QualityLocal),
                              Quality(stats.QualityRemote), stats.OutBytesPerSecond / 1024.0f, stats.InBytesPerSecond / 1024.0f);
            }
        }
    }

    void RuntimeLayer::FlushNetworking()
    {
        if (m_Server)
            m_Server->Flush();
        if (m_Client)
            m_Client->Flush();
    }

    void RuntimeLayer::OnJoinedServer(const Net::NetWelcome& welcome)
    {
        // The server's scene (Unreal's travel to the server's map on join): a fresh copy, even when it is the one already playing,
        // so the client starts from the level as it was saved, like the server did.
        const AssetHandle scene(welcome.SceneHandle);
        if (welcome.SceneHandle == 0 || !AssetManager::IsAssetHandleValid(scene))
        {
            m_Client->Disconnect(Net::DisconnectReason::ContentMismatch,
                                 std::format("the client does not have the server's scene {}", welcome.SceneHandle));
            UpdateWindowTitle();
            return;
        }
        // Replication first: the server's scene starts as the client's copy of it (proxies), its scripts see Network.IsClient.
        m_Replication = std::make_unique<Net::NetReplication>(*m_Client);
        StartScene(scene);
        m_ClientStatusTimer = 0.0f;
        UpdateWindowTitle();
    }

    void RuntimeLayer::OnLeftServer(Net::DisconnectReason reason, const std::string& text)
    {
        // Back to the start scene (Unreal returns to the default map), standalone; nothing changes when the client never joined.
        const bool wasJoined = m_Replication != nullptr;
        m_Replication.reset();
        if (wasJoined && m_StartSceneHandle != 0)
            StartScene(m_StartSceneHandle);
        UpdateWindowTitle();
    }

    void RuntimeLayer::UpdateWindowTitle()
    {
        if (m_Headless || m_NetRole == NetRole::Standalone)
            return;
        std::string state;
        if (m_Server)
            state = std::format("listen server on port {}, {}/{} clients", m_Server->GetConfig().Port, m_Server->GetClients().size(),
                                m_Server->GetConfig().MaxClients);
        else if (m_Client)
        {
            switch (m_Client->GetState())
            {
            case Net::NetClientState::Connecting:
            case Net::NetClientState::Handshaking:
                state = std::format("connecting to {}...", m_Client->GetServerAddress());
                break;
            case Net::NetClientState::Connected:
                state = std::format("client {} on '{}' ({}), ping {} ms", m_Client->GetWelcome().Client, m_Client->GetWelcome().ServerName,
                                    m_Client->GetServerAddress(), m_Client->GetStats().PingMs);
                break;
            case Net::NetClientState::Disconnected:
                state = std::format("disconnected: {} ({})", Net::ToString(m_Client->GetLastDisconnectReason()), m_Client->GetLastDisconnectText());
                break;
            }
        }
        else
            state = "server not running (see the log)";
        if (const auto window = Application::Get().getWindow())
            SDL_SetWindowTitle(window->getHandle(), std::format("{} - {}", m_ProjectName, state).c_str());
    }
}
