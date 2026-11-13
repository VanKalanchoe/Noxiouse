#pragma once
#include "NetBuffer.h"
#include "NetProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// Snapshots (docs/Networking_Plan_2026-11.md, NET2, NET4): how the server tells a client what its replicated entities are now. Unreal
// Iris's model -- every entity has its own acknowledged state:
//   - the server remembers, per client and per entity, the newest state the client acknowledged and the states sent since whose fate
//     is not known yet;
//   - an entity is in a snapshot only when its state differs from what the client is believed to hold (the newest one sent that was
//     not lost): a change is sent once, not repeated until acknowledged;
//   - the client acknowledges the newest snapshot it applied and which of the 32 before it it applied too, so the server knows which
//     were lost -- and sends again only what a lost one carried and nothing newer has replaced;
//   - a change is written against the acknowledged state with every property any state the client may hold differs in (the
//     acknowledged one or one in flight), so it applies to whichever it holds;
//   - the client applies snapshots in order (an older one arriving late is dropped) and keeps what a snapshot does not mention.
// Snapshots are unreliable; spawns, removals and property changes converge under any loss. Nothing here knows the scene: an
// entity's state is opaque property blobs (NetReplication encodes transforms and script fields into them), compared byte for byte.
//
// Snapshot message (type MessageType::Snapshot, server -> client, unreliable):
//   u32 sequence, varint server tick, varint record count, records:
//   varint netId, u8 kind:
//     New      varint prefab, varint owner, varint property count, properties (varint length + bytes each)
//     Changed  u8 flags (1: owner follows), [varint owner], varint property count, bitmask of the changed properties
//              (count + 7) / 8 bytes, the changed ones (varint length + bytes each)
//     Removed  (nothing)
// Ack message (type MessageType::SnapshotAck, client -> server, unreliable): u32 the newest snapshot applied, u32 bits (bit i: the
// snapshot i + 1 before it was applied too).
//
// Bandwidth (NET4; Unreal's NetPriority and per-connection rate): a snapshot may be limited to a number of bytes. Its records are then
// written by priority -- each entity's priority accumulates every snapshot its change waits, so everything is sent eventually (Unreal's
// priority grows with the time since an actor was last replicated) -- and what does not fit waits for a later snapshot.
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

    // The replicated world at one moment. Unchanged entities share their state between worlds (the pointers), so keeping several
    // costs little.
    struct WorldState
    {
        uint64_t Tick = 0;
        std::unordered_map<NetId, EntityStatePtr> Entities;
    };

    constexpr size_t SnapshotHistory = 64;     // snapshots whose fate the server waits for (a snapshot older than the ack's 32 is lost)
    constexpr size_t AckWindow = 32;           // snapshots before the newest an ack reports on
    constexpr size_t MaxProperties = 1024;     // per entity (a malformed count beyond this fails the read)
    constexpr size_t SnapshotHeaderBytes = 20; // the most a Snapshot message's header takes (type, sequence, tick, count)

    // What one snapshot may carry.
    struct SnapshotLimits
    {
        size_t MaxBytes = SIZE_MAX; // the whole message; at least one record is always written (a record bigger than this alone)
        const std::unordered_map<NetId, float>* Priorities = nullptr; // per entity (missing: 1)
    };
    struct SnapshotWriteStats
    {
        size_t Records = 0;  // written (removals included)
        size_t Deferred = 0; // changes that did not fit and wait for a later snapshot
    };

    // The server's side for one client.
    class SnapshotSender
    {
    public:
        // Writes the whole Snapshot message (type byte included) that brings the client from what it is believed to hold to `world`
        // -- all of it, or as much as `limits` allows.
        void Write(const WorldState& world, NetWriter& out, const SnapshotLimits& limits = {}, SnapshotWriteStats* stats = nullptr);
        // The client's ack: `newest` applied, and the 32 before it per `bits`. The snapshots it reports as not applied (and those
        // older than its window still unreported) are lost: what they carried is sent again unless something newer replaced it.
        void Acknowledge(uint32_t newest, uint32_t bits);
        uint32_t GetLastSent() const { return m_NextSequence - 1; }
        uint32_t GetLastAcknowledged() const { return m_Acknowledged; }
        // The entities the client is believed to hold (tests, stats).
        size_t GetTrackedCount() const { return m_Entities.size(); }

    private:
        struct SentState
        {
            uint32_t Sequence = 0;
            EntityStatePtr State; // null: removed (or never had it)
            float Key = 0.0f;     // the priority it had waited up to when sent (a lost one is sent again from there, not from 0)
        };
        struct Tracked
        {
            SentState Acked;                // the newest the client acknowledged
            std::vector<SentState> Pending; // sent after it, fate unknown (oldest first)
            float Waiting = 0.0f;           // accumulated priority of a change that did not fit
            const EntityStatePtr& Believed() const { return Pending.empty() ? Acked.State : Pending.back().State; }
        };
        struct Sent
        {
            uint32_t Sequence = 0;
            bool Resolved = true;
            std::vector<NetId> Entities; // what it carried
        };
        void Resolve(Sent& sent, bool applied);

        std::unordered_map<NetId, Tracked> m_Entities;
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
            Applied,   // a newer snapshot: `world` is the client's world after it
            Stale,     // older than one already applied (arrived late): ignored
            Malformed, // not a valid snapshot: the session drops the connection
        };
        // Reads a Snapshot message (the reader after the type byte).
        Result Read(NetReader& reader, WorldState& world, uint32_t& sequence);
        uint32_t GetLastApplied() const { return m_LastApplied; }
        // Changes for entities this client does not hold, skipped (0 unless acks were lost for a whole window).
        size_t GetUnknownChanges() const { return m_UnknownChanges; }
        // Forgets everything (a new session).
        void Reset();
        // The Ack message for what was applied so far (type byte included).
        void WriteAck(NetWriter& out) const;

    private:
        WorldState m_World;
        uint32_t m_LastApplied = 0;
        uint64_t m_AppliedBits = 0; // bit i: snapshot m_LastApplied - 1 - i applied
        size_t m_UnknownChanges = 0;
    };

    // Reads an Ack message (the reader after the type byte).
    bool ReadSnapshotAck(NetReader& reader, uint32_t& newest, uint32_t& bits);
}
