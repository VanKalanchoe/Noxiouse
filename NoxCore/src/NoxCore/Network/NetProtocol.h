#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "NetBuffer.h"

// The session protocol (docs/Networking_Plan_2026-11.md, NET1): what a client and a server say to each other before the game's
// own messages (NET2's replication and RPCs). Every message starts with its type byte.
//
//   client                                   server
//   (transport connects)                     (accepts; a full server closes at once: ServerFull)
//   Hello: magic, protocol, project name,    checks the protocol, the project and the content checksum; refuses with a reason
//          content checksum, player name     (VersionMismatch / ContentMismatch / ProtocolError), else assigns a client id
//                                      <---  Welcome: client id, server tick, tick rate, scene, server name
//   (loads the server's scene)
//
// A connection that doesn't finish the handshake within HandshakeTimeoutSeconds is closed (HandshakeTimeout). Disconnects carry a
// DisconnectReason in the transport's close (the peer receives it even when the connection ends without a last message).
namespace Nox::Net
{
    constexpr uint32_t ProtocolMagic = 0x4e584f4e;  // "NOXN"
    constexpr uint32_t ProtocolVersion = 1;         // raised whenever a message's layout changes
    constexpr uint16_t DefaultPort = 7777;
    constexpr size_t MaxNameLength = 64;            // player and server names
    constexpr size_t MaxProjectNameLength = 128;

    enum class MessageType : uint8_t
    {
        Hello = 1,
        Welcome = 2,
        // The game's messages (replication, RPCs: NET2) start here; types below are the session's own.
        FirstGameMessage = 32,
    };

    // Why a session ended (sent in the transport's close; Unreal's ENetworkFailure / the login failure strings).
    enum class DisconnectReason : uint16_t
    {
        None = 0,
        Quit,             // the client left
        ServerShutdown,   // the server stopped
        Kicked,           // the server removed the client (the text says why)
        ServerFull,
        VersionMismatch,  // a different engine network protocol
        ContentMismatch,  // a different game: project name or content checksum (scripts) differ
        HandshakeTimeout, // connected, but the handshake did not finish in time
        ProtocolError,    // a malformed or unexpected message
        TimedOut,         // the peer stopped answering
        ConnectionFailed, // the server never answered (wrong address, no server, firewall)
        ConnectionLost,   // the transport ended the connection for another reason (the text says which)
        Count
    };

    inline const char* ToString(DisconnectReason reason)
    {
        switch (reason)
        {
        case DisconnectReason::None: return "none";
        case DisconnectReason::Quit: return "left";
        case DisconnectReason::ServerShutdown: return "server shut down";
        case DisconnectReason::Kicked: return "kicked";
        case DisconnectReason::ServerFull: return "server full";
        case DisconnectReason::VersionMismatch: return "version mismatch";
        case DisconnectReason::ContentMismatch: return "content mismatch";
        case DisconnectReason::HandshakeTimeout: return "handshake timed out";
        case DisconnectReason::ProtocolError: return "protocol error";
        case DisconnectReason::TimedOut: return "timed out";
        case DisconnectReason::ConnectionFailed: return "connection failed";
        case DisconnectReason::ConnectionLost: return "connection lost";
        default: return "unknown";
        }
    }

    // What makes a client fit a server (Unreal's network version checksum): the engine's protocol, the project, and a checksum of
    // the content both must share (the game's script assemblies: replicated fields and RPCs come from them).
    struct NetCompatibility
    {
        uint32_t Protocol = ProtocolVersion;
        std::string ProjectName;
        uint64_t ContentChecksum = 0;
    };

    using ClientId = uint32_t;
    constexpr ClientId InvalidClientId = 0;

    // The server's answer to a client's Hello.
    struct NetWelcome
    {
        ClientId Client = InvalidClientId;
        uint64_t ServerTick = 0;  // Scene::GetTick() on the server when it answered
        uint16_t TickRate = 60;   // simulation ticks a second
        uint64_t SceneHandle = 0; // the scene asset the server plays (the client loads it)
        std::string ServerName;
        uint16_t MaxClients = 0;
    };

    struct NetHello
    {
        uint32_t Magic = 0;
        NetCompatibility Compatibility;
        std::string PlayerName;
    };

    // The sessions' clock: seconds, steady (time-outs, join times).
    inline double NetNow()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The handshake's messages, with their type byte; Read* get the reader after the type byte and fail on anything malformed
    // (a field cut short, a name too long, bytes left over).
    inline void WriteHello(NetWriter& writer, const NetCompatibility& compatibility, std::string_view playerName)
    {
        writer.WriteU8(static_cast<uint8_t>(MessageType::Hello));
        writer.WriteU32(ProtocolMagic);
        writer.WriteU32(compatibility.Protocol);
        writer.WriteString(compatibility.ProjectName.substr(0, MaxProjectNameLength));
        writer.WriteU64(compatibility.ContentChecksum);
        writer.WriteString(playerName.substr(0, MaxNameLength));
    }

    inline bool ReadHello(NetReader& reader, NetHello& hello)
    {
        hello.Magic = reader.ReadU32();
        if (hello.Magic != ProtocolMagic)
            return false; // not a Nox client: nothing after the magic is read
        hello.Compatibility.Protocol = reader.ReadU32();
        // A client of another protocol version may lay out the rest differently: the version is what the server checks first.
        if (hello.Compatibility.Protocol != ProtocolVersion)
            return !reader.IsFailed();
        hello.Compatibility.ProjectName = reader.ReadString(MaxProjectNameLength);
        hello.Compatibility.ContentChecksum = reader.ReadU64();
        hello.PlayerName = reader.ReadString(MaxNameLength);
        return reader.IsAtEnd();
    }

    inline void WriteWelcome(NetWriter& writer, const NetWelcome& welcome)
    {
        writer.WriteU8(static_cast<uint8_t>(MessageType::Welcome));
        writer.WriteU32(welcome.Client);
        writer.WriteU64(welcome.ServerTick);
        writer.WriteU16(welcome.TickRate);
        writer.WriteU64(welcome.SceneHandle);
        writer.WriteString(std::string_view(welcome.ServerName).substr(0, MaxNameLength));
        writer.WriteU16(welcome.MaxClients);
    }

    inline bool ReadWelcome(NetReader& reader, NetWelcome& welcome)
    {
        welcome.Client = reader.ReadU32();
        welcome.ServerTick = reader.ReadU64();
        welcome.TickRate = reader.ReadU16();
        welcome.SceneHandle = reader.ReadU64();
        welcome.ServerName = reader.ReadString(MaxNameLength);
        welcome.MaxClients = reader.ReadU16();
        return reader.IsAtEnd() && welcome.Client != InvalidClientId && welcome.TickRate > 0;
    }
}
