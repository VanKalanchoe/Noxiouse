#pragma once
#include "NetAnimation.h"
#include "NetBuffer.h"
#include "NetLagCompensation.h"
#include "NetPrediction.h"
#include "NetProtocol.h"
#include "NetRelevancy.h"
#include "NetSnapshot.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/UUID.h"

// Replication (docs/Networking_Plan_2026-11.md, NET2; Unreal's replication driver): the scene's entities with a
// NetworkIdentityComponent, from the server to its clients, and RPCs both ways.
//
// Server: every 1 / SnapshotRate s it reads each replicated entity -- the prefab it came from, its owner, its transform (position,
// rotation, scale: three properties) and its scripts' [Replicated] fields (one property each) -- and sends every client a snapshot:
// the delta against that client's acknowledged baseline (NetSnapshot). Client: applies each new snapshot -- spawns the server's new
// entities from their prefabs with the server's ids, removes what the server removed, sets transforms and fields (OnRep) -- and
// acknowledges it. RPCs: [ServerRpc] (an owning client -> the server, checked there), [ClientRpc] (the server -> the owning client),
// [MulticastRpc] (the server -> every client the entity is relevant to); reliable or not.
//
// Interest management (NET4, Unreal's relevancy and level visibility): each client gets only the entities relevant to it (NetRelevancy:
// always relevant, its own, within the cull distance of what it owns; in a streamed world only those of the levels / world cells it has
// loaded too, which it reports), within its bandwidth (bytes a second, by priority), and the server streams the world around every
// client's players. The server reads an entity at its update rate, a dormant one not until it is flushed. On a client a level entity
// that is not relevant is switched off (InactiveComponent) until it is again; a spawned one is removed.
//
// NET5: dynamic rigid bodies are simulated on the clients too and pulled toward the server's state (physics replication); animator
// graphs' parameters and montages are replicated (the owner drives its own); the server keeps a second of the replicated entities'
// hitboxes and checks a client's shot against where they were on that client's screen (lag compensation).
namespace Nox
{
    class Scene;
    class Entity;
}

namespace Nox::Net
{
    class NetServer;
    class NetClient;
    struct NetClientInfo;

    enum class RpcKind : uint8_t { Server = 0, Client = 1, Multicast = 2 };

    // What RaycastLagCompensated hit.
    struct RayCastHitResult
    {
        uint64_t Entity = 0; // its UUID
        glm::dvec3 Point{ 0.0 };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
        float Distance = 0.0f;
    };

    class NetReplication
    {
    public:
        enum class Mode : uint8_t { Server, Client };
        // Installs itself in the session's callbacks (joins / leaves / messages). One at a time per process (Get()).
        explicit NetReplication(NetServer& server);
        explicit NetReplication(NetClient& client);
        ~NetReplication();
        NetReplication(const NetReplication&) = delete;
        NetReplication& operator=(const NetReplication&) = delete;

        // The running one (the scripts' Nox.Network calls go to it); null outside a multiplayer game.
        static NetReplication* Get() { return s_Current; }

        // The scene it replicates (a client's scene changes when it joins and leaves; null: none). A client forgets the snapshots of
        // the scene before.
        void SetScene(Scene* scene);
        // Once a frame after the scene's update: the server sends snapshots when due; the client applies what waited for scripts and
        // sends its character's inputs.
        void Update(float deltaTime);
        // Client, once a frame before the scene's update (after the snapshots of this frame were applied): the other entities drawn
        // between the last snapshots, a little in the past (NET3).
        void Interpolate();
        // Client stats: corrections of the own character's prediction so far, and how far in the past others are drawn.
        size_t GetCorrections() const { return m_Corrections; }
        float GetInterpolationDelayMs() const;

        Mode GetMode() const { return m_Mode; }
        ClientId GetLocalClientId() const;
        void SetSnapshotRate(float rate) { m_SnapshotRate = rate > 0.0f ? rate : 30.0f; }
        float GetSnapshotRate() const { return m_SnapshotRate; }
        // Server: each client's bandwidth for snapshots (bytes a second; Unreal's MaxClientRate, 100 000 by default).
        void SetClientRate(float bytesPerSecond) { m_ClientRate = bytesPerSecond >= 1000.0f ? bytesPerSecond : 100000.0f; }
        float GetClientRate() const { return m_ClientRate; }
        // Bytes of the last snapshot sent to each client together, and of the last one received (stats).
        size_t GetLastSnapshotBytes() const { return m_LastSnapshotBytes; }
        // Server: per client, what relevancy and the budget did (the status line).
        struct ClientNetStats
        {
            ClientId Client = InvalidClientId;
            size_t Relevant = 0;        // entities in its last snapshot's world
            size_t Deferred = 0;        // changes that waited for a later snapshot (the last second)
            float BytesPerSecond = 0.0f; // snapshots (the last second)
            size_t Levels = 0;          // streamed levels it reported loaded
        };
        std::vector<ClientNetStats> GetClientStats() const;
        size_t GetReplicatedCount() const { return m_LastStates.size(); }
        // Client: the replicated entities it holds, and the snapshot bytes a second it received (the last second).
        size_t GetHeldCount() const { return m_World.Entities.size(); }
        float GetReceivedBytesPerSecond() const { return m_ReceivedRate; }

        // Server: places a prefab, replicated (a NetworkIdentityComponent is added to its root when the prefab has none), owned by
        // `owner` (0: the server). Returns the root's id, 0 when it fails.
        uint64_t Spawn(AssetHandle prefab, const glm::dvec3& position, ClientId owner);
        // Server: hands an entity to a client (or back to the server: 0).
        bool SetOwner(UUID entity, ClientId owner);
        ClientId GetOwner(UUID entity) const;
        // An RPC from a script on `entity` (its class `className`); the script ran it locally already when it is for this side.
        // `skipOwner`: a multicast the owning client does not get (it played its effect already: Unreal's predicted cosmetics).
        void SendRpc(UUID entity, std::string_view className, RpcKind kind, uint32_t method, bool reliable, std::span<const uint8_t> arguments,
                     bool skipOwner = false);

        // Lag compensation (NET5). Client: the server tick the others are drawn at now (send it with a shot); 0 before the first snapshot.
        double GetViewTick() const;
        // Server: a ray from client `shooter`'s shot, against the replicated entities' hitboxes as they stood at `viewTick` on that
        // client's screen (held to what its latency allows, at most 1 s back) and the static level as it is (walls hide what is behind
        // them). The shooter's own entities are skipped. `rewoundMs`: how far back it looked.
        bool RaycastLagCompensated(ClientId shooter, double viewTick, const glm::dvec3& origin, const glm::vec3& direction, float maxDistance,
                                   RayCastHitResult& hit, double* rewoundMs = nullptr);
        // Server: off, shots are checked against where the targets are now (the console's `lagcomp off`, to compare).
        void SetLagCompensation(bool enabled) { m_LagCompensation = enabled; }
        bool GetLagCompensation() const { return m_LagCompensation; }

        // The network debug trace (--net-debug; the server console's `netdebug on|off`), in the log: what explains a character's
        // movement and animation on each machine. Server: a client's inputs coming unevenly (ticks that applied 0 or several, a
        // stall stepped without inputs, the burst after it), and a character moving faster than it walks (a speed of its own its
        // inputs do not explain: with its ground). Both: the animation parameters starting and stopping (a float leaving or coming back to about 0, a
        // bool changing) as the server sends them and as a client applies them. Client: each correction of its own character. Both: a
        // frame that took more than 0.1 s (a frozen or dragged window).
        void SetDebugTrace(bool enabled);
        bool GetDebugTrace() const { return m_DebugTrace; }
        void TraceFrame(float deltaTime);

    private:
        struct ClientReplication
        {
            SnapshotSender Sender;
            ClientRelevancy Relevancy;
            std::unordered_set<uint64_t> Levels;   // the streamed levels it has loaded (its LevelVisibility)
            std::vector<glm::dvec3> Viewers;       // where its own entities were at the last snapshot
            std::unordered_set<NetId> Relevant;    // its last snapshot's world (multicast RPCs go to these)
            size_t Deferred = 0, DeferredThisSecond = 0;
            size_t BytesThisSecond = 0;
            float BytesPerSecond = 0.0f;
        };
        struct PendingRpc
        {
            NetId Entity = 0;
            uint8_t Script = 0;
            uint8_t Kind = 0;
            uint32_t Method = 0;
            std::vector<uint8_t> Arguments;
            double ReceivedAt = 0.0;
        };

        // Server
        void OnClientJoined(const NetClientInfo& client);
        void OnClientLeft(const NetClientInfo& client);
        void OnServerMessage(const NetClientInfo& client, uint8_t type, NetReader& reader);
        void BuildWorld(WorldState& world);
        void SendSnapshots();
        RelevancyEntity MakeRelevancyEntity(Entity entity) const;
        bool IsRelevantTo(NetId id, ClientId client, const ClientReplication& replication) const;
        void ReceiveLevelVisibility(ClientId sender, NetReader& reader);
        // Client
        void ReportLevelVisibility();
        void UpdateHidden();
        // NET5
        void RecordHitboxes();
        AnimationNetState BuildAnimationState(Entity entity, NetId id);
        bool ApplyAnimation(Entity entity, NetId id, const std::vector<uint8_t>& property);
        // Client
        void OnClientMessage(uint8_t type, NetReader& reader);
        void ApplyWorld(const WorldState& world);
        void ApplyEntity(NetId id, const EntityState& state, const EntityState* before, bool created);
        bool ApplyScriptFields(NetId id, const EntityState& state, const EntityState* before);
        // Both
        void ReceiveRpc(ClientId sender, NetReader& reader);
        bool DeliverRpc(const PendingRpc& rpc);
        // Prediction (NET3): every fixed tick the server applies its clients' inputs to their characters; a client records the inputs
        // of its own character and where they left it, reconciles with the server's state, and replays.
        void InstallTickHooks();
        void BeforeTick();
        void AfterTick();
        void ReceiveInputs(ClientId sender, NetReader& reader);
        void SendInputs();
        void UpdateOwnership(Entity entity, NetId id, const EntityState& state);
        void Reconcile(Entity entity, NetId id, const EntityState& state);
        double InterpolationDelayTicks() const { return 2.0 * m_SnapshotSpacingTicks + 1.0; }
        // The debug trace (SetDebugTrace)
        std::string DebugName(Entity entity) const;
        void TraceCharacterTick(Entity entity, NetId id, size_t inputs, bool stallStep, double now);
        void TraceAnimationParameter(Entity entity, NetId id, const std::string& name, float value, bool isBool);
        void FlushDebugTrace(double now);

        static NetReplication* s_Current;

        Mode m_Mode;
        NetServer* m_Server = nullptr;
        NetClient* m_Client = nullptr;
        Scene* m_Scene = nullptr;
        float m_SnapshotRate = 30.0f;
        float m_ClientRate = 100000.0f;
        float m_SendAccumulator = 0.0f;
        size_t m_LastSnapshotBytes = 0;
        double m_StatsSecondStart = 0.0;

        // Server
        std::unordered_map<ClientId, ClientReplication> m_Clients;
        std::unordered_map<NetId, EntityStatePtr> m_LastStates; // reused while an entity is unchanged (snapshots share them)
        std::unordered_map<NetId, double> m_NextRead;           // entities with an update rate: when they are read again
        std::unordered_set<NetId> m_StartupIds;                 // the scene's own replicated entities (a client has them already)
        std::vector<RelevancyEntity> m_RelevancyEntities;       // this snapshot's, in the grid
        std::unordered_set<uint64_t> m_StreamedLevels;
        RelevancyGrid m_Grid;
        bool m_WarnedOversized = false;
        LagCompensationHistory m_Hitboxes;
        bool m_LagCompensation = true;
        uint64_t m_LastHitboxTick = 0;
        std::unordered_map<NetId, std::unordered_map<uint32_t, uint64_t>> m_MontageStarts; // server: montage serial -> the tick it started
        // Client
        SnapshotReceiver m_Receiver;
        WorldState m_World;            // the last applied snapshot's world (minus entities whose level was loaded again since)
        std::unordered_set<NetId> m_Hidden;            // level entities switched off here while the server does not send them
        std::vector<uint64_t> m_ReportedLevels;        // the streamed levels last reported to the server (sorted)
        size_t m_ReceivedThisSecond = 0;
        float m_ReceivedRate = 0.0f;
        double m_ExtrapolateSeconds = 0.0;          // the round trip: how far a server body's state is carried forward here
        std::unordered_set<NetId> m_AnimationPending; // animation states that waited for their graph to load
        struct AppliedMontage
        {
            uint32_t Serial = 0;
            bool Stopped = false;
        };
        std::unordered_map<NetId, std::unordered_map<std::string, AppliedMontage>> m_AppliedMontages;
        std::unordered_set<NetId> m_ScriptsPending; // spawned entities whose scripts did not exist yet when their state came
        std::unordered_set<uint64_t> m_WarnedPrefabs;
        std::vector<PendingRpc> m_PendingRpcs; // for entities that have not arrived yet (the snapshot is behind the RPC)

        // Prediction and interpolation (NET3)
        struct ServerInputs
        {
            ClientId Client = InvalidClientId;
            InputQueue Queue;
            double LastInputAt = 0.0;
        };
        std::unordered_map<NetId, ServerInputs> m_InputQueues; // server: the client-owned characters' inputs
        struct Predicted
        {
            PredictionHistory History;
            uint32_t Sequence = 0;     // the last input recorded
            uint32_t Acknowledged = 0; // the newest input the server said it applied
            uint32_t LastSent = 0;
            bool Pending = false;      // an input recorded before this tick's step, its result not yet
            InputFrame PendingInput;
        };
        std::unordered_map<NetId, Predicted> m_Predicted;               // client: its own characters
        std::unordered_map<NetId, InterpolationBuffer> m_Interpolation; // client: the others
        NetClock m_Clock;
        double m_SnapshotSpacingTicks = 2.0; // ticks between the server's snapshots (measured)
        uint64_t m_LastSnapshotTick = 0;
        bool m_HooksInstalled = false;
        size_t m_Corrections = 0;

        // The debug trace
        struct DebugCharacter
        {
            uint32_t TicksByInputs[MaxInputsPerTick + 1] = {}; // the last second's ticks by how many inputs each applied
            uint32_t StallSteps = 0;                           // the last second's steps without an input
            bool Stalled = false;
            double LastInputAt = 0.0;
            uint32_t Lines = 0; // "faster than it walks" lines the last second (at most a few)
        };
        bool m_DebugTrace = false;
        double m_DebugSecondStart = 0.0;
        std::unordered_map<NetId, DebugCharacter> m_DebugCharacters;
        std::unordered_map<NetId, std::unordered_map<std::string, float>> m_DebugAnimation; // the last value logged, per parameter
    };
}
