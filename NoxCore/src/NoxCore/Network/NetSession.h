#pragma once
#include "NetProtocol.h"
#include "NetTransport.h"

#include <functional>
#include <memory>
#include <string>

#include "NoxCore/Asset/Asset.h"

// A game session (docs/Networking_Plan_2026-11.md, NET6; Unreal's net driver + travel on join): a dedicated or listen server, or a
// client, with the scene's replication -- what NoxRuntime and the editor's multiplayer Play both run. The host owns the scene; the
// session tells it which scene to play (a client travels to the server's on join, and back when the session ends) and is called around
// the host's frame: Receive (joins, leaves, snapshots; others interpolated) -> the scene's update -> AfterScene (snapshots) -> Send.
namespace Nox::Net
{
    class NetServer;
    class NetClient;
    class NetReplication;

    enum class SessionRole : uint8_t { DedicatedServer, ListenServer, Client };

    struct NetSessionConfig
    {
        SessionRole Role = SessionRole::ListenServer;
        uint16_t Port = DefaultPort;      // servers
        std::string Address;              // clients: host[:port]
        std::string PlayerName = "Player";
        std::string ServerName;           // servers (empty: the project's name + " server")
        uint32_t MaxClients = 0;          // servers (0: the default)
        NetEmulation Emulation;
        float SnapshotRate = 0.0f;        // servers (0: the default)
        float ClientRate = 0.0f;          // servers (0: the default)
        // Clients: a connection that fails before ever joining is tried again for this long (a server started with it is still
        // loading: the editor's multiplayer Play). 0: once.
        double ConnectRetrySeconds = 0.0;
    };

    class NetSession
    {
    public:
        explicit NetSession(NetSessionConfig config);
        ~NetSession(); // a server tells its clients it shut down, a client tells the server it left
        NetSession(const NetSession&) = delete;
        NetSession& operator=(const NetSession&) = delete;

        // Server: listens (false: the port cannot be opened -- logged). Client: starts connecting (false: not even that).
        bool Start();

        // Server: the Welcome a joining client gets (the host's scene and tick).
        std::function<void(NetWelcome&)> FillWelcome;
        // Client: joined -- the host starts the server's scene now (the replication exists already: hand it the scene before its
        // scripts start). False: this client does not have that scene (the session is left: content mismatch).
        std::function<bool(AssetHandle scene)> OnJoined;
        // Client: the session ended. `wasJoined`: it had joined (the host goes back to where it was before).
        std::function<void(DisconnectReason reason, const std::string& text, bool wasJoined)> OnLeft;

        void Receive(float deltaTime);
        void AfterScene(float deltaTime);
        void Send();

        SessionRole GetRole() const { return m_Config.Role; }
        const NetSessionConfig& GetConfig() const { return m_Config; }
        bool IsServer() const { return m_Config.Role != SessionRole::Client; }
        NetServer* GetServer() const { return m_Server.get(); }
        NetClient* GetClient() const { return m_Client.get(); }
        // A server's from Start; a client's while it is joined.
        NetReplication* GetReplication() const { return m_Replication.get(); }
        // One line for a window title or a status bar ("listen server on port 7777, 2/16 clients", "client 3 on ..., ping 12 ms").
        std::string Describe() const;

    private:
        void Connect();
        void OnConnected(const NetWelcome& welcome);
        void OnDisconnected(DisconnectReason reason, const std::string& text);

        NetSessionConfig m_Config;
        std::unique_ptr<NetServer> m_Server;
        std::unique_ptr<NetClient> m_Client;
        std::unique_ptr<NetReplication> m_Replication;
        double m_StartedAt = 0.0;
        double m_RetryAt = 0.0;   // a failed first connection tried again then (0: no retry pending)
        bool m_EverJoined = false;
        float m_StatusTimer = 0.0f;
    };
}
