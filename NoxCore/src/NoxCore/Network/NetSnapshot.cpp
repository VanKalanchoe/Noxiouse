#include "NetSnapshot.h"

#include <algorithm>

namespace Nox::Net
{
    namespace
    {
        enum class RecordKind : uint8_t { New = 0, Changed = 1, Removed = 2 };
        constexpr uint8_t OwnerFollows = 1;
        constexpr uint32_t MaxRecords = 1u << 20;

        void WriteProperty(NetWriter& out, const std::vector<uint8_t>& property)
        {
            out.WriteVarU32(static_cast<uint32_t>(property.size()));
            out.WriteBytes(property);
        }

        bool ReadProperty(NetReader& in, std::vector<uint8_t>& property)
        {
            const uint32_t length = in.ReadVarU32();
            if (in.IsFailed() || length > in.GetRemaining())
                return false;
            const std::span<const uint8_t> bytes = in.ReadBytes(length);
            property.assign(bytes.begin(), bytes.end());
            return !in.IsFailed();
        }

        bool Same(const EntityStatePtr& a, const EntityStatePtr& b)
        {
            return a == b || (a && b && *a == *b);
        }
    }

    // ------------------------------------------------------------------------------------------------------------------ sender

    void SnapshotSender::Resolve(Sent& sent, bool applied)
    {
        sent.Resolved = true;
        for (NetId id : sent.Entities)
        {
            auto found = m_Entities.find(id);
            if (found == m_Entities.end())
                continue;
            Tracked& tracked = found->second;
            auto entry = std::find_if(tracked.Pending.begin(), tracked.Pending.end(), [&](const SentState& s) { return s.Sequence == sent.Sequence; });
            if (entry == tracked.Pending.end())
                continue;
            if (applied)
            {
                // The client holds this state (or a newer one): what was sent before it no longer matters.
                if (entry->Sequence > tracked.Acked.Sequence)
                    tracked.Acked = *entry;
                tracked.Pending.erase(tracked.Pending.begin(), entry + 1);
            }
            else
            {
                // Lost: what it carried is sent again unless a newer one replaced it -- at the priority it had reached, not from 0.
                tracked.Waiting = std::max(tracked.Waiting, entry->Key);
                tracked.Pending.erase(entry);
            }
        }
        sent.Entities.clear();
    }

    void SnapshotSender::Acknowledge(uint32_t newest, uint32_t bits)
    {
        if (newest == 0 || newest >= m_NextSequence)
            return;
        for (Sent& sent : m_Sent)
        {
            if (sent.Resolved || sent.Sequence == 0 || sent.Sequence > newest)
                continue;
            const uint32_t back = newest - sent.Sequence; // 0: the newest itself
            if (back == 0)
                Resolve(sent, true);
            else if (back <= AckWindow)
                Resolve(sent, ((bits >> (back - 1)) & 1u) != 0);
            else
                Resolve(sent, false); // older than the ack's window and never reported: lost
        }
        m_Acknowledged = std::max(m_Acknowledged, newest);
    }

    void SnapshotSender::Write(const WorldState& world, NetWriter& out, const SnapshotLimits& limits, SnapshotWriteStats* stats)
    {
        const uint32_t sequence = m_NextSequence++;
        Sent& slot = m_Sent[sequence % SnapshotHistory];
        if (!slot.Resolved)
            Resolve(slot, false); // its fate never came back within the history: lost
        slot.Sequence = sequence;
        slot.Resolved = false;
        slot.Entities.clear();

        NetWriter records(256);
        uint32_t recordCount = 0;

        // Removals first: a few bytes each, and an entity the client should no longer have must not linger.
        for (auto it = m_Entities.begin(); it != m_Entities.end();)
        {
            Tracked& tracked = it->second;
            if (world.Entities.contains(it->first))
            {
                ++it;
                continue;
            }
            if (tracked.Believed())
            {
                ++recordCount;
                records.WriteVarU64(it->first);
                records.WriteU8(static_cast<uint8_t>(RecordKind::Removed));
                tracked.Pending.push_back({ sequence, nullptr });
                tracked.Waiting = 0.0f;
                slot.Entities.push_back(it->first);
            }
            else if (!tracked.Acked.State && tracked.Pending.empty())
            {
                it = m_Entities.erase(it); // nothing of it on the client, nothing in flight: forgotten
                continue;
            }
            ++it;
        }

        // The changes: entities whose state differs from what the client is believed to hold.
        struct Change
        {
            NetId Id;
            const EntityStatePtr* State;
            Tracked* Track;
            float Key;
        };
        std::vector<Change> changes;
        for (const auto& [id, state] : world.Entities)
        {
            Tracked& tracked = m_Entities[id];
            if (Same(tracked.Believed(), state))
            {
                tracked.Waiting = 0.0f;
                continue;
            }
            float priority = 1.0f;
            if (limits.Priorities)
                if (auto found = limits.Priorities->find(id); found != limits.Priorities->end())
                    priority = found->second;
            changes.push_back({ id, &state, &tracked, tracked.Waiting + priority });
        }
        const bool limited = limits.MaxBytes != SIZE_MAX;
        if (limited)
            std::sort(changes.begin(), changes.end(),
                      [](const Change& a, const Change& b) { return a.Key != b.Key ? a.Key > b.Key : a.Id < b.Id; });

        const size_t budget = limits.MaxBytes > SnapshotHeaderBytes ? limits.MaxBytes - SnapshotHeaderBytes : 0;
        NetWriter record(64);
        size_t written = 0;
        size_t deferred = 0;
        std::vector<const EntityState*> held; // the states the client may hold
        for (const Change& change : changes)
        {
            const EntityState& state = **change.State;
            Tracked& tracked = *change.Track;
            record.Clear();
            record.WriteVarU64(change.Id);

            // New: the client may not hold it (never acknowledged, or a removal in flight), or what it may hold is no longer comparable
            // property by property (another prefab, scripts added or removed). Else Changed: every property any state it may hold
            // differs in.
            held.clear();
            bool full = !tracked.Acked.State;
            if (!full)
                held.push_back(tracked.Acked.State.get());
            for (const SentState& pending : tracked.Pending)
            {
                if (!pending.State)
                    full = true;
                else
                    held.push_back(pending.State.get());
            }
            for (const EntityState* candidate : held)
                full = full || candidate->Prefab != state.Prefab || candidate->Properties.size() != state.Properties.size();
            if (full)
            {
                record.WriteU8(static_cast<uint8_t>(RecordKind::New));
                record.WriteVarU64(state.Prefab);
                record.WriteVarU32(state.Owner);
                record.WriteVarU32(static_cast<uint32_t>(state.Properties.size()));
                for (const std::vector<uint8_t>& property : state.Properties)
                    WriteProperty(record, property);
            }
            else
            {
                record.WriteU8(static_cast<uint8_t>(RecordKind::Changed));
                bool ownerChanged = false;
                for (const EntityState* candidate : held)
                    ownerChanged = ownerChanged || candidate->Owner != state.Owner;
                record.WriteU8(ownerChanged ? OwnerFollows : 0);
                if (ownerChanged)
                    record.WriteVarU32(state.Owner);
                const size_t count = state.Properties.size();
                record.WriteVarU32(static_cast<uint32_t>(count));
                std::vector<uint8_t> mask((count + 7) / 8, 0);
                for (size_t index = 0; index < count; ++index)
                {
                    for (const EntityState* candidate : held)
                    {
                        if (candidate->Properties[index] != state.Properties[index])
                        {
                            mask[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
                            break;
                        }
                    }
                }
                record.WriteBytes(mask);
                for (size_t index = 0; index < count; ++index)
                {
                    if ((mask[index / 8] >> (index % 8)) & 1u)
                        WriteProperty(record, state.Properties[index]);
                }
            }

            const bool fits = !limited || written == 0 || records.GetSize() + record.GetSize() <= budget;
            if (!fits)
            {
                ++deferred; // waits (smaller ones after it may still fit), its priority growing
                tracked.Waiting = change.Key;
                continue;
            }
            records.WriteBytes(record.GetData());
            ++recordCount;
            ++written;
            tracked.Waiting = 0.0f;
            tracked.Pending.push_back({ sequence, *change.State, change.Key });
            slot.Entities.push_back(change.Id);
        }

        out.WriteU8(static_cast<uint8_t>(MessageType::Snapshot));
        out.WriteU32(sequence);
        out.WriteVarU64(world.Tick);
        out.WriteVarU32(recordCount);
        out.WriteBytes(records.GetData());
        if (stats)
        {
            stats->Records = recordCount;
            stats->Deferred = deferred;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------- receiver

    SnapshotReceiver::Result SnapshotReceiver::Read(NetReader& reader, WorldState& world, uint32_t& sequence)
    {
        sequence = reader.ReadU32();
        const uint64_t tick = reader.ReadVarU64();
        const uint32_t recordCount = reader.ReadVarU32();
        if (reader.IsFailed() || sequence == 0 || recordCount > MaxRecords)
            return Result::Malformed;
        if (sequence <= m_LastApplied)
            return Result::Stale;

        // Read into a copy: a malformed snapshot leaves the world as it was.
        WorldState next;
        next.Tick = tick;
        next.Entities = m_World.Entities;
        size_t unknown = 0;
        for (uint32_t record = 0; record < recordCount; ++record)
        {
            const NetId id = reader.ReadVarU64();
            const uint8_t kind = reader.ReadU8();
            if (reader.IsFailed())
                return Result::Malformed;
            switch (static_cast<RecordKind>(kind))
            {
            case RecordKind::New:
            {
                auto state = std::make_shared<EntityState>();
                state->Prefab = reader.ReadVarU64();
                state->Owner = reader.ReadVarU32();
                const uint32_t count = reader.ReadVarU32();
                if (reader.IsFailed() || count > MaxProperties)
                    return Result::Malformed;
                state->Properties.resize(count);
                for (std::vector<uint8_t>& property : state->Properties)
                {
                    if (!ReadProperty(reader, property))
                        return Result::Malformed;
                }
                next.Entities[id] = std::move(state);
                break;
            }
            case RecordKind::Changed:
            {
                const uint8_t flags = reader.ReadU8();
                const ClientId owner = (flags & OwnerFollows) ? reader.ReadVarU32() : InvalidClientId;
                const uint32_t count = reader.ReadVarU32();
                if (reader.IsFailed() || count > MaxProperties)
                    return Result::Malformed;
                const std::span<const uint8_t> mask = reader.ReadBytes((count + 7) / 8);
                if (reader.IsFailed())
                    return Result::Malformed;
                auto found = next.Entities.find(id);
                std::shared_ptr<EntityState> state;
                if (found != next.Entities.end())
                {
                    if (found->second->Properties.size() != count)
                        return Result::Malformed; // (the server writes a change only against states of the same layout)
                    state = std::make_shared<EntityState>(*found->second);
                    if (flags & OwnerFollows)
                        state->Owner = owner;
                }
                std::vector<uint8_t> skipped;
                for (uint32_t index = 0; index < count; ++index)
                {
                    if (((mask[index / 8] >> (index % 8)) & 1u) && !ReadProperty(reader, state ? state->Properties[index] : skipped))
                        return Result::Malformed;
                }
                if (state)
                    found->second = std::move(state);
                else
                    ++unknown; // (a change to something this client does not hold: only after a whole window of lost acks)
                break;
            }
            case RecordKind::Removed:
                next.Entities.erase(id);
                break;
            default:
                return Result::Malformed;
            }
        }
        if (!reader.IsAtEnd())
            return Result::Malformed;

        // Which snapshots were applied, for the ack: the one before becomes bit (gap - 1).
        if (m_LastApplied != 0)
        {
            const uint32_t gap = sequence - m_LastApplied;
            m_AppliedBits = gap >= 64 ? 0 : (m_AppliedBits << gap) | (1ull << (gap - 1));
        }
        m_LastApplied = sequence;
        m_UnknownChanges += unknown;
        m_World = std::move(next);
        world = m_World;
        return Result::Applied;
    }

    void SnapshotReceiver::Reset()
    {
        m_World = {};
        m_LastApplied = 0;
        m_AppliedBits = 0;
        m_UnknownChanges = 0;
    }

    void SnapshotReceiver::WriteAck(NetWriter& out) const
    {
        out.WriteU8(static_cast<uint8_t>(MessageType::SnapshotAck));
        out.WriteU32(m_LastApplied);
        out.WriteU32(static_cast<uint32_t>(m_AppliedBits));
    }

    bool ReadSnapshotAck(NetReader& reader, uint32_t& newest, uint32_t& bits)
    {
        newest = reader.ReadU32();
        bits = reader.ReadU32();
        return !reader.IsFailed();
    }
}
