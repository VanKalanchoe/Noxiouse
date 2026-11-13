#include "NetSnapshot.h"

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

        const WorldState& EmptyWorld()
        {
            static const WorldState empty;
            return empty;
        }
    }

    const WorldState* SnapshotSender::FindBaseline(uint32_t sequence) const
    {
        if (sequence == 0)
            return nullptr;
        const Sent& sent = m_Sent[sequence % SnapshotHistory];
        return sent.Sequence == sequence ? &sent.World : nullptr;
    }

    void SnapshotSender::Write(const WorldState& world, NetWriter& out)
    {
        const WorldState* baseline = FindBaseline(m_Acknowledged);
        const WorldState& base = baseline ? *baseline : EmptyWorld();
        const uint32_t sequence = m_NextSequence++;

        NetWriter records(256);
        uint32_t recordCount = 0;
        for (const auto& [id, state] : world.Entities)
        {
            auto previous = base.Entities.find(id);
            const bool known = previous != base.Entities.end();
            if (known && (previous->second == state || *previous->second == *state))
                continue; // unchanged since the baseline: nothing to say
            ++recordCount;
            records.WriteVarU64(id);
            // New: not in the baseline, or no longer comparable property by property (another prefab, scripts added or removed).
            if (!known || previous->second->Prefab != state->Prefab || previous->second->Properties.size() != state->Properties.size())
            {
                records.WriteU8(static_cast<uint8_t>(RecordKind::New));
                records.WriteVarU64(state->Prefab);
                records.WriteVarU32(state->Owner);
                records.WriteVarU32(static_cast<uint32_t>(state->Properties.size()));
                for (const std::vector<uint8_t>& property : state->Properties)
                    WriteProperty(records, property);
                continue;
            }
            const EntityState& before = *previous->second;
            records.WriteU8(static_cast<uint8_t>(RecordKind::Changed));
            const bool ownerChanged = before.Owner != state->Owner;
            records.WriteU8(ownerChanged ? OwnerFollows : 0);
            if (ownerChanged)
                records.WriteVarU32(state->Owner);
            const size_t count = state->Properties.size();
            records.WriteVarU32(static_cast<uint32_t>(count));
            std::vector<uint8_t> mask((count + 7) / 8, 0);
            for (size_t index = 0; index < count; ++index)
            {
                if (state->Properties[index] != before.Properties[index])
                    mask[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
            }
            records.WriteBytes(mask);
            for (size_t index = 0; index < count; ++index)
            {
                if ((mask[index / 8] >> (index % 8)) & 1u)
                    WriteProperty(records, state->Properties[index]);
            }
        }
        for (const auto& [id, _] : base.Entities)
        {
            if (world.Entities.contains(id))
                continue;
            ++recordCount;
            records.WriteVarU64(id);
            records.WriteU8(static_cast<uint8_t>(RecordKind::Removed));
        }

        out.WriteU8(static_cast<uint8_t>(MessageType::Snapshot));
        out.WriteU32(sequence);
        out.WriteU32(baseline ? m_Acknowledged : 0);
        out.WriteVarU64(world.Tick);
        out.WriteVarU32(recordCount);
        out.WriteBytes(records.GetData());

        Sent& slot = m_Sent[sequence % SnapshotHistory];
        slot.Sequence = sequence;
        slot.World = world;
    }

    void SnapshotSender::Acknowledge(uint32_t sequence)
    {
        if (sequence <= m_Acknowledged || sequence >= m_NextSequence || !FindBaseline(sequence))
            return;
        m_Acknowledged = sequence;
    }

    SnapshotReceiver::Result SnapshotReceiver::Read(NetReader& reader, WorldState& world, uint32_t& sequence)
    {
        sequence = reader.ReadU32();
        const uint32_t baselineSequence = reader.ReadU32();
        const uint64_t tick = reader.ReadVarU64();
        const uint32_t recordCount = reader.ReadVarU32();
        if (reader.IsFailed() || sequence == 0 || recordCount > MaxRecords)
            return Result::Malformed;
        if (sequence <= m_LastApplied)
            return Result::Stale;

        const WorldState* base = &EmptyWorld();
        if (baselineSequence != 0)
        {
            const Received& received = m_Received[baselineSequence % SnapshotHistory];
            if (received.Sequence != baselineSequence)
                return Result::MissingBaseline;
            base = &received.World;
        }

        WorldState next;
        next.Tick = tick;
        next.Entities = base->Entities;
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
                auto found = next.Entities.find(id);
                if (found == next.Entities.end())
                    return Result::Malformed; // a change to something the baseline does not have
                auto state = std::make_shared<EntityState>(*found->second);
                const uint8_t flags = reader.ReadU8();
                if (flags & OwnerFollows)
                    state->Owner = reader.ReadVarU32();
                const uint32_t count = reader.ReadVarU32();
                if (reader.IsFailed() || count != state->Properties.size())
                    return Result::Malformed;
                const std::span<const uint8_t> mask = reader.ReadBytes((count + 7) / 8);
                if (reader.IsFailed())
                    return Result::Malformed;
                for (uint32_t index = 0; index < count; ++index)
                {
                    if (((mask[index / 8] >> (index % 8)) & 1u) && !ReadProperty(reader, state->Properties[index]))
                        return Result::Malformed;
                }
                found->second = std::move(state);
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

        Received& slot = m_Received[sequence % SnapshotHistory];
        slot.Sequence = sequence;
        slot.World = next;
        m_LastApplied = sequence;
        world = std::move(next);
        return Result::Applied;
    }

    void SnapshotReceiver::Reset()
    {
        m_Received = {};
        m_LastApplied = 0;
    }

    void WriteSnapshotAck(NetWriter& out, uint32_t sequence)
    {
        out.WriteU8(static_cast<uint8_t>(MessageType::SnapshotAck));
        out.WriteU32(sequence);
    }
}
