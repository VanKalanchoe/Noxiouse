#include "NetServer.h"

#include <algorithm>
#include <format>

#include "NoxCore/Core/Log.h"

namespace Nox::Net
{
    NetServer::NetServer(NetServerConfig config)
        : m_Config(std::move(config))
    {
    }

    NetServer::~NetServer()
    {
        DisconnectAll(DisconnectReason::ServerShutdown, "the server is shutting down");
        Flush();
        m_Transports.clear(); // (their destructors give the closes a moment to leave)
    }

    bool NetServer::AddTransport(std::unique_ptr<NetTransport> transport)
    {
        if (!transport || !transport->Listen(m_Config.Port))
            return false;
        NOX_CORE_INFO("Net: server '{}' listening on port {} ({}), up to {} clients", m_Config.ServerName, m_Config.Port,
                      transport->GetName(), m_Config.MaxClients);
        m_Transports.push_back(std::move(transport));
        return true;
    }

    void NetServer::Update()
    {
        for (m_Polling = 0; m_Polling < m_Transports.size(); ++m_Polling)
            m_Transports[m_Polling]->Poll(*this);

        // Connections that never finished the handshake.
        const double now = NetNow();
        std::vector<uint64_t> late;
        for (const auto& [key, peer] : m_Peers)
        {
            if (!peer.Joined && now - peer.ConnectedAt > m_Config.HandshakeTimeoutSeconds)
                late.push_back(key);
        }
        for (uint64_t key : late)
            Drop(key, DisconnectReason::HandshakeTimeout, std::format("no Hello within {:.0f} s", m_Config.HandshakeTimeoutSeconds));
    }

    void NetServer::Flush()
    {
        for (const std::unique_ptr<NetTransport>& transport : m_Transports)
            transport->Flush();
    }

    void NetServer::DisconnectAll(DisconnectReason reason, std::string_view text)
    {
        std::vector<uint64_t> keys;
        keys.reserve(m_Peers.size());
        for (const auto& [key, _] : m_Peers)
            keys.push_back(key);
        for (uint64_t key : keys)
            Drop(key, reason, text);
    }

    void NetServer::OnTransportConnected(ConnectionHandle connection, bool incoming)
    {
        NetTransport& transport = *m_Transports[m_Polling];
        if (!incoming)
            return;
        const std::string address = transport.GetAddress(connection);
        // Full: refused before the handshake, with the reason (the client shows "server full", not a time-out).
        if (m_Peers.size() >= m_Config.MaxClients)
        {
            NOX_CORE_INFO("Net: refused {}: server full ({}/{})", address, m_Peers.size(), m_Config.MaxClients);
            transport.Close(connection, static_cast<uint32_t>(DisconnectReason::ServerFull),
                            std::format("the server is full ({} players)", m_Config.MaxClients), true);
            return;
        }
        Peer peer;
        peer.Transport = m_Polling;
        peer.Connection = connection;
        peer.ConnectedAt = NetNow();
        peer.Info.Address = address;
        m_Peers.emplace(Key(m_Polling, connection), std::move(peer));
    }

    void NetServer::OnTransportDisconnected(ConnectionHandle connection, const CloseInfo& info)
    {
        auto found = m_Peers.find(Key(m_Polling, connection));
        if (found == m_Peers.end())
            return;
        DisconnectReason reason = DisconnectReason::ConnectionLost;
        if (info.AppReason > 0 && info.AppReason < static_cast<uint32_t>(DisconnectReason::Count))
            reason = static_cast<DisconnectReason>(info.AppReason);
        else if (info.TimedOut)
            reason = DisconnectReason::TimedOut;
        const Peer peer = std::move(found->second);
        m_Peers.erase(found);
        if (!peer.Joined)
        {
            NOX_CORE_INFO("Net: {} disconnected during the handshake: {} ({})", peer.Info.Address, ToString(reason), info.Text);
            return;
        }
        RebuildClientList();
        NOX_CORE_INFO("Net: client {} '{}' left: {} ({}) -- {}/{} clients", peer.Info.Id, peer.Info.Name, ToString(reason), info.Text,
                      m_Clients.size(), m_Config.MaxClients);
        if (OnClientLeft)
            OnClientLeft(peer.Info, reason, info.Text);
    }

    void NetServer::OnTransportMessage(ConnectionHandle connection, std::span<const uint8_t> message)
    {
        const uint64_t key = Key(m_Polling, connection);
        auto found = m_Peers.find(key);
        if (found == m_Peers.end())
            return;
        if (message.empty())
        {
            Drop(key, DisconnectReason::ProtocolError, "an empty message");
            return;
        }
        Peer& peer = found->second;
        const uint8_t type = message[0];
        NetReader reader(message.subspan(1));
        if (!peer.Joined)
        {
            if (type == static_cast<uint8_t>(MessageType::Hello))
                HandleHello(peer, reader);
            else
                Drop(key, DisconnectReason::ProtocolError, std::format("message type {} before the handshake", type));
            return;
        }
        if (type < static_cast<uint8_t>(MessageType::FirstGameMessage))
        {
            Drop(key, DisconnectReason::ProtocolError, std::format("unexpected session message {}", type));
            return;
        }
        if (OnMessage)
        {
            const NetClientInfo info = peer.Info; // (the handler may kick this client)
            OnMessage(info, type, reader);
        }
    }

    void NetServer::HandleHello(Peer& peer, NetReader& reader)
    {
        const uint64_t key = Key(peer.Transport, peer.Connection);
        NetHello hello;
        if (!ReadHello(reader, hello))
        {
            Drop(key, DisconnectReason::ProtocolError, hello.Magic != ProtocolMagic ? "not a Nox client" : "a malformed Hello");
            return;
        }
        const NetCompatibility& mine = m_Config.Compatibility;
        const NetCompatibility& theirs = hello.Compatibility;
        if (theirs.Protocol != mine.Protocol)
        {
            Drop(key, DisconnectReason::VersionMismatch,
                 std::format("the server speaks network protocol {}, the client {} (update the older one)", mine.Protocol, theirs.Protocol));
            return;
        }
        if (theirs.ProjectName != mine.ProjectName)
        {
            Drop(key, DisconnectReason::ContentMismatch, std::format("the server plays '{}', the client '{}'", mine.ProjectName, theirs.ProjectName));
            return;
        }
        if (theirs.ContentChecksum != mine.ContentChecksum)
        {
            Drop(key, DisconnectReason::ContentMismatch,
                 std::format("the client's game content (scripts) differs from the server's (checksum {:016x}, the server's {:016x})",
                             theirs.ContentChecksum, mine.ContentChecksum));
            return;
        }

        peer.Joined = true;
        peer.Info.Id = m_NextClientId++;
        peer.Info.Name = hello.PlayerName.empty() ? std::format("Player {}", peer.Info.Id) : hello.PlayerName;
        peer.Info.JoinedAt = NetNow();

        NetWelcome welcome;
        welcome.Client = peer.Info.Id;
        welcome.ServerName = m_Config.ServerName;
        welcome.MaxClients = static_cast<uint16_t>(std::min<uint32_t>(m_Config.MaxClients, UINT16_MAX));
        if (FillWelcome)
            FillWelcome(welcome);
        NetWriter writer(64);
        WriteWelcome(writer, welcome);
        m_Transports[peer.Transport]->Send(peer.Connection, writer.GetData(), Delivery::Reliable);

        RebuildClientList();
        const NetClientInfo info = peer.Info; // (the handlers may kick it: the peer is not used after this)
        NOX_CORE_INFO("Net: client {} '{}' joined from {} -- {}/{} clients", info.Id, info.Name, info.Address, m_Clients.size(),
                      m_Config.MaxClients);
        if (OnClientJoined)
            OnClientJoined(info);
    }

    void NetServer::Drop(uint64_t key, DisconnectReason reason, std::string_view text)
    {
        auto found = m_Peers.find(key);
        if (found == m_Peers.end())
            return;
        const Peer peer = std::move(found->second);
        m_Peers.erase(found);
        m_Transports[peer.Transport]->Close(peer.Connection, static_cast<uint32_t>(reason), text, true);
        if (!peer.Joined)
        {
            NOX_CORE_INFO("Net: refused {}: {} ({})", peer.Info.Address, ToString(reason), text);
            return;
        }
        RebuildClientList();
        NOX_CORE_INFO("Net: client {} '{}' disconnected: {} ({}) -- {}/{} clients", peer.Info.Id, peer.Info.Name, ToString(reason), text,
                      m_Clients.size(), m_Config.MaxClients);
        if (OnClientLeft)
            OnClientLeft(peer.Info, reason, std::string(text));
    }

    bool NetServer::Send(ClientId client, std::span<const uint8_t> message, Delivery delivery)
    {
        if (message.empty() || message[0] < static_cast<uint8_t>(MessageType::FirstGameMessage))
            return false;
        const Peer* peer = FindPeer(client);
        return peer && m_Transports[peer->Transport]->Send(peer->Connection, message, delivery);
    }

    void NetServer::Broadcast(std::span<const uint8_t> message, Delivery delivery)
    {
        if (message.empty() || message[0] < static_cast<uint8_t>(MessageType::FirstGameMessage))
            return;
        for (const auto& [_, peer] : m_Peers)
        {
            if (peer.Joined)
                m_Transports[peer.Transport]->Send(peer.Connection, message, delivery);
        }
    }

    bool NetServer::Kick(ClientId client, std::string_view text)
    {
        const Peer* peer = FindPeer(client);
        if (!peer)
            return false;
        Drop(Key(peer->Transport, peer->Connection), DisconnectReason::Kicked, text.empty() ? std::string_view("kicked by the server") : text);
        return true;
    }

    const NetClientInfo* NetServer::FindClient(ClientId client) const
    {
        const Peer* peer = FindPeer(client);
        return peer ? &peer->Info : nullptr;
    }

    ConnectionStats NetServer::GetStats(ClientId client) const
    {
        const Peer* peer = FindPeer(client);
        return peer ? m_Transports[peer->Transport]->GetStats(peer->Connection) : ConnectionStats{};
    }

    void NetServer::SetEmulation(const NetEmulation& emulation)
    {
        for (const std::unique_ptr<NetTransport>& transport : m_Transports)
            transport->SetEmulation(emulation);
    }

    NetEmulation NetServer::GetEmulation() const
    {
        return m_Transports.empty() ? NetEmulation{} : m_Transports.front()->GetEmulation();
    }

    NetServer::Peer* NetServer::FindPeer(ClientId client)
    {
        for (auto& [_, peer] : m_Peers)
        {
            if (peer.Joined && peer.Info.Id == client)
                return &peer;
        }
        return nullptr;
    }

    const NetServer::Peer* NetServer::FindPeer(ClientId client) const
    {
        return const_cast<NetServer*>(this)->FindPeer(client);
    }

    void NetServer::RebuildClientList()
    {
        m_Clients.clear();
        for (const auto& [_, peer] : m_Peers)
        {
            if (peer.Joined)
                m_Clients.push_back(peer.Info);
        }
        std::sort(m_Clients.begin(), m_Clients.end(), [](const NetClientInfo& a, const NetClientInfo& b) { return a.Id < b.Id; });
    }
}
