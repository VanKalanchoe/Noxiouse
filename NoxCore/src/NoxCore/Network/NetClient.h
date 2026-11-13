#pragma once
#include "NetBuffer.h"
#include "NetProtocol.h"
#include "NetTransport.h"

#include <functional>
#include <memory>
#include <string>

// The client's side of a session (docs/Networking_Plan_2026-11.md, NET1; Unreal's UPendingNetGame + UNetConnection): connects
// to a server address ("loopback[:port]" in the same process, else IP / host name over UDP), sends the Hello, waits for the
// Welcome, then exchanges the game's messages until either side disconnects. Update once a frame (before the simulation), Flush
// after the frame's sends.
namespace Nox::Net
{
    enum class NetClientState : uint8_t
    {
        Disconnected,
        Connecting,  // the transport is reaching the server
        Handshaking, // connected; Hello sent, waiting for the Welcome
        Connected,   // joined: the Welcome arrived
    };

    inline const char* ToString(NetClientState state)
    {
        switch (state)
        {
        case NetClientState::Disconnected: return "disconnected";
        case NetClientState::Connecting: return "connecting";
        case NetClientState::Handshaking: return "handshaking";
        case NetClientState::Connected: return "connected";
        }
        return "unknown";
    }

    struct NetClientConfig
    {
        float HandshakeTimeoutSeconds = 5.0f;
        std::string PlayerName;
        NetCompatibility Compatibility; // what this client is: the server checks it
        TransportConfig Transport;
    };

    class NetClient final : private TransportListener
    {
    public:
        explicit NetClient(NetClientConfig config);
        ~NetClient() override; // Disconnect(Quit)

        // Starts connecting (an earlier session ends first). False if the address can't be used at all (logged; nothing to wait for).
        bool Connect(std::string_view address);
        // Leaves: the server receives the reason. OnDisconnected is not called for a disconnect this side asked for.
        void Disconnect(DisconnectReason reason = DisconnectReason::Quit, std::string_view text = "the client left");
        void Update();
        void Flush();

        // A game message (its first byte a type >= MessageType::FirstGameMessage); false unless connected.
        bool Send(std::span<const uint8_t> message, Delivery delivery);

        NetClientState GetState() const { return m_State; }
        bool IsConnected() const { return m_State == NetClientState::Connected; }
        const NetWelcome& GetWelcome() const { return m_Welcome; }
        const std::string& GetServerAddress() const { return m_Address; }
        ConnectionStats GetStats() const;
        // Why the last session ended (after OnDisconnected; None while connected or never connected).
        DisconnectReason GetLastDisconnectReason() const { return m_LastReason; }
        const std::string& GetLastDisconnectText() const { return m_LastText; }
        // Applied to this client's transport now and to the next one Connect makes.
        void SetEmulation(const NetEmulation& emulation);
        const NetEmulation& GetEmulation() const { return m_Emulation; }

        std::function<void(const NetWelcome&)> OnConnected;
        std::function<void(DisconnectReason, const std::string&)> OnDisconnected;
        // A game message from the server (type >= FirstGameMessage; the reader is after the type byte).
        std::function<void(uint8_t, NetReader&)> OnMessage;

    private:
        void OnTransportConnected(ConnectionHandle connection, bool incoming) override;
        void OnTransportDisconnected(ConnectionHandle connection, const CloseInfo& info) override;
        void OnTransportMessage(ConnectionHandle connection, std::span<const uint8_t> message) override;
        // This side ends the session: closes the connection with the reason, and reports it like a disconnect from the server.
        void Fail(DisconnectReason reason, const std::string& text);
        void Ended(DisconnectReason reason, const std::string& text);

        NetClientConfig m_Config;
        NetEmulation m_Emulation;
        std::unique_ptr<NetTransport> m_Transport;
        std::unique_ptr<NetTransport> m_RetiredTransport; // replaced while it was being polled
        bool m_InPoll = false;
        ConnectionHandle m_Connection = InvalidConnection;
        NetClientState m_State = NetClientState::Disconnected;
        std::string m_Address;
        double m_StateSince = 0.0;
        NetWelcome m_Welcome;
        DisconnectReason m_LastReason = DisconnectReason::None;
        std::string m_LastText;
    };
}
