#pragma once
#include "NetBuffer.h"
#include "NetSnapshot.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Prediction, reconciliation, interpolation and the clock (docs/Networking_Plan_2026-11.md, NET3) -- the parts that know nothing of
// the scene, so they can be tested alone. Unreal's model:
//   - the client's own character (autonomous proxy) moves at once with its inputs; each fixed tick's input is numbered, kept with
//     where it left the character, and sent to the server (the last few each time, for loss);
//   - the server applies each to its character once, when it arrives (InputQueue: Unreal's server moves -- not one per server tick, which
//     the two clocks' drift turned into repeated and dropped inputs) and says in its snapshots which one it applied last and
//     where that left the character; the client compares with what it predicted for that input and, when they differ, takes the
//     server's state and re-applies the inputs that came after it (PredictionHistory);
//   - everyone else's entities (simulated proxies) are drawn a little in the past, between the last snapshots (InterpolationBuffer),
//     at a time the client's estimate of the server's clock gives (NetClock).
namespace Nox::Net
{
    // The client's estimate of the server's tick, from when snapshots arrive: the earliest arrivals are the ones the network delayed
    // least, so the estimate follows them up quickly and down slowly (a late packet does not drag it). It is the server's tick as it
    // reaches this client -- one-way latency included, which is what drawing others in the past needs.
    class NetClock
    {
    public:
        // A snapshot of server tick `tick` arrived at local time `now` (seconds, steady clock).
        void OnSnapshot(uint64_t tick, double now, float tickRate);
        // The server tick arriving now (fractional); 0 before the first snapshot.
        double GetServerTick(double now) const;
        bool IsValid() const { return m_Valid; }
        float GetTickRate() const { return m_TickRate; }
        void Reset() { m_Valid = false; }

    private:
        double m_Offset = 0.0; // server seconds - local seconds
        float m_TickRate = 60.0f;
        bool m_Valid = false;
    };

    // A replicated entity's transform at a server tick.
    struct PoseSample
    {
        double Tick = 0.0;
        glm::dvec3 Position{ 0.0 };
        glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale{ 1.0f };
    };

    // An entity's last poses from snapshots, sampled at a time between them.
    class InterpolationBuffer
    {
    public:
        // (Older than the newest are ignored: snapshots arrive in order or are dropped by the receiver anyway.)
        void Add(const PoseSample& sample);
        // The pose at `tick`: between the two samples around it; before the first, the first; after the last, moved on along the last
        // two samples' velocity for at most `maxExtrapolationTicks`, then held. A jump longer than `teleportDistance` between two
        // samples is not swept (a respawn): the later one is taken. False while empty.
        bool Sample(double tick, double maxExtrapolationTicks, double teleportDistance, PoseSample& pose) const;
        bool IsEmpty() const { return m_Samples.empty(); }
        void Clear() { m_Samples.clear(); }

    private:
        std::deque<PoseSample> m_Samples;
    };

    // What the owning client asked its character to do in one fixed tick (a character controller's inputs).
    struct InputFrame
    {
        uint32_t Sequence = 0;        // counts the client's ticks for this character (1, 2, ...)
        glm::vec3 Move{ 0.0f };       // CharacterController3DComponent::MoveVelocity
        float Jump = 0.0f;            // CharacterController3DComponent::JumpSpeed (0: none)
        glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f }; // the entity's rotation (the client turns its own character)

        bool operator==(const InputFrame& other) const = default;
    };

    constexpr size_t InputRedundancy = 8;  // inputs in each Input message: the newest and the 7 before it (a lost message is covered)
    constexpr size_t MaxQueuedInputs = 16; // the server catches up past this many waiting (a client that sent a burst)
    constexpr size_t MaxInputsPerTick = 4; // applied in one server tick at most (a client sending faster than real time is held back)

    // Input message (type MessageType::Input, client -> server, unreliable): varint entity, u8 count, then the frames oldest first:
    // u32 sequence, 3 floats move, float jump, 4 floats rotation (x y z w).
    void WriteInputs(NetWriter& out, NetId entity, std::span<const InputFrame> frames);
    bool ReadInputs(NetReader& in, NetId& entity, std::vector<InputFrame>& frames);

    // The server's inputs of one client's character, each applied once, in order, when it has arrived.
    class InputQueue
    {
    public:
        // Frames from a message: those not applied or queued yet join, in order.
        void Push(std::span<const InputFrame> frames);
        // The inputs to apply now (in order; at most `max`): the character is stepped once per input.
        std::vector<InputFrame> Take(size_t max);
        // The newest input applied (what the client compares its prediction with); 0 before the first.
        uint32_t GetLastProcessed() const { return m_LastProcessed; }
        size_t GetQueued() const { return m_Queue.size(); }

    private:
        std::deque<InputFrame> m_Queue;
        InputFrame m_Last;
        uint32_t m_LastProcessed = 0;
        uint32_t m_LastReceived = 0;
    };

    // The client's predicted ticks of its own character: each input and where it left the character.
    class PredictionHistory
    {
    public:
        struct Entry
        {
            InputFrame Input;
            glm::dvec3 Position{ 0.0 }; // after the tick
        };
        void Record(const InputFrame& input, const glm::dvec3& positionAfter);
        // The server applied input `sequence` and the character ended at `serverPosition`: forgets what is older, and returns the
        // inputs after it to replay when the prediction for it was off by more than `tolerance` (nullopt: it was right, or that input
        // is no longer known). `error`: how far the prediction was off, the server's position minus the predicted one (when that
        // input is known).
        std::optional<std::vector<InputFrame>> Reconcile(uint32_t sequence, const glm::dvec3& serverPosition, double tolerance,
                                                         glm::dvec3* error = nullptr);
        // The replay's result for an input (the corrected prediction the next comparison is made with).
        void Rewrite(uint32_t sequence, const glm::dvec3& positionAfter);
        // The inputs not yet acknowledged, newest last (what the next Input message repeats).
        std::vector<InputFrame> NewestInputs(size_t count) const;
        size_t GetSize() const { return m_Entries.size(); }
        void Clear() { m_Entries.clear(); }

    private:
        std::deque<Entry> m_Entries;
    };
}
