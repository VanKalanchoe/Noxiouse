#pragma once
#include "NetBuffer.h"
#include "NetProtocol.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// Snapshots (docs/Networking_Plan_2026-11.md, NET2): how the server tells a client what its replicated entities are now. The
// Quake 3 / Unreal Iris model: each snapshot is a delta against the newest snapshot the client acknowledged (its baseline), sent
// unreliably; a lost snapshot costs nothing (the next one is a delta against an older baseline and carries the change again), so
// spawns, removals and property changes always converge without any reliable stream. Nothing here knows the scene: an entity's
// state is opaque property blobs (NetReplication encodes transforms and script fields into them), compared byte for byte.
//
// Snapshot message (type MessageType::Snapshot, server -> client, unreliable):
//   u32 sequence, u32 baseline (0: none -- relative to an empty world), varint server tick, varint record count, records:
//   varint netId, u8 kind:
//     New      varint prefab, varint owner, varint property count, properties (varint length + bytes each)
//     Changed  u8 flags (1: owner follows), [varint owner], varint property count, bitmask of the changed properties
//              (count + 7) / 8 bytes, the changed ones (varint length + bytes each)
//     Removed  (nothing)
// Ack message (type MessageType::SnapshotAck, client -> server, unreliable): u32 sequence of the newest snapshot it decoded.
namespace Nox::Net
{
    using NetId = uint64_t; // a replicated entity's id: its UUID (the same on the server and every client)

    // A replicated entity's state as the server sends it.
    struct EntityState
    {
        uint64_t Prefab = 0;   // the prefab a client spawns it from; 0: a level entity every client already has
        ClientId Owner = InvalidClientId;
        std::vector<std::vector<uint8_t>> Properties; // [0] the transform, then the scripts' replicated fields (NetReplication)

        bool operator==(const EntityState& other) const = default;
    };
    using EntityStatePtr = std::shared_ptr<const EntityState>;

    // The replicated world at one moment: what a snapshot says the client should have. Unchanged entities share their state between
    // snapshots (the pointers), so keeping a few dozen of them costs little.
    struct WorldState
    {
        uint64_t Tick = 0;
        std::unordered_map<NetId, EntityStatePtr> Entities;
    };

    constexpr size_t SnapshotHistory = 32; // snapshots remembered as baselines (~1 s at 30 snapshots a second)
    constexpr size_t MaxProperties = 1024; // per entity (a malformed count beyond this fails the read)

    // The server's side for one client.
    class SnapshotSender
    {
    public:
        // Writes the whole Snapshot message for `world` (type byte included), relative to the client's newest acknowledged
        // snapshot still remembered (else to an empty world), and remembers it as a possible baseline.
        void Write(const WorldState& world, NetWriter& out);
        // The client decoded snapshot `sequence`: it may serve as the next baseline. Older or unknown ones are ignored.
        void Acknowledge(uint32_t sequence);
        uint32_t GetLastSent() const { return m_NextSequence - 1; }
        uint32_t GetLastAcknowledged() const { return m_Acknowledged; }

    private:
        struct Sent
        {
            uint32_t Sequence = 0;
            WorldState World;
        };
        const WorldState* FindBaseline(uint32_t sequence) const;

        std::array<Sent, SnapshotHistory> m_Sent{};
        uint32_t m_NextSequence = 1;
        uint32_t m_Acknowledged = 0;
    };

    // The client's side.
    class SnapshotReceiver
    {
    public:
        enum class Result : uint8_t
        {
            Applied,         // a newer snapshot: `world` is the server's world at its tick
            Stale,           // older than one already applied (arrived late): ignored
            MissingBaseline, // its baseline is no longer (or never was) here: ignored, the server will send against another
            Malformed,       // not a valid snapshot: the session drops the connection
        };
        // Reads a Snapshot message (the reader after the type byte).
        Result Read(NetReader& reader, WorldState& world, uint32_t& sequence);
        uint32_t GetLastApplied() const { return m_LastApplied; }
        // Forgets everything (a new session).
        void Reset();

    private:
        struct Received
        {
            uint32_t Sequence = 0;
            WorldState World;
        };
        std::array<Received, SnapshotHistory> m_Received{};
        uint32_t m_LastApplied = 0;
    };

    // The Ack message for a snapshot (type byte included).
    void WriteSnapshotAck(NetWriter& out, uint32_t sequence);
}
