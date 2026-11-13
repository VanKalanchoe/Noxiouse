#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// The transport (docs/Networking_Plan_2026-11.md, NET1): connections and messages, nothing about the game. Two backends behind
// one interface, chosen by the address: "loopback[:port]" connects inside the process (LoopbackTransport: editor Play with clients,
// tests), anything else is an IP address or host name over UDP (GnsTransport: Valve's GameNetworkingSockets -- reliable and
// unreliable messages, fragmentation, encryption, keep-alives and time-outs). A Steam backend (relays, Steam IDs) is NET6.
//
// Messages are whole: what one Send() sends arrives as one message, or not at all (unreliable). Reliable messages arrive in the
// order they were sent; unreliable ones may arrive late, out of order, or never. A connection is the transport's: it is accepted
// at once, and the session above decides whether to keep it (the handshake). The transport detects a dead peer by itself (no
// packets for TimeoutSeconds) and reports it as a disconnect that timed out.
namespace Nox::Net
{
    using ConnectionHandle = uint32_t;
    constexpr ConnectionHandle InvalidConnection = 0;

    enum class Delivery : uint8_t
    {
        Unreliable, // may be lost or reordered: state that the next update replaces (positions)
        Reliable,   // resent until acknowledged, in order: events (spawns, RPCs, the handshake)
    };

    // Why a connection ended. AppReason: the reason the closing side gave (the session's DisconnectReason), 0 if it ended without
    // one (a time-out, an unreachable address, a transport error).
    struct CloseInfo
    {
        uint32_t AppReason = 0;
        bool ClosedByPeer = false; // the other side closed it (else the transport gave up locally)
        bool TimedOut = false;     // nothing arrived for the time-out (or the address never answered)
        std::string Text;          // the closing side's explanation, or the transport's diagnosis
    };

    // A connection's quality right now (the transport's own measurements).
    struct ConnectionStats
    {
        int PingMs = -1;               // round trip
        float QualityLocal = -1.0f;    // fraction of packets that arrived intact and in order here (0..1, -1 unknown)
        float QualityRemote = -1.0f;   // the same, measured by the peer
        float OutBytesPerSecond = 0.0f;
        float InBytesPerSecond = 0.0f;
        float OutPacketsPerSecond = 0.0f;
        float InPacketsPerSecond = 0.0f;
        int PendingReliableBytes = 0;  // queued, not yet acknowledged
        int PendingUnreliableBytes = 0;
    };

    // Network emulation (Unreal's PktLag / PktLoss / the "Average" and "Bad" profiles): applied to the packets this process sends,
    // so a server and a client with 50 ms each make a 100 ms round trip. Reliable messages still arrive (resent), later.
    struct NetEmulation
    {
        float LagMs = 0.0f;       // added to every packet
        float JitterMs = 0.0f;    // random extra delay per packet: exponential with this mean, at most 3x
        float LossPercent = 0.0f; // packets dropped (0..100)

        bool IsActive() const { return LagMs > 0.0f || JitterMs > 0.0f || LossPercent > 0.0f; }
        static NetEmulation Off() { return {}; }
        static NetEmulation Average() { return { 30.0f, 10.0f, 1.0f }; } // each direction: ~60-80 ms round trip, 1 % loss
        static NetEmulation Bad() { return { 100.0f, 40.0f, 5.0f }; }    // each direction: ~200-300 ms round trip, 5 % loss
        // "off", "average", "bad"; false for another name.
        static bool FromPreset(std::string_view name, NetEmulation& emulation);
    };

    struct TransportConfig
    {
        float ConnectTimeoutSeconds = 10.0f; // a connection that never gets through
        float TimeoutSeconds = 10.0f;        // an established one that stops hearing from its peer
    };

    // What a transport reports while it is polled. The listener may call Send / Close from inside these.
    class TransportListener
    {
    public:
        virtual ~TransportListener() = default;
        // Established (incoming: a client connected to this listening transport; else the connection this side asked for).
        virtual void OnTransportConnected(ConnectionHandle connection, bool incoming) = 0;
        // Ended by the peer or by the transport (never for a connection this side closed itself). The handle is invalid afterwards.
        virtual void OnTransportDisconnected(ConnectionHandle connection, const CloseInfo& info) = 0;
        // A message: the bytes are only valid during the call.
        virtual void OnTransportMessage(ConnectionHandle connection, std::span<const uint8_t> message) = 0;
    };

    class NetTransport
    {
    public:
        virtual ~NetTransport() = default;

        virtual const char* GetName() const = 0;
        // Accepts connections on this port (all interfaces). False with a logged reason if it can't (port in use).
        virtual bool Listen(uint16_t port) = 0;
        // Starts a connection; OnTransportConnected or OnTransportDisconnected tells how it went. InvalidConnection (logged) if the
        // address can't be used at all.
        virtual ConnectionHandle Connect(std::string_view address) = 0;
        // Ends a connection with a reason the peer receives (CloseInfo::AppReason / Text). linger: reliable messages already sent
        // are still delivered first.
        virtual void Close(ConnectionHandle connection, uint32_t appReason, std::string_view text, bool linger) = 0;
        // False if the connection is gone, the message is empty (empty ones are the transport's own) or too large (512 KB).
        virtual bool Send(ConnectionHandle connection, std::span<const uint8_t> message, Delivery delivery) = 0;
        // Sends what Send() queued now, instead of waiting a few ms to batch it with more (call at the end of the frame).
        virtual void Flush() = 0;
        // Delivers what happened since the last poll to the listener (on the calling thread).
        virtual void Poll(TransportListener& listener) = 0;
        virtual ConnectionStats GetStats(ConnectionHandle connection) const = 0;
        // The connection's address as text (for logs): "192.168.1.5:7777", "loopback:7777".
        virtual std::string GetAddress(ConnectionHandle connection) const = 0;
        virtual void SetEmulation(const NetEmulation& emulation) = 0;
        virtual NetEmulation GetEmulation() const = 0;

        static constexpr size_t MaxMessageSize = 512 * 1024;
    };

    // "loopback" or "loopback:<port>": a LoopbackTransport.
    bool IsLoopbackAddress(std::string_view address);
    // The transport for an address (Connect) or for a server (loopback: false -> GameNetworkingSockets).
    std::unique_ptr<NetTransport> CreateTransport(bool loopback, const TransportConfig& config = {});
}
