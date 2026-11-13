#include "NetPrediction.h"

#include <algorithm>
#include <cmath>

namespace Nox::Net
{
    // ------------------------------------------------------------------------------------------------------------------- clock

    void NetClock::OnSnapshot(uint64_t tick, double now, float tickRate)
    {
        m_TickRate = tickRate > 0.0f ? tickRate : 60.0f;
        const double sample = static_cast<double>(tick) / m_TickRate - now;
        if (!m_Valid || std::abs(sample - m_Offset) > 1.0)
        {
            // The first snapshot, or the server's clock jumped (a hitch, a restart): taken as it is.
            m_Offset = sample;
            m_Valid = true;
            return;
        }
        // Earlier than expected: the network delayed this one less -- followed quickly. Later: followed slowly (one late packet must
        // not drag the clock back; a lasting rise in latency still moves it within a few seconds).
        const double rate = sample > m_Offset ? 0.2 : 0.01;
        m_Offset += (sample - m_Offset) * rate;
    }

    double NetClock::GetServerTick(double now) const
    {
        return m_Valid ? (now + m_Offset) * m_TickRate : 0.0;
    }

    // ------------------------------------------------------------------------------------------------------------ interpolation

    void InterpolationBuffer::Add(const PoseSample& sample)
    {
        if (!m_Samples.empty() && sample.Tick <= m_Samples.back().Tick)
            return;
        m_Samples.push_back(sample);
        while (m_Samples.size() > 32)
            m_Samples.pop_front();
    }

    bool InterpolationBuffer::Sample(double tick, double maxExtrapolationTicks, double teleportDistance, PoseSample& pose) const
    {
        if (m_Samples.empty())
            return false;
        if (tick <= m_Samples.front().Tick || m_Samples.size() == 1)
        {
            pose = tick <= m_Samples.front().Tick ? m_Samples.front() : m_Samples.back();
            return true;
        }
        const PoseSample& last = m_Samples.back();
        if (tick >= last.Tick)
        {
            // Past the newest snapshot (it is late, or lost): on along the last movement for a short while, then held there.
            const PoseSample& before = m_Samples[m_Samples.size() - 2];
            const double span = last.Tick - before.Tick;
            const double ahead = std::min(tick - last.Tick, std::max(maxExtrapolationTicks, 0.0));
            pose = last;
            if (span > 0.0 && glm::distance(before.Position, last.Position) <= teleportDistance)
                pose.Position = last.Position + (last.Position - before.Position) * (ahead / span);
            pose.Tick = tick;
            return true;
        }
        // The two around it.
        size_t next = 1;
        while (m_Samples[next].Tick < tick)
            ++next;
        const PoseSample& a = m_Samples[next - 1];
        const PoseSample& b = m_Samples[next];
        if (glm::distance(a.Position, b.Position) > teleportDistance)
        {
            pose = b; // a respawn or teleport: there at once, not swept across the level
            pose.Tick = tick;
            return true;
        }
        const double t = (tick - a.Tick) / (b.Tick - a.Tick);
        pose.Tick = tick;
        pose.Position = glm::mix(a.Position, b.Position, t);
        pose.Rotation = glm::slerp(a.Rotation, b.Rotation, static_cast<float>(t));
        pose.Scale = glm::mix(a.Scale, b.Scale, static_cast<float>(t));
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------------- inputs

    void WriteInputs(NetWriter& out, NetId entity, std::span<const InputFrame> frames)
    {
        out.WriteU8(static_cast<uint8_t>(MessageType::Input));
        out.WriteVarU64(entity);
        const size_t count = std::min(frames.size(), InputRedundancy);
        out.WriteU8(static_cast<uint8_t>(count));
        for (const InputFrame& frame : frames.subspan(frames.size() - count))
        {
            out.WriteU32(frame.Sequence);
            out.WriteFloat(frame.Move.x);
            out.WriteFloat(frame.Move.y);
            out.WriteFloat(frame.Move.z);
            out.WriteFloat(frame.Jump);
            out.WriteFloat(frame.Rotation.x);
            out.WriteFloat(frame.Rotation.y);
            out.WriteFloat(frame.Rotation.z);
            out.WriteFloat(frame.Rotation.w);
        }
    }

    bool ReadInputs(NetReader& in, NetId& entity, std::vector<InputFrame>& frames)
    {
        entity = in.ReadVarU64();
        const uint8_t count = in.ReadU8();
        if (in.IsFailed() || count > InputRedundancy)
            return false;
        frames.resize(count);
        for (InputFrame& frame : frames)
        {
            frame.Sequence = in.ReadU32();
            frame.Move = { in.ReadFloat(), in.ReadFloat(), in.ReadFloat() };
            frame.Jump = in.ReadFloat();
            frame.Rotation.x = in.ReadFloat();
            frame.Rotation.y = in.ReadFloat();
            frame.Rotation.z = in.ReadFloat();
            frame.Rotation.w = in.ReadFloat();
            // Never trust a client: no NaN or infinity reaches the character; the rotation is a unit one.
            const float values[] = { frame.Move.x, frame.Move.y, frame.Move.z, frame.Jump, frame.Rotation.x, frame.Rotation.y,
                                     frame.Rotation.z, frame.Rotation.w };
            for (float value : values)
                if (!std::isfinite(value))
                    return false;
            const float length = glm::length(frame.Rotation);
            frame.Rotation = length > 1e-6f ? frame.Rotation / length : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }
        return in.IsAtEnd();
    }

    void InputQueue::Push(std::span<const InputFrame> frames)
    {
        for (const InputFrame& frame : frames)
        {
            if (frame.Sequence <= m_LastReceived)
                continue; // applied already, or queued (the repeats of earlier messages)
            m_Queue.push_back(frame);
            m_LastReceived = frame.Sequence;
        }
        // Too many waiting (a burst after a stall): the oldest go, so the character does not lag behind its client for good.
        while (m_Queue.size() > MaxQueuedInputs)
        {
            m_LastProcessed = m_Queue.front().Sequence;
            m_Last = m_Queue.front();
            m_Queue.pop_front();
        }
    }

    std::vector<InputFrame> InputQueue::Take(size_t max)
    {
        std::vector<InputFrame> taken;
        while (!m_Queue.empty() && taken.size() < max)
        {
            m_Last = m_Queue.front();
            m_Queue.pop_front();
            m_LastProcessed = m_Last.Sequence;
            taken.push_back(m_Last);
        }
        return taken;
    }

    // --------------------------------------------------------------------------------------------------------------- prediction

    void PredictionHistory::Record(const InputFrame& input, const glm::dvec3& positionAfter)
    {
        m_Entries.push_back({ input, positionAfter });
        while (m_Entries.size() > 256) // (~4 s of ticks without an acknowledgement: the oldest are no use any more)
            m_Entries.pop_front();
    }

    std::optional<std::vector<InputFrame>> PredictionHistory::Reconcile(uint32_t sequence, const glm::dvec3& serverPosition, double tolerance,
                                                                       double* error)
    {
        while (!m_Entries.empty() && m_Entries.front().Input.Sequence < sequence)
            m_Entries.pop_front();
        if (m_Entries.empty() || m_Entries.front().Input.Sequence != sequence)
            return std::nullopt;
        const double off = glm::distance(m_Entries.front().Position, serverPosition);
        if (error)
            *error = off;
        const bool right = off <= tolerance;
        m_Entries.front().Position = serverPosition;
        if (right)
            return std::nullopt;
        std::vector<InputFrame> replay;
        replay.reserve(m_Entries.size() - 1);
        for (size_t index = 1; index < m_Entries.size(); ++index)
            replay.push_back(m_Entries[index].Input);
        return replay;
    }

    void PredictionHistory::Rewrite(uint32_t sequence, const glm::dvec3& positionAfter)
    {
        for (Entry& entry : m_Entries)
        {
            if (entry.Input.Sequence == sequence)
            {
                entry.Position = positionAfter;
                return;
            }
        }
    }

    std::vector<InputFrame> PredictionHistory::NewestInputs(size_t count) const
    {
        const size_t first = m_Entries.size() > count ? m_Entries.size() - count : 0;
        std::vector<InputFrame> inputs;
        inputs.reserve(m_Entries.size() - first);
        for (size_t index = first; index < m_Entries.size(); ++index)
            inputs.push_back(m_Entries[index].Input);
        return inputs;
    }
}
