#pragma once
#include "NetBuffer.h"
#include "NetProtocol.h"
#include "NetTransport.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The server's side of the sessions (docs/Networking_Plan_2026-11.md, NET1; Unreal's UNetDriver + the login flow of
// AGameModeBase::PreLogin/Login): accepts connections on its transports (a dedicated server: GameNetworkingSockets on a UDP port;
// a listen server can add the loopback one for clients in its own process), runs the handshake (NetProtocol.h), keeps the joined
// clients, and hands their game messages up. Everything happens in Update (call it once a frame, before the simulation) and
// Flush (after the frame's sends).
namespace Nox::Net
{
    struct NetServerConfig
    {
        uint16_t Port = DefaultPort;
        uint32_t MaxClients = 16;
        float HandshakeTimeoutSeconds = 5.0f;
        std::string ServerName = "Nox server";
        NetCompatibility Compatibility; // what a client must match
        TransportConfig Transport;
    };

    struct NetClientInfo
    {
        ClientId Id = InvalidClientId;
        std::string Name;
        std::string Address;
        double JoinedAt = 0.0; // seconds (steady clock)
    };

    class NetServer final : private TransportListener
    {
    public:
        explicit NetServer(NetServerConfig config);
        ~NetServer() override; // Stop(ServerShutdown)

        // Adds a transport and listens on it (the config's port; a loopback transport: "loopback:<port>"). False if it can't listen.
        bool AddTransport(std::unique_ptr<NetTransport> transport);
        // Polls the transports: connections, handshakes, messages, time-outs.
        void Update();
        // Sends what this frame queued.
        void Flush();
        // Disconnects every client with this reason (the transports stay; Update can accept again).
        void DisconnectAll(DisconnectReason reason, std::string_view text);

        // A game message (its first byte a type >= MessageType::FirstGameMessage) to one client / to all joined ones.
        bool Send(ClientId client, std::span<const uint8_t> message, Delivery delivery);
        void Broadcast(std::span<const uint8_t> message, Delivery delivery);
        // Removes a client; it receives DisconnectReason::Kicked and the text.
        bool Kick(ClientId client, std::string_view text);

        const std::vector<NetClientInfo>& GetClients() const { return m_Clients; }
        const NetClientInfo* FindClient(ClientId client) const;
        ConnectionStats GetStats(ClientId client) const;
        const NetServerConfig& GetConfig() const { return m_Config; }
        size_t GetTransportCount() const { return m_Transports.size(); }
        void SetEmulation(const NetEmulation& emulation);
        NetEmulation GetEmulation() const;

        // The game's part of the handshake: fills the Welcome's server tick and scene (the client id, tick rate default, server name
        // and limits are the server's).
        std::function<void(NetWelcome&)> FillWelcome;
        std::function<void(const NetClientInfo&)> OnClientJoined;
        std::function<void(const NetClientInfo&, DisconnectReason, const std::string&)> OnClientLeft;
        // A game message from a joined client (type >= FirstGameMessage; the reader is after the type byte).
        std::function<void(const NetClientInfo&, uint8_t, NetReader&)> OnMessage;

    private:
        struct Peer
        {
            size_t Transport = 0;
            ConnectionHandle Connection = InvalidConnection;
            bool Joined = false;
            double ConnectedAt = 0.0;
            NetClientInfo Info;
        };
        static uint64_t Key(size_t transport, ConnectionHandle connection) { return (static_cast<uint64_t>(transport) << 32) | connection; }

        void OnTransportConnected(ConnectionHandle connection, bool incoming) override;
        void OnTransportDisconnected(ConnectionHandle connection, const CloseInfo& info) override;
        void OnTransportMessage(ConnectionHandle connection, std::span<const uint8_t> message) override;

        void HandleHello(Peer& peer, NetReader& reader);
        // Closes a peer's connection with a reason; a joined one leaves (OnClientLeft).
        void Drop(uint64_t key, DisconnectReason reason, std::string_view text);
        Peer* FindPeer(ClientId client);
        const Peer* FindPeer(ClientId client) const;
        void RebuildClientList();

        NetServerConfig m_Config;
        std::vector<std::unique_ptr<NetTransport>> m_Transports;
        size_t m_Polling = 0; // the transport being polled (its callbacks don't say)
        std::unordered_map<uint64_t, Peer> m_Peers;
        std::vector<NetClientInfo> m_Clients; // joined, in join order
        ClientId m_NextClientId = 1;
    };
}
