#include "LoopbackTransport.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <format>

#include "NetProtocol.h"
#include "NoxCore/Core/Log.h"

namespace Nox::Net
{
    namespace
    {
        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // The listening loopback transports by port, and every link's queues: one lock for all of them (the transports of a
        // process normally live on one thread; the lock keeps one on another thread safe).
        std::mutex s_HubMutex;
        std::unordered_map<uint16_t, LoopbackTransport*> s_Listeners;

        bool ParsePort(std::string_view address, uint16_t& port)
        {
            port = DefaultPort;
            const size_t colon = address.find(':');
            if (colon == std::string_view::npos)
                return true;
            const std::string digits(address.substr(colon + 1));
            char* end = nullptr;
            const long value = std::strtol(digits.c_str(), &end, 10);
            if (digits.empty() || *end != '\0' || value <= 0 || value > 65535)
                return false;
            port = static_cast<uint16_t>(value);
            return true;
        }

    }

    struct LoopbackPacket
    {
        enum class Kind : uint8_t { Connect, Accept, Data, KeepAlive, Close };
        Kind Type = Kind::Data;
        double DeliverAt = 0.0;
        uint64_t Order = 0; // sending order: ties of DeliverAt keep it
        std::vector<uint8_t> Bytes;
        uint32_t AppReason = 0;
        std::string Text;
    };

    struct LoopbackSide
    {
        LoopbackTransport* Owner = nullptr; // null once that transport closed its side or was destroyed
        ConnectionHandle Handle = InvalidConnection;
        std::vector<LoopbackPacket> Inbound; // on their way to this side
        double LastReliableDeliverAt = 0.0;  // of what this side sends: reliable packets never overtake each other
        double CreatedAt = 0.0;
        double LastReceivedAt = 0.0;
        double LastSentAt = 0.0;
        bool Established = false;
        bool Closed = false;
        // What this side's sender emulates (for the stats of both sides).
        float EmulatedDelayMs = 0.0f;
        float EmulatedLoss = 0.0f;
        // Rates over the last full second.
        double WindowStart = 0.0;
        uint64_t WindowBytesOut = 0, WindowBytesIn = 0, WindowPacketsOut = 0, WindowPacketsIn = 0;
        float BytesOutPerSecond = 0.0f, BytesInPerSecond = 0.0f, PacketsOutPerSecond = 0.0f, PacketsInPerSecond = 0.0f;
    };

    struct LoopbackLink
    {
        LoopbackSide Sides[2];
        uint64_t NextOrder = 0;
        uint16_t Port = 0;

        // A packet from one side to the other under the sender's emulation (hub locked).
        void Push(LoopbackTransport& sender, int fromSide, LoopbackPacket::Kind type, std::span<const uint8_t> bytes, bool reliable,
                  double now, uint32_t appReason = 0, std::string_view text = {})
        {
            LoopbackSide& from = Sides[fromSide];
            LoopbackSide& to = Sides[1 - fromSide];
            from.LastSentAt = now;
            from.EmulatedDelayMs = sender.m_Emulation.LagMs + sender.m_Emulation.JitterMs;
            from.EmulatedLoss = std::clamp(sender.m_Emulation.LossPercent / 100.0f, 0.0f, 1.0f);
            from.WindowBytesOut += bytes.size();
            ++from.WindowPacketsOut;
            if (to.Closed)
                return; // gone: the packet goes nowhere, as on a network
            double deliverAt = now;
            if (!sender.ScheduleDelivery(now, reliable, deliverAt, from.LastReliableDeliverAt))
                return; // lost
            LoopbackPacket packet;
            packet.Type = type;
            packet.DeliverAt = deliverAt;
            packet.Order = NextOrder++;
            packet.Bytes.assign(bytes.begin(), bytes.end());
            packet.AppReason = appReason;
            packet.Text = std::string(text);
            to.Inbound.push_back(std::move(packet));
        }
    };

    LoopbackTransport::LoopbackTransport(const TransportConfig& config)
        : m_Config(config), m_Random(std::random_device{}())
    {
    }

    LoopbackTransport::~LoopbackTransport()
    {
        std::lock_guard lock(s_HubMutex);
        if (m_ListenPort != 0)
        {
            auto listener = s_Listeners.find(m_ListenPort);
            if (listener != s_Listeners.end() && listener->second == this)
                s_Listeners.erase(listener);
        }
        // What is still open ends as if the process had closed its sockets: the peers hear it (no reason), then the links are theirs.
        const double now = Now();
        for (auto& [handle, connection] : m_Connections)
        {
            connection.Link->Push(*this, connection.Side, LoopbackPacket::Kind::Close, {}, true, now, 0, "the peer's transport was destroyed");
            LoopbackSide& self = connection.Link->Sides[connection.Side];
            self.Owner = nullptr;
            self.Closed = true;
            self.Inbound.clear();
        }
    }

    bool LoopbackTransport::Listen(uint16_t port)
    {
        std::lock_guard lock(s_HubMutex);
        auto [listener, inserted] = s_Listeners.try_emplace(port, this);
        if (!inserted && listener->second != this)
        {
            NOX_CORE_ERROR("Net: loopback port {} is already served in this process", port);
            return false;
        }
        m_ListenPort = port;
        return true;
    }

    ConnectionHandle LoopbackTransport::AddIncoming(const std::shared_ptr<LoopbackLink>& link)
    {
        const ConnectionHandle handle = m_NextHandle++;
        LoopbackSide& side = link->Sides[1];
        side.Owner = this;
        side.Handle = handle;
        side.CreatedAt = side.LastReceivedAt = side.WindowStart = Now();
        m_Connections.emplace(handle, Connection{ link, 1 });
        return handle;
    }

    ConnectionHandle LoopbackTransport::Connect(std::string_view address)
    {
        uint16_t port = DefaultPort;
        if (!IsLoopbackAddress(address) || !ParsePort(address, port))
        {
            NOX_CORE_ERROR("Net: '{}' is not a loopback address (loopback or loopback:<port>)", address);
            return InvalidConnection;
        }

        std::lock_guard lock(s_HubMutex);
        const double now = Now();
        auto link = std::make_shared<LoopbackLink>();
        link->Port = port;
        const ConnectionHandle handle = m_NextHandle++;
        LoopbackSide& side = link->Sides[0];
        side.Owner = this;
        side.Handle = handle;
        side.CreatedAt = side.LastReceivedAt = side.WindowStart = now;
        m_Connections.emplace(handle, Connection{ link, 0 });

        auto listener = s_Listeners.find(port);
        if (listener == s_Listeners.end())
        {
            // Nothing listens: the connect request is never answered (like an address without a server), and times out.
            link->Sides[1].Closed = true;
            return handle;
        }
        listener->second->AddIncoming(link);
        link->Push(*this, 0, LoopbackPacket::Kind::Connect, {}, true, now);
        return handle;
    }

    void LoopbackTransport::Close(ConnectionHandle connection, uint32_t appReason, std::string_view text, bool /*linger: always*/)
    {
        std::lock_guard lock(s_HubMutex);
        auto found = m_Connections.find(connection);
        if (found == m_Connections.end())
            return;
        // After what was already sent: a loopback connection always lingers (its reliable messages are already on their way).
        found->second.Link->Push(*this, found->second.Side, LoopbackPacket::Kind::Close, {}, true, Now(), appReason, text);
        LoopbackSide& self = found->second.Link->Sides[found->second.Side];
        self.Owner = nullptr;
        self.Closed = true;
        self.Inbound.clear();
        m_Connections.erase(found);
    }

    bool LoopbackTransport::Send(ConnectionHandle connection, std::span<const uint8_t> message, Delivery delivery)
    {
        if (message.empty() || message.size() > MaxMessageSize)
            return false;
        std::lock_guard lock(s_HubMutex);
        auto found = m_Connections.find(connection);
        if (found == m_Connections.end() || !found->second.Link->Sides[found->second.Side].Established)
            return false;
        found->second.Link->Push(*this, found->second.Side, LoopbackPacket::Kind::Data, message, delivery == Delivery::Reliable, Now());
        return true;
    }

    bool LoopbackTransport::ScheduleDelivery(double now, bool reliable, double& deliverAt, double& lastReliable)
    {
        const float loss = std::clamp(m_Emulation.LossPercent / 100.0f, 0.0f, 1.0f);
        std::uniform_real_distribution<float> chance(0.0f, 1.0f);
        auto delayMs = [&]()
        {
            float jitter = 0.0f;
            if (m_Emulation.JitterMs > 0.0f)
            {
                std::exponential_distribution<float> distribution(1.0f / m_Emulation.JitterMs);
                jitter = std::min(distribution(m_Random), 3.0f * m_Emulation.JitterMs);
            }
            return std::max(m_Emulation.LagMs, 0.0f) + jitter;
        };

        if (!reliable)
        {
            if (loss > 0.0f && chance(m_Random) < loss)
                return false;
            deliverAt = now + delayMs() / 1000.0;
            return true;
        }
        if (loss >= 1.0f)
            return false; // nothing gets through: the connection times out
        // A lost reliable packet is resent after about a round trip; each loss costs another one.
        float ms = delayMs();
        const float resend = std::max(2.0f * (m_Emulation.LagMs + m_Emulation.JitterMs), 20.0f);
        for (int attempt = 0; attempt < 32 && loss > 0.0f && chance(m_Random) < loss; ++attempt)
            ms += resend;
        deliverAt = std::max(now + ms / 1000.0, lastReliable);
        lastReliable = deliverAt;
        return true;
    }

    void LoopbackTransport::Poll(TransportListener& listener)
    {
        struct Event
        {
            enum class Kind : uint8_t { Connected, Disconnected, Message };
            Kind Type = Kind::Message;
            ConnectionHandle Connection = InvalidConnection;
            bool Incoming = false;
            CloseInfo Close;
            std::vector<uint8_t> Bytes;

            static Event Connected(ConnectionHandle connection, bool incoming)
            {
                Event event;
                event.Type = Kind::Connected;
                event.Connection = connection;
                event.Incoming = incoming;
                return event;
            }
            static Event Message(ConnectionHandle connection, std::vector<uint8_t> bytes)
            {
                Event event;
                event.Type = Kind::Message;
                event.Connection = connection;
                event.Bytes = std::move(bytes);
                return event;
            }
            static Event Disconnected(ConnectionHandle connection, CloseInfo close)
            {
                Event event;
                event.Type = Kind::Disconnected;
                event.Connection = connection;
                event.Close = std::move(close);
                return event;
            }
        };
        std::vector<Event> events;
        {
            std::lock_guard lock(s_HubMutex);
            const double now = Now();
            std::vector<ConnectionHandle> ended;
            for (auto& [handle, connection] : m_Connections)
            {
                LoopbackLink& link = *connection.Link;
                LoopbackSide& self = link.Sides[connection.Side];

                // What arrived by now, in arrival order.
                std::vector<LoopbackPacket> due;
                auto firstLater = std::partition(self.Inbound.begin(), self.Inbound.end(),
                                                 [now](const LoopbackPacket& packet) { return packet.DeliverAt <= now; });
                due.assign(std::make_move_iterator(self.Inbound.begin()), std::make_move_iterator(firstLater));
                self.Inbound.erase(self.Inbound.begin(), firstLater);
                std::sort(due.begin(), due.end(), [](const LoopbackPacket& a, const LoopbackPacket& b)
                          { return a.DeliverAt != b.DeliverAt ? a.DeliverAt < b.DeliverAt : a.Order < b.Order; });

                bool closed = false;
                for (LoopbackPacket& packet : due)
                {
                    self.LastReceivedAt = now;
                    self.WindowBytesIn += packet.Bytes.size();
                    ++self.WindowPacketsIn;
                    switch (packet.Type)
                    {
                    case LoopbackPacket::Kind::Connect:
                        self.Established = true;
                        events.push_back(Event::Connected(handle, true));
                        link.Push(*this, connection.Side, LoopbackPacket::Kind::Accept, {}, true, now);
                        break;
                    case LoopbackPacket::Kind::Accept:
                        self.Established = true;
                        events.push_back(Event::Connected(handle, false));
                        break;
                    case LoopbackPacket::Kind::Data:
                        if (self.Established)
                            events.push_back(Event::Message(handle, std::move(packet.Bytes)));
                        break;
                    case LoopbackPacket::Kind::KeepAlive:
                        break;
                    case LoopbackPacket::Kind::Close:
                    {
                        CloseInfo info;
                        info.AppReason = packet.AppReason;
                        info.ClosedByPeer = true;
                        info.Text = packet.Text.empty() ? "closed by the peer" : packet.Text;
                        if (self.Established)
                            events.push_back(Event::Disconnected(handle, std::move(info)));
                        closed = true;
                        break;
                    }
                    }
                    if (closed)
                        break;
                }

                if (!closed)
                {
                    if (!self.Established && now - self.CreatedAt > m_Config.ConnectTimeoutSeconds)
                    {
                        // An incoming one that never arrived was never announced; an outgoing one reports the time-out.
                        if (connection.Side == 0)
                        {
                            CloseInfo info;
                            info.TimedOut = true;
                            info.Text = std::format("no answer from loopback:{} within {:.0f} s", link.Port, m_Config.ConnectTimeoutSeconds);
                            events.push_back(Event::Disconnected(handle, std::move(info)));
                        }
                        closed = true;
                    }
                    else if (self.Established && now - self.LastReceivedAt > m_Config.TimeoutSeconds)
                    {
                        CloseInfo info;
                        info.TimedOut = true;
                        info.Text = std::format("nothing received for {:.0f} s", m_Config.TimeoutSeconds);
                        events.push_back(Event::Disconnected(handle, std::move(info)));
                        closed = true;
                    }
                    else if (self.Established && now - self.LastSentAt >= 1.0)
                        link.Push(*this, connection.Side, LoopbackPacket::Kind::KeepAlive, {}, false, now);
                }

                if (now - self.WindowStart >= 1.0)
                {
                    const double seconds = now - self.WindowStart;
                    self.BytesOutPerSecond = static_cast<float>(self.WindowBytesOut / seconds);
                    self.BytesInPerSecond = static_cast<float>(self.WindowBytesIn / seconds);
                    self.PacketsOutPerSecond = static_cast<float>(self.WindowPacketsOut / seconds);
                    self.PacketsInPerSecond = static_cast<float>(self.WindowPacketsIn / seconds);
                    self.WindowBytesOut = self.WindowBytesIn = self.WindowPacketsOut = self.WindowPacketsIn = 0;
                    self.WindowStart = now;
                }

                if (closed)
                {
                    self.Owner = nullptr;
                    self.Closed = true;
                    self.Inbound.clear();
                    ended.push_back(handle);
                }
            }
            for (ConnectionHandle handle : ended)
                m_Connections.erase(handle);
        }

        // Outside the lock: the listener sends and closes from in here. A connection it closed meanwhile gets nothing more.
        for (Event& event : events)
        {
            switch (event.Type)
            {
            case Event::Kind::Connected:
                if (m_Connections.contains(event.Connection))
                    listener.OnTransportConnected(event.Connection, event.Incoming);
                break;
            case Event::Kind::Message:
                if (m_Connections.contains(event.Connection))
                    listener.OnTransportMessage(event.Connection, event.Bytes);
                break;
            case Event::Kind::Disconnected:
                listener.OnTransportDisconnected(event.Connection, event.Close);
                break;
            }
        }
    }

    ConnectionStats LoopbackTransport::GetStats(ConnectionHandle connection) const
    {
        std::lock_guard lock(s_HubMutex);
        ConnectionStats stats;
        auto found = m_Connections.find(connection);
        if (found == m_Connections.end())
            return stats;
        const LoopbackSide& self = found->second.Link->Sides[found->second.Side];
        const LoopbackSide& peer = found->second.Link->Sides[1 - found->second.Side];
        if (!self.Established)
            return stats;
        // The emulated round trip: what each side's sender adds (no wire delay inside a process).
        stats.PingMs = static_cast<int>(self.EmulatedDelayMs + peer.EmulatedDelayMs + 0.5f);
        stats.QualityLocal = 1.0f - peer.EmulatedLoss;
        stats.QualityRemote = 1.0f - self.EmulatedLoss;
        stats.OutBytesPerSecond = self.BytesOutPerSecond;
        stats.InBytesPerSecond = self.BytesInPerSecond;
        stats.OutPacketsPerSecond = self.PacketsOutPerSecond;
        stats.InPacketsPerSecond = self.PacketsInPerSecond;
        return stats;
    }

    std::string LoopbackTransport::GetAddress(ConnectionHandle connection) const
    {
        std::lock_guard lock(s_HubMutex);
        auto found = m_Connections.find(connection);
        if (found == m_Connections.end())
            return {};
        return std::format("loopback:{}#{}", found->second.Link->Port, found->second.Side == 0 ? found->second.Link->Sides[1].Handle : connection);
    }
}
