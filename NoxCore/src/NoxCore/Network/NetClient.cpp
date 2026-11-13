#include "NetClient.h"

#include <format>

#include "NoxCore/Core/Log.h"

namespace Nox::Net
{
    NetClient::NetClient(NetClientConfig config)
        : m_Config(std::move(config))
    {
    }

    NetClient::~NetClient()
    {
        Disconnect(DisconnectReason::Quit, "the client left");
        Flush();
        m_Transport.reset(); // (its destructor gives the close a moment to leave)
    }

    bool NetClient::Connect(std::string_view address)
    {
        if (m_State != NetClientState::Disconnected)
            Disconnect(DisconnectReason::Quit, "the client connects elsewhere");
        m_Address = std::string(address);
        m_LastReason = DisconnectReason::None;
        m_LastText.clear();
        m_Welcome = {};

        // (From inside a callback of the old transport's poll -- a reconnect in OnDisconnected -- it is destroyed after the poll.)
        if (m_InPoll)
            m_RetiredTransport = std::move(m_Transport);
        m_Transport = CreateTransport(IsLoopbackAddress(address), m_Config.Transport);
        m_Transport->SetEmulation(m_Emulation);
        m_Connection = m_Transport->Connect(address);
        if (m_Connection == InvalidConnection)
        {
            m_LastReason = DisconnectReason::ConnectionFailed;
            m_LastText = std::format("'{}' can't be connected to", address);
            return false;
        }
        m_State = NetClientState::Connecting;
        m_StateSince = NetNow();
        NOX_CORE_INFO("Net: connecting to {} ({})", m_Address, m_Transport->GetName());
        return true;
    }

    void NetClient::Disconnect(DisconnectReason reason, std::string_view text)
    {
        if (m_State == NetClientState::Disconnected)
            return;
        if (m_Connection != InvalidConnection)
            m_Transport->Close(m_Connection, static_cast<uint32_t>(reason), text, true);
        NOX_CORE_INFO("Net: left {}: {} ({})", m_Address, ToString(reason), text);
        m_Connection = InvalidConnection;
        m_State = NetClientState::Disconnected;
        m_LastReason = reason;
        m_LastText = std::string(text);
    }

    void NetClient::Update()
    {
        if (m_Transport)
        {
            NetTransport* polled = m_Transport.get();
            m_InPoll = true;
            polled->Poll(*this);
            m_InPoll = false;
            m_RetiredTransport.reset();
        }
        if (m_State == NetClientState::Handshaking && NetNow() - m_StateSince > m_Config.HandshakeTimeoutSeconds)
            Fail(DisconnectReason::HandshakeTimeout, std::format("no Welcome within {:.0f} s", m_Config.HandshakeTimeoutSeconds));
    }

    void NetClient::Flush()
    {
        if (m_Transport)
            m_Transport->Flush();
    }

    bool NetClient::Send(std::span<const uint8_t> message, Delivery delivery)
    {
        if (m_State != NetClientState::Connected || message.empty() || message[0] < static_cast<uint8_t>(MessageType::FirstGameMessage))
            return false;
        return m_Transport->Send(m_Connection, message, delivery);
    }

    ConnectionStats NetClient::GetStats() const
    {
        return m_Transport && m_Connection != InvalidConnection ? m_Transport->GetStats(m_Connection) : ConnectionStats{};
    }

    void NetClient::SetEmulation(const NetEmulation& emulation)
    {
        m_Emulation = emulation;
        if (m_Transport)
            m_Transport->SetEmulation(emulation);
    }

    void NetClient::OnTransportConnected(ConnectionHandle connection, bool /*incoming*/)
    {
        if (connection != m_Connection || m_State != NetClientState::Connecting)
            return;
        m_State = NetClientState::Handshaking;
        m_StateSince = NetNow();
        NetWriter writer(64);
        WriteHello(writer, m_Config.Compatibility, m_Config.PlayerName);
        m_Transport->Send(m_Connection, writer.GetData(), Delivery::Reliable);
        m_Transport->Flush();
    }

    void NetClient::OnTransportDisconnected(ConnectionHandle connection, const CloseInfo& info)
    {
        if (connection != m_Connection)
            return;
        const bool reachedServer = m_State != NetClientState::Connecting;
        DisconnectReason reason = reachedServer ? DisconnectReason::ConnectionLost : DisconnectReason::ConnectionFailed;
        if (info.AppReason > 0 && info.AppReason < static_cast<uint32_t>(DisconnectReason::Count))
            reason = static_cast<DisconnectReason>(info.AppReason);
        else if (info.TimedOut && reachedServer)
            reason = DisconnectReason::TimedOut;
        std::string text = info.Text;
        if (reason == DisconnectReason::ConnectionFailed && info.TimedOut)
            text = std::format("no server answered at {} ({})", m_Address, info.Text);
        m_Connection = InvalidConnection;
        Ended(reason, text);
    }

    void NetClient::OnTransportMessage(ConnectionHandle connection, std::span<const uint8_t> message)
    {
        if (connection != m_Connection)
            return;
        if (message.empty())
        {
            Fail(DisconnectReason::ProtocolError, "an empty message from the server");
            return;
        }
        const uint8_t type = message[0];
        NetReader reader(message.subspan(1));
        if (m_State == NetClientState::Handshaking)
        {
            NetWelcome welcome;
            if (type != static_cast<uint8_t>(MessageType::Welcome) || !ReadWelcome(reader, welcome))
            {
                Fail(DisconnectReason::ProtocolError, std::format("expected the Welcome, got message type {}", type));
                return;
            }
            m_Welcome = std::move(welcome);
            m_State = NetClientState::Connected;
            m_StateSince = NetNow();
            NOX_CORE_INFO("Net: joined '{}' at {} as client {} (server tick {}, {} ticks/s, scene {})", m_Welcome.ServerName, m_Address,
                          m_Welcome.Client, m_Welcome.ServerTick, m_Welcome.TickRate, m_Welcome.SceneHandle);
            if (OnConnected)
                OnConnected(m_Welcome);
            return;
        }
        if (m_State != NetClientState::Connected)
            return;
        if (type < static_cast<uint8_t>(MessageType::FirstGameMessage))
        {
            Fail(DisconnectReason::ProtocolError, std::format("unexpected session message {} from the server", type));
            return;
        }
        if (OnMessage)
            OnMessage(type, reader);
    }

    void NetClient::Fail(DisconnectReason reason, const std::string& text)
    {
        if (m_Connection != InvalidConnection)
            m_Transport->Close(m_Connection, static_cast<uint32_t>(reason), text, true);
        m_Connection = InvalidConnection;
        Ended(reason, text);
    }

    void NetClient::Ended(DisconnectReason reason, const std::string& text)
    {
        m_State = NetClientState::Disconnected;
        m_LastReason = reason;
        m_LastText = text;
        NOX_CORE_WARN("Net: disconnected from {}: {} ({})", m_Address, ToString(reason), text);
        if (OnDisconnected)
            OnDisconnected(reason, text);
    }
}
