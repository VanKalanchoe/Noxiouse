// NoxNetTest (docs/Networking_Plan_2026-11.md, NET1): the sessions and transports end to end, without a window or GPU --
// NetServer / NetClient over LoopbackTransport and GnsTransport (real UDP on 127.0.0.1, ports 7100-7260): the handshake and its
// refusals (protocol, project, content checksum, server full), game messages both ways, network emulation (round trip, loss,
// reliable order under 20 % loss), kick, handshake time-out, garbage, dead links (with and without traffic), server shutdown,
// and the message reader against random bytes. Build with -DNOX_BUILD_TESTS=ON; run NoxNetTest (exit code 0: all passed).
#include "NoxCore/Network/GnsTransport.h"
#include "NoxCore/Network/LoopbackTransport.h"
#include "NoxCore/Network/NetClient.h"
#include "NoxCore/Network/NetServer.h"
#include "NoxCore/Network/NetSnapshot.h"
#include "NoxCore/Network/NetQuantize.h"
#include "NoxCore/Network/NetPrediction.h"
#include "NoxCore/Core/Log.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <thread>
#include <vector>

using namespace Nox::Net;

static int s_Failures = 0;
static int s_Checks = 0;
#define CHECK(condition, ...)                                                                                                          \
    do {                                                                                                                               \
        ++s_Checks;                                                                                                                    \
        if (!(condition)) { ++s_Failures; std::printf("FAIL %s:%d: %s -- %s\n", __FILE__, __LINE__, #condition, std::format(__VA_ARGS__).c_str()); } \
        else std::printf("ok   %s\n", std::format(__VA_ARGS__).c_str());                                                               \
    } while (0)

static NetCompatibility Game(uint64_t checksum = 0x1234, uint32_t protocol = ProtocolVersion, std::string name = "Facerun")
{
    NetCompatibility c;
    c.Protocol = protocol;
    c.ProjectName = name;
    c.ContentChecksum = checksum;
    return c;
}

// Runs server / clients for up to `seconds` or until `done` (~1 ms steps, like a frame loop).
static bool Pump(NetServer* server, std::vector<NetClient*> clients, double seconds, const std::function<bool()>& done)
{
    const double until = NetNow() + seconds;
    while (NetNow() < until)
    {
        if (server) { server->Update(); server->Flush(); }
        for (NetClient* client : clients) { client->Update(); client->Flush(); }
        if (done && done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done ? done() : true;
}

static std::unique_ptr<NetTransport> MakeTransport(bool loopback, TransportConfig config = {})
{
    return CreateTransport(loopback, config);
}

static std::string Address(bool loopback, uint16_t port)
{
    return loopback ? std::format("loopback:{}", port) : std::format("127.0.0.1:{}", port);
}

static constexpr uint8_t GameMessage = static_cast<uint8_t>(MessageType::FirstGameMessage);

static void TestHandshake(bool loopback, uint16_t port)
{
    std::printf("\n== handshake (%s)\n", loopback ? "loopback" : "GNS");
    NetServerConfig sc;
    sc.Port = port;
    sc.ServerName = "Test Server";
    sc.Compatibility = Game();
    NetServer server(sc);
    server.FillWelcome = [](NetWelcome& w) { w.ServerTick = 4242; w.SceneHandle = 0xABCDEF; };
    int joined = 0;
    server.OnClientJoined = [&](const NetClientInfo&) { ++joined; };
    CHECK(server.AddTransport(MakeTransport(loopback)), "server listens on {}", port);

    NetClientConfig cc;
    cc.Compatibility = Game();
    cc.PlayerName = "Alice";
    NetClient client(cc);
    NetWelcome got;
    client.OnConnected = [&](const NetWelcome& w) { got = w; };
    CHECK(client.Connect(Address(loopback, port)), "client starts connecting");
    const double start = NetNow();
    Pump(&server, { &client }, 5.0, [&] { return client.IsConnected() && joined == 1; });
    CHECK(client.IsConnected(), "client connected in {:.1f} ms", (NetNow() - start) * 1000.0);
    CHECK(got.Client == 1 && got.ServerTick == 4242 && got.SceneHandle == 0xABCDEF && got.ServerName == "Test Server" && got.TickRate == 60,
          "welcome: client {}, tick {}, scene {:x}, '{}'", got.Client, got.ServerTick, got.SceneHandle, got.ServerName);
    CHECK(server.GetClients().size() == 1 && server.GetClients()[0].Name == "Alice", "server lists 'Alice'");

    // Game messages both ways.
    std::vector<uint32_t> atServer, atClient;
    server.OnMessage = [&](const NetClientInfo& from, uint8_t type, NetReader& reader)
    {
        if (type != GameMessage) return;
        const uint32_t value = reader.ReadU32();
        atServer.push_back(value);
        NetWriter echo;
        echo.WriteU8(GameMessage);
        echo.WriteU32(value * 2);
        server.Send(from.Id, echo.GetData(), Delivery::Reliable);
    };
    client.OnMessage = [&](uint8_t type, NetReader& reader) { if (type == GameMessage) atClient.push_back(reader.ReadU32()); };
    for (uint32_t i = 0; i < 100; ++i)
    {
        NetWriter w;
        w.WriteU8(GameMessage);
        w.WriteU32(i);
        client.Send(w.GetData(), Delivery::Reliable);
    }
    Pump(&server, { &client }, 5.0, [&] { return atClient.size() == 100; });
    bool ordered = atServer.size() == 100 && atClient.size() == 100;
    for (uint32_t i = 0; ordered && i < 100; ++i)
        ordered = atServer[i] == i && atClient[i] == 2 * i;
    CHECK(ordered, "100 reliable messages there and back, in order ({} / {})", atServer.size(), atClient.size());
    CHECK(!client.Send(std::vector<uint8_t>{ 5 }, Delivery::Reliable), "a session-type message is refused by Send");

    // The client leaves: the server hears Quit.
    DisconnectReason leftReason = DisconnectReason::None;
    server.OnClientLeft = [&](const NetClientInfo&, DisconnectReason r, const std::string&) { leftReason = r; };
    client.Disconnect();
    Pump(&server, { &client }, 5.0, [&] { return leftReason != DisconnectReason::None; });
    CHECK(leftReason == DisconnectReason::Quit, "server: client left with '{}'", ToString(leftReason));
    CHECK(server.GetClients().empty(), "server has no clients");
}

static void TestEmulation(bool loopback, uint16_t port)
{
    std::printf("\n== emulation: 40 ms lag each way, 10 ms jitter, 20 %% loss (%s)\n", loopback ? "loopback" : "GNS");
    NetServerConfig sc;
    sc.Port = port;
    sc.Compatibility = Game();
    NetServer server(sc);
    CHECK(server.AddTransport(MakeTransport(loopback)), "listening");
    NetClientConfig cc;
    cc.Compatibility = Game();
    NetClient client(cc);
    client.Connect(Address(loopback, port));
    Pump(&server, { &client }, 5.0, [&] { return client.IsConnected(); });
    CHECK(client.IsConnected(), "connected without emulation");

    const NetEmulation emulation{ 40.0f, 10.0f, 20.0f };
    server.SetEmulation(emulation);
    client.SetEmulation(emulation);

    // Round trip: a reliable ping-pong of 20 messages, one at a time (what the session layer sees, resends included).
    uint32_t received = 0;
    std::vector<double> roundTrips;
    double sentAt = 0.0;
    server.OnMessage = [&](const NetClientInfo& from, uint8_t, NetReader& reader)
    {
        NetWriter w;
        w.WriteU8(GameMessage);
        w.WriteU32(reader.ReadU32());
        server.Send(from.Id, w.GetData(), Delivery::Unreliable);
    };
    client.OnMessage = [&](uint8_t, NetReader& reader) { reader.ReadU32(); ++received; roundTrips.push_back(NetNow() - sentAt); };
    int lost = 0;
    for (uint32_t i = 0; i < 40; ++i)
    {
        const uint32_t before = received;
        NetWriter w;
        w.WriteU8(GameMessage);
        w.WriteU32(i);
        sentAt = NetNow();
        client.Send(w.GetData(), Delivery::Unreliable);
        if (!Pump(&server, { &client }, 0.6, [&] { return received > before; }))
            ++lost;
    }
    std::sort(roundTrips.begin(), roundTrips.end());
    const double median = roundTrips.empty() ? 0.0 : roundTrips[roundTrips.size() / 2] * 1000.0;
    // 2 x 40 ms lag + jitter (mean 10 each way, capped 30)
    CHECK(median >= 80.0 && median <= 160.0, "unreliable round trip median {:.0f} ms (expected 80-160: 2 x 40 lag + jitter)", median);
    // P(both directions survive) = 0.8^2 = 0.64 -> ~14 of 40 lost
    CHECK(lost >= 4 && lost <= 26, "{} of 40 unreliable round trips lost (expected ~14: 1 - 0.8^2)", lost);
    const ConnectionStats stats = client.GetStats();
    std::printf("     transport ping %d ms, quality local %.2f remote %.2f, out %.0f B/s in %.0f B/s\n", stats.PingMs, stats.QualityLocal,
                stats.QualityRemote, stats.OutBytesPerSecond, stats.InBytesPerSecond);
    CHECK(stats.PingMs >= 60 && stats.PingMs <= 250, "transport reports ping {} ms", stats.PingMs);

    // 500 reliable messages under 20 % loss: all, in order.
    std::vector<uint32_t> order;
    server.OnMessage = [&](const NetClientInfo&, uint8_t, NetReader& reader) { order.push_back(reader.ReadU32()); };
    const double start = NetNow();
    for (uint32_t i = 0; i < 500; ++i)
    {
        NetWriter w;
        w.WriteU8(GameMessage);
        w.WriteU32(i);
        w.WriteBytes(std::vector<uint8_t>(200, static_cast<uint8_t>(i)));
        client.Send(w.GetData(), Delivery::Reliable);
    }
    Pump(&server, { &client }, 20.0, [&] { return order.size() == 500; });
    bool inOrder = order.size() == 500;
    for (uint32_t i = 0; inOrder && i < 500; ++i)
        inOrder = order[i] == i;
    CHECK(inOrder, "500 reliable messages under 20 % loss arrive complete and in order ({} in {:.2f} s)", order.size(), NetNow() - start);
    server.SetEmulation({});
    client.SetEmulation({});
}

static void TestRefusals(bool loopback, uint16_t port)
{
    std::printf("\n== refusals (%s)\n", loopback ? "loopback" : "GNS");
    NetServerConfig sc;
    sc.Port = port;
    sc.MaxClients = 1;
    sc.Compatibility = Game();
    NetServer server(sc);
    CHECK(server.AddTransport(MakeTransport(loopback)), "listening");

    auto attempt = [&](NetCompatibility compatibility, DisconnectReason expected, const char* what)
    {
        NetClientConfig cc;
        cc.Compatibility = compatibility;
        NetClient client(cc);
        DisconnectReason reason = DisconnectReason::None;
        std::string text;
        client.OnDisconnected = [&](DisconnectReason r, const std::string& t) { reason = r; text = t; };
        client.Connect(Address(loopback, port));
        Pump(&server, { &client }, 5.0, [&] { return reason != DisconnectReason::None; });
        CHECK(reason == expected && !client.IsConnected(), "{}: '{}' ({})", what, ToString(reason), text);
    };
    attempt(Game(0x1234, 99), DisconnectReason::VersionMismatch, "other protocol");
    attempt(Game(0x9999), DisconnectReason::ContentMismatch, "other content checksum");
    attempt(Game(0x1234, ProtocolVersion, "OtherGame"), DisconnectReason::ContentMismatch, "other project");
    CHECK(server.GetClients().empty(), "nobody joined");

    // Full: one joins, the second is refused.
    NetClientConfig cc;
    cc.Compatibility = Game();
    NetClient first(cc);
    first.Connect(Address(loopback, port));
    Pump(&server, { &first }, 5.0, [&] { return first.IsConnected(); });
    CHECK(first.IsConnected(), "first client joined");
    NetClient second(cc);
    DisconnectReason reason = DisconnectReason::None;
    second.OnDisconnected = [&](DisconnectReason r, const std::string&) { reason = r; };
    second.Connect(Address(loopback, port));
    Pump(&server, { &first, &second }, 5.0, [&] { return reason != DisconnectReason::None; });
    CHECK(reason == DisconnectReason::ServerFull, "second client: '{}' ({})", ToString(reason), second.GetLastDisconnectText());
    CHECK(first.IsConnected(), "first client still connected");

    // Kick.
    DisconnectReason kicked = DisconnectReason::None;
    std::string kickText;
    first.OnDisconnected = [&](DisconnectReason r, const std::string& t) { kicked = r; kickText = t; };
    CHECK(server.Kick(server.GetClients()[0].Id, "too much lag"), "kick");
    Pump(&server, { &first }, 5.0, [&] { return kicked != DisconnectReason::None; });
    CHECK(kicked == DisconnectReason::Kicked && kickText == "too much lag", "kicked client: '{}' ({})", ToString(kicked), kickText);
}

// A raw transport connection that never sends a Hello / sends garbage.
struct RawPeer final : TransportListener
{
    std::unique_ptr<NetTransport> Transport;
    ConnectionHandle Connection = InvalidConnection;
    bool Connected = false;
    bool Ended = false;
    CloseInfo Close;
    void OnTransportConnected(ConnectionHandle, bool) override { Connected = true; }
    void OnTransportDisconnected(ConnectionHandle, const CloseInfo& info) override { Ended = true; Close = info; }
    void OnTransportMessage(ConnectionHandle, std::span<const uint8_t>) override {}
};

static void TestBadClients(bool loopback, uint16_t port)
{
    std::printf("\n== handshake time-out and garbage (%s)\n", loopback ? "loopback" : "GNS");
    NetServerConfig sc;
    sc.Port = port;
    sc.HandshakeTimeoutSeconds = 1.0f;
    sc.Compatibility = Game();
    NetServer server(sc);
    CHECK(server.AddTransport(MakeTransport(loopback)), "listening");

    auto run = [&](RawPeer& raw, double seconds, const std::function<void()>& onConnected)
    {
        bool sent = false;
        const double until = NetNow() + seconds;
        while (NetNow() < until && !raw.Ended)
        {
            server.Update();
            raw.Transport->Poll(raw);
            if (raw.Connected && !sent) { sent = true; if (onConnected) onConnected(); raw.Transport->Flush(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    RawPeer silent;
    silent.Transport = MakeTransport(loopback);
    silent.Connection = silent.Transport->Connect(Address(loopback, port));
    const double start = NetNow();
    run(silent, 5.0, nullptr);
    CHECK(silent.Ended && silent.Close.AppReason == static_cast<uint32_t>(DisconnectReason::HandshakeTimeout),
          "a silent connection is dropped after {:.2f} s: reason {} ({})", NetNow() - start, silent.Close.AppReason, silent.Close.Text);

    RawPeer garbage;
    garbage.Transport = MakeTransport(loopback);
    garbage.Connection = garbage.Transport->Connect(Address(loopback, port));
    run(garbage, 5.0, [&] { std::vector<uint8_t> junk = { 1, 0xde, 0xad }; garbage.Transport->Send(garbage.Connection, junk, Delivery::Reliable); });
    CHECK(garbage.Ended && garbage.Close.AppReason == static_cast<uint32_t>(DisconnectReason::ProtocolError),
          "a garbage Hello is refused: reason {} ({})", garbage.Close.AppReason, garbage.Close.Text);
}

static void TestTimeouts(bool loopback, uint16_t port)
{
    std::printf("\n== time-outs (%s)\n", loopback ? "loopback" : "GNS");
    TransportConfig fast;
    fast.ConnectTimeoutSeconds = 2.0f;
    fast.TimeoutSeconds = 2.0f;

    // Nobody there.
    {
        NetClientConfig cc;
        cc.Compatibility = Game();
        cc.Transport = fast;
        NetClient client(cc);
        DisconnectReason reason = DisconnectReason::None;
        client.OnDisconnected = [&](DisconnectReason r, const std::string&) { reason = r; };
        client.Connect(Address(loopback, static_cast<uint16_t>(port + 1)));
        const double start = NetNow();
        Pump(nullptr, { &client }, 6.0, [&] { return reason != DisconnectReason::None; });
        CHECK(reason == DisconnectReason::ConnectionFailed, "no server: '{}' after {:.1f} s ({})", ToString(reason), NetNow() - start,
              client.GetLastDisconnectText());
    }

    // The link dies (100 % loss both ways) after joining: both sides time out.
    NetServerConfig sc;
    sc.Port = port;
    sc.Compatibility = Game();
    sc.Transport = fast;
    NetServer server(sc);
    CHECK(server.AddTransport(MakeTransport(loopback, fast)), "listening");
    NetClientConfig cc;
    cc.Compatibility = Game();
    cc.Transport = fast;
    NetClient client(cc);
    client.Connect(Address(loopback, port));
    Pump(&server, { &client }, 5.0, [&] { return client.IsConnected(); });
    CHECK(client.IsConnected(), "joined");
    DisconnectReason clientReason = DisconnectReason::None, serverReason = DisconnectReason::None;
    client.OnDisconnected = [&](DisconnectReason r, const std::string&) { clientReason = r; };
    server.OnClientLeft = [&](const NetClientInfo&, DisconnectReason r, const std::string&) { serverReason = r; };
    server.SetEmulation({ 0.0f, 0.0f, 100.0f });
    client.SetEmulation({ 0.0f, 0.0f, 100.0f });
    // Game traffic every frame both ways (as a game sends input and snapshots each tick): the transport notices the silence.
    const double start = NetNow();
    std::vector<uint8_t> tick = { GameMessage, 0, 0, 0, 0 };
    Pump(&server, { &client }, 8.0, [&]
    {
        server.Broadcast(tick, Delivery::Unreliable);
        client.Send(tick, Delivery::Unreliable);
        return clientReason != DisconnectReason::None && serverReason != DisconnectReason::None;
    });
    CHECK(clientReason == DisconnectReason::TimedOut && serverReason == DisconnectReason::TimedOut,
          "dead link: client '{}', server '{}' after {:.1f} s", ToString(clientReason), ToString(serverReason), NetNow() - start);
    server.SetEmulation({});
    client.SetEmulation({});

    // The same with no game traffic at all (an idle lobby): the transport's own probes find the dead link.
    NetClient idle(cc);
    idle.Connect(Address(loopback, port));
    Pump(&server, { &idle }, 5.0, [&] { return idle.IsConnected(); });
    CHECK(idle.IsConnected(), "idle client joined");
    Pump(&server, { &idle }, 1.5, nullptr); // quiet for a while first
    DisconnectReason idleReason = DisconnectReason::None;
    serverReason = DisconnectReason::None;
    idle.OnDisconnected = [&](DisconnectReason r, const std::string&) { idleReason = r; };
    server.SetEmulation({ 0.0f, 0.0f, 100.0f });
    idle.SetEmulation({ 0.0f, 0.0f, 100.0f });
    const double idleStart = NetNow();
    Pump(&server, { &idle }, 8.0, [&] { return idleReason != DisconnectReason::None && serverReason != DisconnectReason::None; });
    CHECK(idleReason == DisconnectReason::TimedOut && serverReason == DisconnectReason::TimedOut,
          "dead idle link: client '{}', server '{}' after {:.1f} s", ToString(idleReason), ToString(serverReason), NetNow() - idleStart);
    server.SetEmulation({});
    idle.SetEmulation({});
}

static void TestShutdown(bool loopback, uint16_t port)
{
    std::printf("\n== server shutdown (%s)\n", loopback ? "loopback" : "GNS");
    NetClientConfig cc;
    cc.Compatibility = Game();
    NetClient client(cc);
    DisconnectReason reason = DisconnectReason::None;
    client.OnDisconnected = [&](DisconnectReason r, const std::string&) { reason = r; };
    {
        NetServerConfig sc;
        sc.Port = port;
        sc.Compatibility = Game();
        auto server = std::make_unique<NetServer>(sc);
        server->AddTransport(MakeTransport(loopback));
        client.Connect(Address(loopback, port));
        Pump(server.get(), { &client }, 5.0, [&] { return client.IsConnected(); });
        CHECK(client.IsConnected(), "joined");
        server.reset();
    }
    Pump(nullptr, { &client }, 5.0, [&] { return reason != DisconnectReason::None; });
    CHECK(reason == DisconnectReason::ServerShutdown, "client: '{}' ({})", ToString(reason), client.GetLastDisconnectText());
}

static void TestReaderFuzz()
{
    std::printf("\n== reader fuzz\n");
    std::mt19937 random(7);
    int accepted = 0;
    for (int i = 0; i < 200000; ++i)
    {
        std::vector<uint8_t> bytes(random() % 48);
        for (uint8_t& b : bytes) b = static_cast<uint8_t>(random());
        if (i % 3 == 0 && bytes.size() >= 8) // a valid magic and protocol sometimes, to reach the strings
        {
            const uint32_t magic = ProtocolMagic, protocol = ProtocolVersion;
            std::memcpy(bytes.data(), &magic, 4);
            std::memcpy(bytes.data() + 4, &protocol, 4);
        }
        NetReader reader(bytes);
        NetHello hello;
        accepted += ReadHello(reader, hello) ? 1 : 0;
        NetReader reader2(bytes);
        NetWelcome welcome;
        ReadWelcome(reader2, welcome);
    }
    CHECK(true, "200000 random Hello/Welcome parses without a crash ({} random ones were well-formed)", accepted);

    NetWriter writer;
    writer.WriteVarU64(0); writer.WriteVarU64(127); writer.WriteVarU64(128); writer.WriteVarU64(UINT64_MAX);
    writer.WriteString("héllo"); writer.WriteFloat(-1.5f); writer.WriteU16(0xBEEF);
    NetReader reader(writer.GetData());
    const bool roundTrip = reader.ReadVarU64() == 0 && reader.ReadVarU64() == 127 && reader.ReadVarU64() == 128 &&
                           reader.ReadVarU64() == UINT64_MAX && reader.ReadString(16) == "héllo" && reader.ReadFloat() == -1.5f &&
                           reader.ReadU16() == 0xBEEF && reader.IsAtEnd();
    CHECK(roundTrip, "writer/reader round trip");
    NetReader shortReader(std::span(writer.GetData().data(), 3));
    shortReader.ReadVarU64(); shortReader.ReadVarU64(); shortReader.ReadVarU64(); shortReader.ReadVarU64();
    CHECK(shortReader.IsFailed() && shortReader.ReadU32() == 0, "a cut message fails the reader and reads zeros");
}

// Transform quantization: positions exact to the step anywhere (large worlds), rotations to ~0.003 degrees, malformed input refused.
static void TestQuantize()
{
    std::printf("\n== transform quantization\n");
    std::mt19937 random(99);
    std::uniform_real_distribution<double> near(-1.0e4, 1.0e4), far(-1.0e9, 1.0e9);
    const double quantum = 0.01; // 0.1 mm in a centimeter project
    double worstPosition = 0.0;
    size_t worstBytes = 0, nearBytes = 0;
    for (int i = 0; i < 100000; ++i)
    {
        const glm::dvec3 position = i % 2 ? glm::dvec3(near(random), near(random), near(random)) : glm::dvec3(far(random), far(random), far(random));
        const std::vector<uint8_t> bytes = EncodePosition(position, quantum);
        glm::dvec3 decoded(0.0);
        if (!DecodePosition(bytes, quantum, decoded)) { worstPosition = 1e30; break; }
        const glm::dvec3 error = glm::abs(decoded - position);
        worstPosition = std::max({ worstPosition, error.x, error.y, error.z });
        worstBytes = std::max(worstBytes, bytes.size());
        if (i % 2) nearBytes = std::max(nearBytes, bytes.size());
    }
    CHECK(worstPosition <= quantum * 0.5 + 1e-6, "positions up to 10^9 units: worst error {:.4f} (half a step: {:.4f}); at most {} bytes, {} within 100 m",
          worstPosition, quantum * 0.5, worstBytes, nearBytes);

    std::normal_distribution<float> gauss;
    float worstDegrees = 0.0f;
    for (int i = 0; i < 100000; ++i)
    {
        const glm::quat rotation = glm::normalize(glm::quat(gauss(random), gauss(random), gauss(random), gauss(random)));
        glm::quat decoded;
        const std::vector<uint8_t> bytes = EncodeRotation(rotation);
        if (bytes.size() != 7 || !DecodeRotation(bytes, decoded)) { worstDegrees = 1e9f; break; }
        // The angle between them in double, both normalized in double first: a float-normalized quaternion is only unit to ~3e-7,
        // which acos turns into ~0.09 degrees by itself -- more than the encoding loses.
        const glm::dvec4 a = glm::normalize(glm::dvec4(rotation.x, rotation.y, rotation.z, rotation.w));
        const glm::dvec4 b = glm::normalize(glm::dvec4(decoded.x, decoded.y, decoded.z, decoded.w));
        const double dot = std::min(1.0, std::abs(glm::dot(a, b)));
        worstDegrees = std::max(worstDegrees, static_cast<float>(2.0 * std::acos(dot) * 57.29577951308232));
    }
    CHECK(worstDegrees < 0.01f, "rotations in 7 bytes: worst error {:.4f} degrees", worstDegrees);

    glm::dvec3 untouched(1.0, 2.0, 3.0);
    const std::vector<uint8_t> cut = { 0x80 };
    glm::quat rotationUntouched(1, 0, 0, 0);
    const std::vector<uint8_t> badRotation = { 7, 0, 0, 0, 0, 0, 0 };
    glm::vec3 scale(1.0f);
    const std::vector<uint8_t> nanScale = EncodeScale(glm::vec3(std::nanf(""), 1.0f, 1.0f));
    const bool refused = !DecodePosition(cut, quantum, untouched) && untouched == glm::dvec3(1.0, 2.0, 3.0) &&
                         !DecodeRotation(badRotation, rotationUntouched) && !DecodeScale(nanScale, scale) && scale == glm::vec3(1.0f);
    glm::dvec3 nanPosition(0.0);
    const bool nanSafe = DecodePosition(EncodePosition(glm::dvec3(std::nan(""), 5.0, 0.0), quantum), quantum, nanPosition) && nanPosition.y == 5.0;
    CHECK(refused && nanSafe, "malformed bytes leave the transform as it was; a NaN position is sent as 0");
}

// Prediction and reconciliation between a toy client and server (a character that moves by its input each tick; deterministic, like
// the real one on both sides): inputs and snapshots over a channel with latency, jitter and loss.
struct PredictionRun
{
    size_t Corrections = 0;
    size_t Acknowledged = 0;
    double WorstError = 0.0;  // between the client's predicted and the server's position for the same input, before correcting
    double FinalGap = 0.0;    // client vs server once both are at rest
};

static PredictionRun RunPrediction(int latencyTicks, int jitterTicks, float loss, int pushAtTick, uint32_t seed)
{
    std::mt19937 random(seed);
    auto chance = [&](float p) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(random) < p; };
    const float dt = 1.0f / 60.0f;
    auto step = [&](glm::dvec3 position, const InputFrame& input) { return position + glm::dvec3(input.Move) * static_cast<double>(dt); };

    struct Message { int DeliverAt; std::vector<uint8_t> Bytes; };
    std::vector<Message> toServer, toClient;
    PredictionHistory history;
    InputQueue queue;
    glm::dvec3 client(0.0), server(0.0);
    uint32_t sequence = 0;
    uint32_t newestSnapshot = 0;
    PredictionRun run;
    const int moveTicks = 1200, totalTicks = 1500;
    for (int tick = 1; tick <= totalTicks; ++tick)
    {
        // Client: this tick's input (a direction that changes every 50 ticks; at rest at the end), applied at once.
        InputFrame input;
        input.Sequence = ++sequence;
        if (tick <= moveTicks)
        {
            const float angle = static_cast<float>((tick / 50) % 8) * 0.785f;
            input.Move = glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * 500.0f;
        }
        client = step(client, input);
        history.Record(input, client);
        NetWriter inputs;
        WriteInputs(inputs, 42, history.NewestInputs(InputRedundancy));
        if (!chance(loss))
            toServer.push_back({ tick + latencyTicks + (jitterTicks ? static_cast<int>(random() % (jitterTicks + 1)) : 0),
                                 std::vector<uint8_t>(inputs.GetData().begin(), inputs.GetData().end()) });

        // Server: the inputs that arrived, each applied once (Unreal's server moves); a snapshot every other tick.
        std::stable_sort(toServer.begin(), toServer.end(), [](const Message& a, const Message& b) { return a.DeliverAt < b.DeliverAt; });
        while (!toServer.empty() && toServer.front().DeliverAt <= tick)
        {
            NetReader reader(toServer.front().Bytes);
            reader.ReadU8();
            NetId entity = 0;
            std::vector<InputFrame> frames;
            if (ReadInputs(reader, entity, frames))
                queue.Push(frames);
            toServer.erase(toServer.begin());
        }
        for (const InputFrame& frame : queue.Take(MaxInputsPerTick))
            server = step(server, frame);
        if (tick == pushAtTick)
            server.x += 100.0; // something only the server knows about pushed the character (a collision, a knock-back)
        if (tick % 2 == 0 && !chance(loss))
        {
            NetWriter snapshot;
            snapshot.WriteU32(queue.GetLastProcessed());
            snapshot.WriteBytes(EncodePosition(server, 0.01));
            toClient.push_back({ tick + latencyTicks + (jitterTicks ? static_cast<int>(random() % (jitterTicks + 1)) : 0),
                                 std::vector<uint8_t>(snapshot.GetData().begin(), snapshot.GetData().end()) });
        }

        // Client: snapshots that arrived (late ones skipped), each compared with the prediction for its input.
        std::stable_sort(toClient.begin(), toClient.end(), [](const Message& a, const Message& b) { return a.DeliverAt < b.DeliverAt; });
        while (!toClient.empty() && toClient.front().DeliverAt <= tick)
        {
            NetReader reader(toClient.front().Bytes);
            const uint32_t acknowledged = reader.ReadU32();
            glm::dvec3 position;
            DecodePosition(reader.ReadBytes(reader.GetRemaining()), 0.01, position);
            toClient.erase(toClient.begin());
            if (acknowledged <= newestSnapshot || acknowledged == 0)
                continue;
            newestSnapshot = acknowledged;
            ++run.Acknowledged;
            std::optional<std::vector<InputFrame>> replay = history.Reconcile(acknowledged, position, 0.02);
            if (!replay)
                continue;
            ++run.Corrections;
            client = position;
            for (const InputFrame& frame : *replay)
            {
                client = step(client, frame);
                history.Rewrite(frame.Sequence, client);
            }
        }
    }
    run.FinalGap = glm::distance(client, server);
    return run;
}

static void TestPrediction()
{
    std::printf("\n== prediction and reconciliation\n");
    {
        PredictionRun run = RunPrediction(6, 0, 0.0f, 0, 1); // 100 ms each way, steady
        CHECK(run.Corrections == 0 && run.FinalGap < 0.02, "100 ms each way, no jitter or loss: {} snapshots checked, {} corrections, final gap {:.3f}",
              run.Acknowledged, run.Corrections, run.FinalGap);
    }
    {
        PredictionRun run = RunPrediction(5, 4, 0.05f, 0, 2); // 83-150 ms each way, 5 % loss both ways
        CHECK(run.FinalGap < 0.02 && run.Corrections == 0,
              "83-150 ms each way, 5 % loss: {} corrections in {} snapshots (every input applied once, lost ones covered by the repeats), final gap {:.3f}",
              run.Corrections, run.Acknowledged, run.FinalGap);
    }
    {
        PredictionRun run = RunPrediction(6, 0, 0.0f, 600, 3); // the server pushes the character once
        CHECK(run.Corrections >= 1 && run.Corrections <= 2 && run.FinalGap < 0.02,
              "a push only the server saw: {} correction(s), then the client matches the server (final gap {:.3f})", run.Corrections, run.FinalGap);
    }

    // Input messages: garbage and NaN are refused.
    NetWriter writer;
    InputFrame bad;
    bad.Sequence = 1;
    bad.Move.x = std::nanf("");
    WriteInputs(writer, 7, std::span(&bad, 1));
    NetReader reader(writer.GetData());
    reader.ReadU8();
    NetId entity = 0;
    std::vector<InputFrame> frames;
    CHECK(!ReadInputs(reader, entity, frames), "an input with a NaN is refused");
}

// The clock: snapshots every 2 ticks arriving 50-80 ms late (uniform jitter); the estimate should follow the earliest arrivals.
static void TestClockAndInterpolation()
{
    std::printf("\n== clock and interpolation\n");
    std::mt19937 random(5);
    NetClock clock;
    InterpolationBuffer buffer;
    const double speed = 300.0; // world units a second along x
    struct Arrival { double At; uint64_t Tick; };
    std::vector<Arrival> arrivals;
    for (uint64_t tick = 0; tick < 60 * 20; tick += 2)
        arrivals.push_back({ tick / 60.0 + 0.050 + std::uniform_real_distribution<double>(0.0, 0.030)(random), tick });
    std::sort(arrivals.begin(), arrivals.end(), [](const Arrival& a, const Arrival& b) { return a.At < b.At; });

    // Render at 144 frames a second, 4 ticks behind the clock (2 snapshot intervals: the jitter must not empty the buffer).
    size_t next = 0;
    double worstClockError = 0.0, worstStepError = 0.0, previousX = -1e30;
    size_t backwards = 0, frames = 0;
    const double frame = 1.0 / 144.0;
    for (double now = 0.0; now < 20.0; now += frame)
    {
        while (next < arrivals.size() && arrivals[next].At <= now)
        {
            const Arrival& arrival = arrivals[next++];
            clock.OnSnapshot(arrival.Tick, arrival.At, 60.0f);
            PoseSample sample;
            sample.Tick = static_cast<double>(arrival.Tick);
            sample.Position = glm::dvec3(speed * arrival.Tick / 60.0, 0.0, 0.0);
            buffer.Add(sample);
        }
        if (!clock.IsValid() || now < 3.0)
            continue; // (settled)
        // The fastest arrivals are 50 ms late: the clock should sit there (within about a tick).
        worstClockError = std::max(worstClockError, std::abs(clock.GetServerTick(now) - (now - 0.050) * 60.0));
        PoseSample pose;
        if (!buffer.Sample(clock.GetServerTick(now) - 4.0, 6.0, 500.0, pose))
            continue;
        if (pose.Position.x < previousX - 1e-9)
            ++backwards;
        if (previousX > -1e29)
            worstStepError = std::max(worstStepError, std::abs((pose.Position.x - previousX) - speed * frame));
        previousX = pose.Position.x;
        ++frames;
    }
    CHECK(worstClockError < 1.5, "the clock follows the earliest arrivals (50-80 ms jitter): worst error {:.2f} ticks", worstClockError);
    CHECK(backwards == 0 && worstStepError < speed * frame * 0.5, "{} frames drawn between snapshots: never backwards, worst step error {:.2f} units (a step is {:.2f})",
          frames, worstStepError, speed * frame);

    // A teleport is taken at once, not swept.
    InterpolationBuffer jump;
    jump.Add({ 0.0, glm::dvec3(0.0) });
    jump.Add({ 2.0, glm::dvec3(10000.0, 0.0, 0.0) });
    PoseSample pose;
    jump.Sample(1.0, 6.0, 500.0, pose);
    CHECK(pose.Position.x == 10000.0, "a 100 m jump between two snapshots is a teleport (no sweep across the level)");
}

// The snapshot codec alone: a world that keeps changing (spawns, removals, property and owner changes, scripts added), snapshots
// over a channel that loses 30 % and delivers the rest up to 4 ticks late, acknowledgements lost and late the same way. Every snapshot
// the client applies must be exactly the server's world at that tick; after the loss stops the client must hold the final world.
static void TestSnapshots()
{
    std::printf("\n== snapshots: deltas against acknowledged baselines, 30 %% loss and reordering both ways\n");
    std::mt19937 random(1234);
    auto chance = [&](float p) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(random) < p; };
    auto randomBlob = [&](size_t maxSize) { std::vector<uint8_t> blob(1 + random() % maxSize); for (uint8_t& b : blob) b = static_cast<uint8_t>(random()); return blob; };

    WorldState world;
    NetId nextId = 1000;
    auto spawn = [&]()
    {
        auto state = std::make_shared<EntityState>();
        state->Prefab = random() % 3 == 0 ? 0 : 0xAB00 + random() % 4;
        state->Owner = random() % 4;
        const size_t properties = 1 + random() % 5;
        for (size_t index = 0; index < properties; ++index)
            state->Properties.push_back(randomBlob(index == 0 ? 24 : 8));
        world.Entities[nextId++] = state;
    };
    for (int i = 0; i < 40; ++i)
        spawn();

    SnapshotSender sender;
    SnapshotReceiver receiver;
    std::unordered_map<uint32_t, WorldState> sentWorlds; // what each sequence carried
    struct InFlight { int DeliverAt; std::vector<uint8_t> Bytes; };
    std::vector<InFlight> toClient, toServer;
    size_t applied = 0, stale = 0, missing = 0, malformed = 0, mismatches = 0, bytes = 0, fullBytes = 0;
    WorldState clientWorld;
    const int lossyTicks = 600, totalTicks = 700;
    for (int tick = 1; tick <= totalTicks; ++tick)
    {
        const bool lossy = tick <= lossyTicks;
        // The world changes.
        world.Tick = static_cast<uint64_t>(tick);
        for (auto it = world.Entities.begin(); it != world.Entities.end();)
        {
            if (lossy && chance(0.01f)) { it = world.Entities.erase(it); continue; }
            if (lossy && chance(0.2f))
            {
                auto changed = std::make_shared<EntityState>(*it->second);
                changed->Properties[random() % changed->Properties.size()] = randomBlob(16);
                if (chance(0.05f)) changed->Owner = random() % 4;
                if (chance(0.02f)) changed->Properties.push_back(randomBlob(8)); // a script added
                it->second = changed;
            }
            ++it;
        }
        if (lossy && chance(0.3f))
            spawn();

        // The server sends.
        NetWriter out;
        sender.Write(world, out);
        sentWorlds[sender.GetLastSent()] = world;
        bytes += out.GetSize();
        {
            SnapshotSender full; // the same world from nothing: what no deltas would cost
            NetWriter fullOut;
            full.Write(world, fullOut);
            fullBytes += fullOut.GetSize();
        }
        if (!lossy || !chance(0.3f))
            toClient.push_back({ tick + (lossy ? static_cast<int>(random() % 5) : 0), std::vector<uint8_t>(out.GetData().begin(), out.GetData().end()) });

        // The client receives what arrived by now (in arrival order) and acknowledges.
        std::stable_sort(toClient.begin(), toClient.end(), [](const InFlight& a, const InFlight& b) { return a.DeliverAt < b.DeliverAt; });
        while (!toClient.empty() && toClient.front().DeliverAt <= tick)
        {
            NetReader reader(toClient.front().Bytes);
            reader.ReadU8(); // the type
            WorldState decoded;
            uint32_t sequence = 0;
            switch (receiver.Read(reader, decoded, sequence))
            {
            case SnapshotReceiver::Result::Applied:
            {
                ++applied;
                const WorldState& expected = sentWorlds[sequence];
                bool same = decoded.Tick == expected.Tick && decoded.Entities.size() == expected.Entities.size();
                for (const auto& [id, state] : expected.Entities)
                {
                    auto found = decoded.Entities.find(id);
                    same = same && found != decoded.Entities.end() && *found->second == *state;
                }
                mismatches += same ? 0 : 1;
                clientWorld = decoded;
                NetWriter ack;
                WriteSnapshotAck(ack, sequence);
                if (!lossy || !chance(0.3f))
                    toServer.push_back({ tick + (lossy ? static_cast<int>(random() % 5) : 0), std::vector<uint8_t>(ack.GetData().begin(), ack.GetData().end()) });
                break;
            }
            case SnapshotReceiver::Result::Stale: ++stale; break;
            case SnapshotReceiver::Result::MissingBaseline: ++missing; break;
            case SnapshotReceiver::Result::Malformed: ++malformed; break;
            }
            toClient.erase(toClient.begin());
        }
        std::stable_sort(toServer.begin(), toServer.end(), [](const InFlight& a, const InFlight& b) { return a.DeliverAt < b.DeliverAt; });
        while (!toServer.empty() && toServer.front().DeliverAt <= tick)
        {
            NetReader reader(toServer.front().Bytes);
            reader.ReadU8();
            sender.Acknowledge(reader.ReadU32());
            toServer.erase(toServer.begin());
        }
    }
    CHECK(malformed == 0 && mismatches == 0, "{} snapshots applied, each exactly the server's world ({} mismatches, {} malformed); {} late ones skipped, {} without their baseline",
          applied, mismatches, malformed, stale, missing);
    bool converged = clientWorld.Entities.size() == world.Entities.size();
    for (const auto& [id, state] : world.Entities)
    {
        auto found = clientWorld.Entities.find(id);
        converged = converged && found != clientWorld.Entities.end() && *found->second == *state;
    }
    CHECK(converged, "after the loss stops the client holds the server's world ({} entities)", world.Entities.size());
    // (A churny world -- a fifth of the entities change every tick, baselines several ticks old under the loss: a level where most
    // things stand still sends far less.)
    CHECK(bytes * 2 < fullBytes, "deltas: {} KB sent, {} KB without them ({:.0f} %)", bytes / 1024, fullBytes / 1024, 100.0 * bytes / fullBytes);

    // Random bytes never crash the reader or apply anything.
    SnapshotReceiver fuzzed;
    size_t accepted = 0;
    for (int i = 0; i < 100000; ++i)
    {
        std::vector<uint8_t> junk(random() % 64);
        for (uint8_t& b : junk) b = static_cast<uint8_t>(random());
        NetReader reader(junk);
        WorldState decoded;
        uint32_t sequence = 0;
        accepted += fuzzed.Read(reader, decoded, sequence) == SnapshotReceiver::Result::Applied ? 1 : 0;
    }
    CHECK(true, "100000 random snapshots read without a crash ({} happened to be well-formed)", accepted);
}

int main()
{
    Nox::Log::Init();
    TestReaderFuzz();
    TestSnapshots();
    TestQuantize();
    TestPrediction();
    TestClockAndInterpolation();
    for (bool loopback : { true, false })
    {
        const uint16_t base = loopback ? 7100 : 7200;
        TestHandshake(loopback, base + 0);
        TestEmulation(loopback, base + 10);
        TestRefusals(loopback, base + 20);
        TestBadClients(loopback, base + 30);
        TestTimeouts(loopback, base + 40);
        TestShutdown(loopback, base + 50);
    }
    std::printf("\n%d checks, %d failed\n", s_Checks, s_Failures);
    return s_Failures == 0 ? 0 : 1;
}
