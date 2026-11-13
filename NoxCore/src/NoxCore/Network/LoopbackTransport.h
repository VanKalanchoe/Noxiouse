#pragma once
#include "NetTransport.h"

#include <memory>
#include <random>
#include <unordered_map>

// The in-process transport (docs/Networking_Plan_2026-11.md, NET1): a server and its clients in one process -- the editor's Play
// with clients (NET6), tests -- with no sockets. It behaves like the network one where the session can tell: connections are
// accepted, then established after the emulated delay; reliable messages arrive in order (a "lost" one later, as if resent),
// unreliable ones can be lost or overtaken; keep-alives every second, and a connection that hears nothing for TimeoutSeconds times
// out (emulated 100 % loss). Address: "loopback" or "loopback:<port>" (the port a loopback server listens on, default 7777).
// Everything runs on the thread that polls; transports can be created and destroyed in any order.
namespace Nox::Net
{
    struct LoopbackLink;

    class LoopbackTransport final : public NetTransport
    {
    public:
        explicit LoopbackTransport(const TransportConfig& config = {});
        ~LoopbackTransport() override;

        const char* GetName() const override { return "loopback"; }
        bool Listen(uint16_t port) override;
        ConnectionHandle Connect(std::string_view address) override;
        void Close(ConnectionHandle connection, uint32_t appReason, std::string_view text, bool linger) override;
        bool Send(ConnectionHandle connection, std::span<const uint8_t> message, Delivery delivery) override;
        void Flush() override {}
        void Poll(TransportListener& listener) override;
        ConnectionStats GetStats(ConnectionHandle connection) const override;
        std::string GetAddress(ConnectionHandle connection) const override;
        void SetEmulation(const NetEmulation& emulation) override { m_Emulation = emulation; }
        NetEmulation GetEmulation() const override { return m_Emulation; }

    private:
        friend struct LoopbackLink;
        struct Connection
        {
            std::shared_ptr<LoopbackLink> Link;
            int Side = 0; // this transport's side of the link (0: the side that connected, 1: the listener's)
        };
        // A new incoming link from a client's Connect (called with the hub locked).
        ConnectionHandle AddIncoming(const std::shared_ptr<LoopbackLink>& link);
        // When a packet sent now arrives at the peer under this transport's emulation; false: lost.
        bool ScheduleDelivery(double now, bool reliable, double& deliverAt, double& lastReliable);

        TransportConfig m_Config;
        NetEmulation m_Emulation;
        uint16_t m_ListenPort = 0;
        ConnectionHandle m_NextHandle = 1;
        std::unordered_map<ConnectionHandle, Connection> m_Connections;
        std::mt19937 m_Random;
    };
}
