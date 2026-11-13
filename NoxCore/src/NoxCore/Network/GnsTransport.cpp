#include "GnsTransport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <thread>

// Host name resolution (getaddrinfo): Winsock before anything that could bring windows.h (winsock2.h must come first).
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <sys/socket.h>
#endif

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include "NetProtocol.h"
#include "NoxCore/Core/Log.h"

namespace Nox::Net
{
    namespace
    {
        // The library's state: started with the first transport, stopped with the last; which transport owns which listen socket
        // and connection (the status callback is process-wide).
        std::mutex s_LibraryMutex;
        int s_LibraryUsers = 0;
        std::unordered_map<HSteamListenSocket, GnsTransport*> s_ListenSockets;
        std::unordered_map<HSteamNetConnection, GnsTransport*> s_Connections;
        NetEmulation s_Emulation;

        void DebugOutput(ESteamNetworkingSocketsDebugOutputType type, const char* message)
        {
            if (type <= k_ESteamNetworkingSocketsDebugOutputType_Error)
                NOX_CORE_ERROR("[GameNetworkingSockets] {}", message);
            else if (type <= k_ESteamNetworkingSocketsDebugOutputType_Warning)
                NOX_CORE_WARN("[GameNetworkingSockets] {}", message);
            else
                NOX_CORE_INFO("[GameNetworkingSockets] {}", message);
        }

        bool AcquireLibrary()
        {
            std::lock_guard lock(s_LibraryMutex);
            if (s_LibraryUsers > 0)
            {
                ++s_LibraryUsers;
                return true;
            }
            SteamDatagramErrMsg error{};
            if (!GameNetworkingSockets_Init(nullptr, error))
            {
                NOX_CORE_ERROR("Net: GameNetworkingSockets could not start: {}", error);
                return false;
            }
            SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, DebugOutput);
            SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(&GnsTransport::OnStatusChanged);
            s_LibraryUsers = 1;
            return true;
        }

        void ReleaseLibrary()
        {
            std::lock_guard lock(s_LibraryMutex);
            if (--s_LibraryUsers == 0)
            {
                GameNetworkingSockets_Kill();
                s_Emulation = {};
            }
        }

        // "host[:port]", "[v6]:port", "a.b.c.d[:port]" -> an address; host names resolved (IPv4 preferred).
        bool ResolveAddress(std::string_view text, SteamNetworkingIPAddr& address, std::string& problem)
        {
            address.Clear();
            const std::string input(text);
            if (address.ParseString(input.c_str()))
            {
                if (address.m_port == 0)
                    address.m_port = DefaultPort;
                return true;
            }
            // A host name: "name" or "name:port" (one colon; more would be an IPv6 address, which ParseString handles).
            std::string host = input;
            uint16_t port = DefaultPort;
            const size_t colon = input.rfind(':');
            if (colon != std::string::npos && input.find(':') == colon)
            {
                host = input.substr(0, colon);
                const std::string digits = input.substr(colon + 1);
                char* end = nullptr;
                const long value = std::strtol(digits.c_str(), &end, 10);
                if (digits.empty() || *end != '\0' || value <= 0 || value > 65535)
                {
                    problem = std::format("'{}' is not a port", digits);
                    return false;
                }
                port = static_cast<uint16_t>(value);
            }
            if (host.empty())
            {
                problem = "no host";
                return false;
            }
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* results = nullptr;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || !results)
            {
                problem = std::format("the host name '{}' could not be resolved", host);
                return false;
            }
            const addrinfo* chosen = nullptr;
            for (const addrinfo* result = results; result; result = result->ai_next)
            {
                if (result->ai_family == AF_INET)
                {
                    chosen = result;
                    break;
                }
                if (result->ai_family == AF_INET6 && !chosen)
                    chosen = result;
            }
            bool resolved = false;
            if (chosen && chosen->ai_family == AF_INET)
            {
                const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(chosen->ai_addr);
                address.SetIPv4(ntohl(ipv4->sin_addr.s_addr), port);
                resolved = true;
            }
            else if (chosen && chosen->ai_family == AF_INET6)
            {
                const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(chosen->ai_addr);
                address.SetIPv6(reinterpret_cast<const uint8*>(&ipv6->sin6_addr), port);
                resolved = true;
            }
            freeaddrinfo(results);
            if (!resolved)
                problem = std::format("'{}' has no IPv4 or IPv6 address", host);
            return resolved;
        }

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        constexpr double ProbeAfterSilenceSeconds = 1.0;
        constexpr int AppReasonBase = k_ESteamNetConnectionEnd_App_Min;
        constexpr int AppReasonLimit = k_ESteamNetConnectionEnd_App_Max;
    }

    GnsTransport::GnsTransport(const TransportConfig& config)
        : m_Config(config)
    {
        if (!AcquireLibrary())
            return;
        m_PollGroup = SteamNetworkingSockets()->CreatePollGroup();
        if (m_PollGroup == k_HSteamNetPollGroup_Invalid)
        {
            NOX_CORE_ERROR("Net: GameNetworkingSockets could not create a poll group");
            m_PollGroup = 0;
            ReleaseLibrary();
        }
    }

    GnsTransport::~GnsTransport()
    {
        if (!IsValid())
            return;
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        {
            std::lock_guard lock(s_LibraryMutex);
            for (const auto& [connection, _] : m_Connections)
                s_Connections.erase(connection);
            if (m_ListenSocket != 0)
                s_ListenSockets.erase(m_ListenSocket);
        }
        for (const auto& [connection, _] : m_Connections)
        {
            sockets->CloseConnection(connection, AppReasonBase, "the peer's transport was destroyed", true);
            m_ClosedWithLinger = true;
        }
        m_Connections.clear();

        // Closes are sent by the library's own thread once a lingering connection's reliable messages are through: a moment for
        // them before the listen socket goes -- closing it destroys the connections it accepted at once, without telling their
        // peers (they would only time out). A few frames' worth; a peer whose close is lost times out anyway.
        if (m_ClosedWithLinger)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (std::chrono::steady_clock::now() < until)
            {
                sockets->RunCallbacks(); // (changes of connections no transport owns any more are ignored)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (m_ListenSocket != 0)
            sockets->CloseListenSocket(m_ListenSocket);
        sockets->DestroyPollGroup(m_PollGroup);
        ReleaseLibrary();
    }

    bool GnsTransport::Listen(uint16_t port)
    {
        if (!IsValid())
            return false;
        SteamNetworkingIPAddr address;
        address.Clear(); // every interface, IPv4 and IPv6
        address.m_port = port;
        SteamNetworkingConfigValue_t options[2];
        options[0].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, static_cast<int32_t>(m_Config.ConnectTimeoutSeconds * 1000.0f));
        options[1].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, static_cast<int32_t>(m_Config.TimeoutSeconds * 1000.0f));
        const HSteamListenSocket socket = SteamNetworkingSockets()->CreateListenSocketIP(address, 2, options);
        if (socket == k_HSteamListenSocket_Invalid)
        {
            NOX_CORE_ERROR("Net: cannot listen on UDP port {} (in use by another program?)", port);
            return false;
        }
        std::lock_guard lock(s_LibraryMutex);
        m_ListenSocket = socket;
        s_ListenSockets[socket] = this;
        return true;
    }

    ConnectionHandle GnsTransport::Connect(std::string_view address)
    {
        if (!IsValid())
            return InvalidConnection;
        SteamNetworkingIPAddr remote;
        std::string problem;
        if (!ResolveAddress(address, remote, problem))
        {
            NOX_CORE_ERROR("Net: cannot connect to '{}': {}", address, problem);
            return InvalidConnection;
        }
        SteamNetworkingConfigValue_t options[2];
        options[0].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, static_cast<int32_t>(m_Config.ConnectTimeoutSeconds * 1000.0f));
        options[1].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, static_cast<int32_t>(m_Config.TimeoutSeconds * 1000.0f));
        // The library holds its callbacks until RunCallbacks: the connection is registered before its first change is handled.
        std::lock_guard lock(s_LibraryMutex);
        const HSteamNetConnection connection = SteamNetworkingSockets()->ConnectByIPAddress(remote, 2, options);
        if (connection == k_HSteamNetConnection_Invalid)
        {
            NOX_CORE_ERROR("Net: cannot connect to '{}'", address);
            return InvalidConnection;
        }
        SteamNetworkingSockets()->SetConnectionPollGroup(connection, m_PollGroup);
        s_Connections[connection] = this;
        m_Connections[connection] = Connection{ false, false, Now(), 0.0 };
        return connection;
    }

    void GnsTransport::Close(ConnectionHandle connection, uint32_t appReason, std::string_view text, bool linger)
    {
        auto found = m_Connections.find(connection);
        if (found == m_Connections.end())
            return;
        {
            std::lock_guard lock(s_LibraryMutex);
            s_Connections.erase(connection);
        }
        m_Connections.erase(found);
        const int reason = AppReasonBase + static_cast<int>(std::min<uint32_t>(appReason, AppReasonLimit - AppReasonBase));
        const std::string debug(text.substr(0, k_cchSteamNetworkingMaxConnectionCloseReason - 1));
        SteamNetworkingSockets()->CloseConnection(connection, reason, debug.c_str(), linger);
        m_ClosedWithLinger |= linger;
    }

    bool GnsTransport::Send(ConnectionHandle connection, std::span<const uint8_t> message, Delivery delivery)
    {
        auto found = m_Connections.find(connection);
        if (found == m_Connections.end() || !found->second.Established || message.empty() || message.size() > MaxMessageSize)
            return false;
        const int flags = delivery == Delivery::Reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
        return SteamNetworkingSockets()->SendMessageToConnection(connection, message.data(), static_cast<uint32>(message.size()), flags,
                                                                 nullptr) == k_EResultOK;
    }

    void GnsTransport::Flush()
    {
        for (const auto& [connection, state] : m_Connections)
        {
            if (state.Established)
                SteamNetworkingSockets()->FlushMessagesOnConnection(connection);
        }
    }

    void GnsTransport::OnStatusChanged(SteamNetConnectionStatusChangedCallback_t* change)
    {
        std::lock_guard lock(s_LibraryMutex);
        GnsTransport* owner = nullptr;
        if (auto connection = s_Connections.find(change->m_hConn); connection != s_Connections.end())
            owner = connection->second;
        else if (change->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid)
        {
            if (auto listener = s_ListenSockets.find(change->m_info.m_hListenSocket); listener != s_ListenSockets.end())
                owner = listener->second;
        }
        if (!owner)
            return; // a connection closed locally (its last changes) or of a transport that is gone
        std::lock_guard changesLock(owner->m_ChangesMutex);
        owner->m_Changes.push_back(*change);
    }

    void GnsTransport::Poll(TransportListener& listener)
    {
        if (!IsValid())
            return;
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        sockets->RunCallbacks(); // (also queues the other GNS transports' changes, handled when they poll)

        std::vector<SteamNetConnectionStatusChangedCallback_t> changes;
        {
            std::lock_guard lock(m_ChangesMutex);
            changes.swap(m_Changes);
        }
        for (const SteamNetConnectionStatusChangedCallback_t& change : changes)
            HandleStatusChange(change, listener);

        SteamNetworkingMessage_t* messages[64];
        for (;;)
        {
            const int count = sockets->ReceiveMessagesOnPollGroup(m_PollGroup, messages, 64);
            if (count <= 0)
                break;
            for (int index = 0; index < count; ++index)
            {
                SteamNetworkingMessage_t* message = messages[index];
                // (A connection the listener closed while handling an earlier message gets nothing more.)
                auto found = m_Connections.find(message->m_conn);
                if (found != m_Connections.end() && found->second.Established)
                {
                    found->second.LastReceived = Now();
                    if (message->m_cbSize > 0) // (empty: a silence probe)
                        listener.OnTransportMessage(message->m_conn, std::span(static_cast<const uint8_t*>(message->m_pData),
                                                                               static_cast<size_t>(message->m_cbSize)));
                }
                message->Release();
            }
            if (count < 64)
                break;
        }
        SendProbes();
    }

    void GnsTransport::SendProbes()
    {
        const double now = Now();
        for (auto& [connection, state] : m_Connections)
        {
            if (!state.Established || now - state.LastReceived < ProbeAfterSilenceSeconds || now - state.LastProbe < ProbeAfterSilenceSeconds)
                continue;
            state.LastProbe = now;
            SteamNetworkingSockets()->SendMessageToConnection(connection, nullptr, 0, k_nSteamNetworkingSend_Reliable, nullptr);
        }
    }

    void GnsTransport::DeliverMessagesOf(ConnectionHandle connection, TransportListener& listener)
    {
        SteamNetworkingMessage_t* messages[64];
        for (;;)
        {
            const int count = SteamNetworkingSockets()->ReceiveMessagesOnConnection(connection, messages, 64);
            if (count <= 0)
                break;
            for (int index = 0; index < count; ++index)
            {
                if (m_Connections.contains(connection) && messages[index]->m_cbSize > 0)
                    listener.OnTransportMessage(connection, std::span(static_cast<const uint8_t*>(messages[index]->m_pData),
                                                                      static_cast<size_t>(messages[index]->m_cbSize)));
                messages[index]->Release();
            }
            if (count < 64)
                break;
        }
    }

    void GnsTransport::HandleStatusChange(const SteamNetConnectionStatusChangedCallback_t& change, TransportListener& listener)
    {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        const HSteamNetConnection connection = change.m_hConn;
        switch (change.m_info.m_eState)
        {
        case k_ESteamNetworkingConnectionState_Connecting:
        {
            // Someone connecting to this transport's listen socket: accepted at once (the session's handshake decides).
            if (change.m_info.m_hListenSocket == k_HSteamListenSocket_Invalid || m_Connections.contains(connection))
                break; // (the connection this side started)
            if (sockets->AcceptConnection(connection) != k_EResultOK)
            {
                sockets->CloseConnection(connection, 0, nullptr, false);
                break;
            }
            sockets->SetConnectionPollGroup(connection, m_PollGroup);
            std::lock_guard lock(s_LibraryMutex);
            s_Connections[connection] = this;
            m_Connections[connection] = Connection{ true, false, Now(), 0.0 };
            break;
        }
        case k_ESteamNetworkingConnectionState_Connected:
        {
            auto found = m_Connections.find(connection);
            if (found == m_Connections.end() || found->second.Established)
                break;
            found->second.Established = true;
            found->second.LastReceived = Now();
            listener.OnTransportConnected(connection, found->second.Incoming);
            break;
        }
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
            auto found = m_Connections.find(connection);
            if (found == m_Connections.end())
                break;
            const bool established = found->second.Established;
            const bool incoming = found->second.Incoming;
            // What arrived before the end is still delivered (a last reliable message before the peer closed).
            if (established)
                DeliverMessagesOf(connection, listener);

            CloseInfo info;
            const int reason = change.m_info.m_eEndReason;
            if (reason >= AppReasonBase && reason <= AppReasonLimit)
                info.AppReason = static_cast<uint32_t>(reason - AppReasonBase);
            info.ClosedByPeer = change.m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer;
            info.TimedOut = reason == k_ESteamNetConnectionEnd_Misc_Timeout;
            info.Text = change.m_info.m_szEndDebug;
            {
                std::lock_guard lock(s_LibraryMutex);
                s_Connections.erase(connection);
            }
            m_Connections.erase(connection);
            sockets->CloseConnection(connection, 0, nullptr, false); // frees the handle
            // An incoming connection that ended before it was established was never announced.
            if (established || !incoming)
                listener.OnTransportDisconnected(connection, info);
            break;
        }
        default:
            break;
        }
    }

    ConnectionStats GnsTransport::GetStats(ConnectionHandle connection) const
    {
        ConnectionStats stats;
        if (!m_Connections.contains(connection))
            return stats;
        SteamNetConnectionRealTimeStatus_t status{};
        if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) != k_EResultOK)
            return stats;
        stats.PingMs = status.m_nPing;
        stats.QualityLocal = status.m_flConnectionQualityLocal;
        stats.QualityRemote = status.m_flConnectionQualityRemote;
        stats.OutBytesPerSecond = status.m_flOutBytesPerSec;
        stats.InBytesPerSecond = status.m_flInBytesPerSec;
        stats.OutPacketsPerSecond = status.m_flOutPacketsPerSec;
        stats.InPacketsPerSecond = status.m_flInPacketsPerSec;
        stats.PendingReliableBytes = status.m_cbPendingReliable;
        stats.PendingUnreliableBytes = status.m_cbPendingUnreliable;
        return stats;
    }

    std::string GnsTransport::GetAddress(ConnectionHandle connection) const
    {
        SteamNetConnectionInfo_t info{};
        if (!m_Connections.contains(connection) || !SteamNetworkingSockets()->GetConnectionInfo(connection, &info))
            return {};
        char text[SteamNetworkingIPAddr::k_cchMaxString];
        info.m_addrRemote.ToString(text, sizeof(text), true);
        return text;
    }

    void GnsTransport::SetEmulation(const NetEmulation& emulation)
    {
        if (!IsValid())
            return;
        ISteamNetworkingUtils* utils = SteamNetworkingUtils();
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, static_cast<int32_t>(std::max(emulation.LagMs, 0.0f)));
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, std::clamp(emulation.LossPercent, 0.0f, 100.0f));
        // Jitter: exponential with this mean, capped at 3x (as LoopbackTransport), on every packet.
        const float jitter = std::max(emulation.JitterMs, 0.0f);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, jitter);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, 3.0f * jitter);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, jitter > 0.0f ? 100.0f : 0.0f);
        std::lock_guard lock(s_LibraryMutex);
        s_Emulation = emulation;
    }

    NetEmulation GnsTransport::GetEmulation() const
    {
        std::lock_guard lock(s_LibraryMutex);
        return s_Emulation;
    }
}
