#pragma once
#include "NetBuffer.h"
#include "NetPrediction.h"
#include "NetProtocol.h"
#include "NetSnapshot.h"

#include <glm/glm.hpp>

#include <span>
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
// [MulticastRpc] (the server -> everyone); reliable or not.
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
        // Bytes of the last snapshot sent to each client together, and of the last one received (stats).
        size_t GetLastSnapshotBytes() const { return m_LastSnapshotBytes; }

        // Server: places a prefab, replicated (a NetworkIdentityComponent is added to its root when the prefab has none), owned by
        // `owner` (0: the server). Returns the root's id, 0 when it fails.
        uint64_t Spawn(AssetHandle prefab, const glm::dvec3& position, ClientId owner);
        // Server: hands an entity to a client (or back to the server: 0).
        bool SetOwner(UUID entity, ClientId owner);
        ClientId GetOwner(UUID entity) const;
        // An RPC from a script on `entity` (its class `className`); the script ran it locally already when it is for this side.
        void SendRpc(UUID entity, std::string_view className, RpcKind kind, uint32_t method, bool reliable, std::span<const uint8_t> arguments);

    private:
        struct ClientReplication
        {
            SnapshotSender Sender;
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

        static NetReplication* s_Current;

        Mode m_Mode;
        NetServer* m_Server = nullptr;
        NetClient* m_Client = nullptr;
        Scene* m_Scene = nullptr;
        float m_SnapshotRate = 30.0f;
        float m_SendAccumulator = 0.0f;
        size_t m_LastSnapshotBytes = 0;

        // Server
        std::unordered_map<ClientId, ClientReplication> m_Clients;
        std::unordered_map<NetId, EntityStatePtr> m_LastStates; // reused while an entity is unchanged (snapshots share them)
        bool m_WarnedOversized = false;
        // Client
        SnapshotReceiver m_Receiver;
        WorldState m_World;            // the last applied snapshot's world
        bool m_ReceivedFirst = false;  // the first snapshot also removes the level's replicated entities the server no longer has
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
    };
}
