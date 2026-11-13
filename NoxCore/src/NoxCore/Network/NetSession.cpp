#include "NetSession.h"

#include <algorithm>
#include <format>

#include "NetClient.h"
#include "NetProject.h"
#include "NetReplication.h"
#include "NetServer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

namespace Nox::Net
{
    namespace
    {
        // A connection quality (0..1) as a percentage; the transport reports -1 until it has measured it.
        std::string Quality(float quality)
        {
            return quality < 0.0f ? std::string("unknown") : std::format("{:.0f} %", quality * 100.0f);
        }

        std::string DescribeEmulation(const NetEmulation& emulation)
        {
            return std::format("{:.0f} ms lag, {:.0f} ms jitter, {:.1f} % loss (each direction)", emulation.LagMs, emulation.JitterMs,
                               emulation.LossPercent);
        }

        constexpr double RetryIntervalSeconds = 0.5;

        TConsoleVariable<bool>* s_DebugTrace = nullptr;
        TConsoleVariable<bool>* s_LagCompensation = nullptr;
    }

    void RegisterConsoleVariables()
    {
        if (s_DebugTrace)
            return;
        Console& console = Console::Get();
        s_DebugTrace = console.RegisterVariable<bool>("net.DebugTrace",
            "The network debug trace in the log: uneven inputs, characters faster than they walk, animation sent / applied, corrections, "
            "long frames (--net-debug)", false);
        s_DebugTrace->OnChanged([](ConsoleVariable&)
        {
            if (NetReplication* replication = NetReplication::Get())
                replication->SetDebugTrace(s_DebugTrace->Get());
        });
        s_LagCompensation = console.RegisterVariable<bool>("net.LagCompensation",
            "Server: shots are checked against where the targets were on the shooter's screen (0: where they are now, to compare)", true);
        s_LagCompensation->OnChanged([](ConsoleVariable&)
        {
            if (NetReplication* replication = NetReplication::Get(); replication && replication->GetMode() == NetReplication::Mode::Server)
                replication->SetLagCompensation(s_LagCompensation->Get());
        });
    }

    NetSession::NetSession(NetSessionConfig config)
        : m_Config(std::move(config))
    {
        RegisterConsoleVariables();
        if (m_Config.DebugTrace)
            s_DebugTrace->Set(true); // (--net-debug: the variable shows it, and turning it off works)
        RegisterConsoleCommands();
    }

    void NetSession::ApplyConsoleVariables()
    {
        if (!m_Replication)
            return;
        if (s_DebugTrace->Get())
            m_Replication->SetDebugTrace(true);
        if (m_Replication->GetMode() == NetReplication::Mode::Server)
            m_Replication->SetLagCompensation(s_LagCompensation->Get());
    }

    void NetSession::RegisterConsoleCommands()
    {
        Console& console = Console::Get();
        m_ConsoleCommands.push_back(console.RegisterCommand({ "net.Status", "net.Status", "The network session: role, port or server, clients, ping",
            ConsoleAccess::Read,
            [this](const std::vector<std::string>&, ConsoleOutput& output)
            {
                output.Info(Describe());
                if (m_Server)
                    for (const NetClientInfo& client : m_Server->GetClients())
                        output.Info(std::format("  client {} '{}' from {}", client.Id, client.Name, client.Address));
            } }));
        m_ConsoleCommands.push_back(console.RegisterCommand({ "net.Emulation", "net.Emulation off|average|bad|<lag ms> <jitter ms> <loss %>",
            "Simulated lag, jitter and packet loss on this machine's connections (each direction)", ConsoleAccess::Write,
            [this](const std::vector<std::string>& arguments, ConsoleOutput& output)
            {
                NetEmulation emulation;
                if (arguments.size() >= 3)
                {
                    emulation.LagMs = std::max(0.0f, static_cast<float>(std::atof(arguments[0].c_str())));
                    emulation.JitterMs = std::max(0.0f, static_cast<float>(std::atof(arguments[1].c_str())));
                    emulation.LossPercent = std::clamp(static_cast<float>(std::atof(arguments[2].c_str())), 0.0f, 100.0f);
                }
                else if (arguments.size() != 1 || !NetEmulation::FromPreset(arguments[0], emulation))
                {
                    output.Error("net.Emulation off|average|bad|<lag ms> <jitter ms> <loss %>");
                    return;
                }
                if (m_Server)
                    m_Server->SetEmulation(emulation);
                if (m_Client)
                    m_Client->SetEmulation(emulation);
                output.Info(std::format("Network emulation: {}", emulation.IsActive() ? DescribeEmulation(emulation) : std::string("off")));
            } }));
    }

    NetSession::~NetSession()
    {
        // The replication first (it holds the session's callbacks), then the connections: the clients hear "server shut down" / the
        // server hears "left" while the host's game still exists.
        m_Replication.reset();
        m_Client.reset();
        m_Server.reset();
    }

    bool NetSession::Start()
    {
        const NetCompatibility compatibility = MakeProjectCompatibility();
        m_StartedAt = NetNow();
        if (m_Config.Role == SessionRole::Client)
        {
            NetClientConfig config;
            config.Compatibility = compatibility;
            config.PlayerName = m_Config.PlayerName.empty() ? std::string("Player") : m_Config.PlayerName;
            m_Client = std::make_unique<NetClient>(std::move(config));
            m_Client->SetEmulation(m_Config.Emulation);
            m_Client->OnConnected = [this](const NetWelcome& welcome) { OnConnected(welcome); };
            m_Client->OnDisconnected = [this](DisconnectReason reason, const std::string& text) { OnDisconnected(reason, text); };
            if (m_Config.Emulation.IsActive())
                NOX_CORE_INFO("Net: network emulation {}", DescribeEmulation(m_Config.Emulation));
            return m_Client->Connect(m_Config.Address);
        }

        NetServerConfig config;
        config.Port = m_Config.Port;
        config.Compatibility = compatibility;
        config.ServerName = !m_Config.ServerName.empty() ? m_Config.ServerName
                          : Project::GetActive() ? Project::GetActive()->GetConfig().Name + " server" : std::string("Nox server");
        if (m_Config.MaxClients > 0)
            config.MaxClients = m_Config.MaxClients;
        m_Server = std::make_unique<NetServer>(std::move(config));
        m_Server->FillWelcome = [this](NetWelcome& welcome)
        {
            if (FillWelcome)
                FillWelcome(welcome);
        };
        if (!m_Server->AddTransport(CreateTransport(false)))
        {
            NOX_CORE_ERROR("Net: the server cannot accept clients (port {} is taken or cannot be opened)", m_Config.Port);
            m_Server.reset();
            return false;
        }
        if (m_Config.Emulation.IsActive())
        {
            m_Server->SetEmulation(m_Config.Emulation);
            NOX_CORE_INFO("Net: network emulation {}", DescribeEmulation(m_Config.Emulation));
        }
        m_Replication = std::make_unique<NetReplication>(*m_Server);
        ApplyConsoleVariables();
        if (m_Config.SnapshotRate > 0.0f)
            m_Replication->SetSnapshotRate(m_Config.SnapshotRate);
        if (m_Config.ClientRate > 0.0f)
            m_Replication->SetClientRate(m_Config.ClientRate);
        NOX_CORE_INFO("Net: replicating at {:.0f} snapshots a second, up to {:.0f} KB/s per client (--net-rate, --net-client-rate)",
                      m_Replication->GetSnapshotRate(), m_Replication->GetClientRate() / 1024.0f);
        NOX_CORE_INFO("Net: content checksum {:016x} ('{}', network protocol {})", compatibility.ContentChecksum, compatibility.ProjectName,
                      compatibility.Protocol);
        return true;
    }

    void NetSession::OnConnected(const NetWelcome& welcome)
    {
        // Replication first: the server's scene starts as this client's copy of it (proxies), its scripts see Network.IsClient.
        m_Replication = std::make_unique<NetReplication>(*m_Client);
        ApplyConsoleVariables();
        const AssetHandle scene(welcome.SceneHandle);
        if (welcome.SceneHandle == 0 || !OnJoined || !OnJoined(scene))
        {
            m_Replication.reset();
            m_Client->Disconnect(DisconnectReason::ContentMismatch,
                                 std::format("the client does not have the server's scene {}", welcome.SceneHandle));
            return;
        }
        m_EverJoined = true;
        m_StatusTimer = 0.0f;
    }

    void NetSession::OnDisconnected(DisconnectReason reason, const std::string& text)
    {
        const bool wasJoined = m_Replication != nullptr;
        m_Replication.reset();
        // A first connection that failed while the server may still be starting: tried again (the editor's multiplayer Play launches
        // the server and its clients together).
        if (!m_EverJoined && reason == DisconnectReason::ConnectionFailed && NetNow() - m_StartedAt < m_Config.ConnectRetrySeconds)
        {
            m_RetryAt = NetNow() + RetryIntervalSeconds;
            return;
        }
        if (OnLeft)
            OnLeft(reason, text, wasJoined);
    }

    void NetSession::Receive(float deltaTime)
    {
        if (m_Server)
            m_Server->Update();
        if (m_Client)
        {
            if (m_RetryAt > 0.0 && NetNow() >= m_RetryAt)
            {
                m_RetryAt = 0.0;
                m_Client->Connect(m_Config.Address);
            }
            m_Client->Update();
        }
        // Others drawn between the snapshots just received (before the scene's update: this frame's draw uses it).
        if (m_Replication)
        {
            m_Replication->TraceFrame(deltaTime);
            m_Replication->Interpolate();
        }

        // A joined client's status line every 10 s.
        if (m_Client && m_Client->IsConnected())
        {
            m_StatusTimer += deltaTime;
            if (m_StatusTimer >= 10.0f)
            {
                m_StatusTimer = 0.0f;
                const ConnectionStats stats = m_Client->GetStats();
                NOX_CORE_INFO("Client status: client {} on '{}', ping {} ms, quality {} / {}, out {:.1f} KB/s, in {:.1f} KB/s, "
                              "others drawn {:.0f} ms in the past, {} prediction corrections so far, {} replicated entities relevant here "
                              "(snapshots {:.1f} KB/s)",
                              m_Client->GetWelcome().Client, m_Client->GetWelcome().ServerName, stats.PingMs, Quality(stats.QualityLocal),
                              Quality(stats.QualityRemote), stats.OutBytesPerSecond / 1024.0f, stats.InBytesPerSecond / 1024.0f,
                              m_Replication ? m_Replication->GetInterpolationDelayMs() : 0.0f, m_Replication ? m_Replication->GetCorrections() : 0,
                              m_Replication ? m_Replication->GetHeldCount() : 0,
                              m_Replication ? m_Replication->GetReceivedBytesPerSecond() / 1024.0f : 0.0f);
            }
        }
    }

    void NetSession::AfterScene(float deltaTime)
    {
        // The server's snapshots of this frame's results; the client's state that waited for scripts.
        if (m_Replication)
            m_Replication->Update(deltaTime);
    }

    void NetSession::Send()
    {
        if (m_Server)
            m_Server->Flush();
        if (m_Client)
            m_Client->Flush();
    }

    std::string NetSession::Describe() const
    {
        if (m_Server)
            return std::format("{} on port {}, {}/{} clients", m_Config.Role == SessionRole::DedicatedServer ? "dedicated server" : "listen server",
                               m_Server->GetConfig().Port, m_Server->GetClients().size(), m_Server->GetConfig().MaxClients);
        if (!m_Client)
            return "server not running (see the log)";
        if (m_RetryAt > 0.0)
            return std::format("waiting for the server at {}...", m_Client->GetServerAddress());
        switch (m_Client->GetState())
        {
        case NetClientState::Connecting:
        case NetClientState::Handshaking:
            return std::format("connecting to {}...", m_Client->GetServerAddress());
        case NetClientState::Connected:
            return std::format("client {} on '{}' ({}), ping {} ms", m_Client->GetWelcome().Client, m_Client->GetWelcome().ServerName,
                               m_Client->GetServerAddress(), m_Client->GetStats().PingMs);
        case NetClientState::Disconnected:
            break;
        }
        return std::format("disconnected: {} ({})", ToString(m_Client->GetLastDisconnectReason()), m_Client->GetLastDisconnectText());
    }
}
