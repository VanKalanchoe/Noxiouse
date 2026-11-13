#pragma once
#include "NetTransport.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

// The UDP transport (docs/Networking_Plan_2026-11.md, NET1): Valve's open-source GameNetworkingSockets (BSD-3, vcpkg
// "gamenetworkingsockets") -- reliable / unreliable messages over UDP, fragmentation, AES-GCM encryption, keep-alives and
// time-outs, congestion control, connection quality. Addresses: "192.168.1.5:7777", "[::1]:7777", "localhost:7777", a host name
// ("myserver.lan:7777"); without a port, 7777. The library is started with the first transport and stopped with the last.
// Its packet emulation (fake lag / jitter / loss) is process-wide: SetEmulation on any GNS transport sets it for all of them.
struct SteamNetConnectionStatusChangedCallback_t;

namespace Nox::Net
{
    class GnsTransport final : public NetTransport
    {
    public:
        explicit GnsTransport(const TransportConfig& config = {});
        ~GnsTransport() override;

        // False if the library could not start (logged): the transport then refuses every connection.
        bool IsValid() const { return m_PollGroup != 0; }

        const char* GetName() const override { return "GameNetworkingSockets"; }
        bool Listen(uint16_t port) override;
        ConnectionHandle Connect(std::string_view address) override;
        void Close(ConnectionHandle connection, uint32_t appReason, std::string_view text, bool linger) override;
        bool Send(ConnectionHandle connection, std::span<const uint8_t> message, Delivery delivery) override;
        void Flush() override;
        void Poll(TransportListener& listener) override;
        ConnectionStats GetStats(ConnectionHandle connection) const override;
        std::string GetAddress(ConnectionHandle connection) const override;
        void SetEmulation(const NetEmulation& emulation) override;
        NetEmulation GetEmulation() const override;

        // The library's status callback (any connection of the process) hands each change to the transport that owns it.
        static void OnStatusChanged(SteamNetConnectionStatusChangedCallback_t* change);

    private:
        struct Connection
        {
            bool Incoming = false;
            bool Established = false;
            double LastReceived = 0.0; // seconds (steady clock): the last message from the peer
            double LastProbe = 0.0;    // the last keep-alive probe this side sent
        };
        // Silence probes: GameNetworkingSockets only starts checking a peer that sends nothing reliable after 10 s without
        // hearing from it; a reliable empty message after 1 s of silence makes it check now, so a dead peer is found within
        // TimeoutSeconds whatever the game sends (empty messages are the transport's own: never delivered).
        void SendProbes();
        void HandleStatusChange(const SteamNetConnectionStatusChangedCallback_t& change, TransportListener& listener);
        void DeliverMessagesOf(ConnectionHandle connection, TransportListener& listener);

        TransportConfig m_Config;
        uint32_t m_ListenSocket = 0; // HSteamListenSocket
        uint32_t m_PollGroup = 0;    // HSteamNetPollGroup: every connection's messages, received in one call
        std::unordered_map<ConnectionHandle, Connection> m_Connections;
        // Status changes the library reported for this transport's connections, handled at the next poll.
        std::mutex m_ChangesMutex;
        std::vector<SteamNetConnectionStatusChangedCallback_t> m_Changes;
        bool m_ClosedWithLinger = false; // shutting down waits a moment so the closes still go out
    };
}
