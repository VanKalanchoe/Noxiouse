#include "NetReplication.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <glm/gtc/quaternion.hpp>

#include "NetClient.h"
#include "NetQuantize.h"
#include "NetServer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Physics/Physics3DScene.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scripting/IScriptBackend.h"
#include "NoxCore/Scripting/ScriptEngine.h"

namespace Nox::Net
{
    NetReplication* NetReplication::s_Current = nullptr;

    namespace
    {
        // The transform's three properties (then the scripts' fields).
        constexpr size_t PositionProperty = 0;
        constexpr size_t RotationProperty = 1;
        constexpr size_t ScaleProperty = 2;
        constexpr size_t FirstScriptProperty = 3;
        constexpr double PendingRpcSeconds = 2.0;

        // Positions to 0.1 mm whatever the project's world unit.
        double PositionQuantum()
        {
            return static_cast<double>(WorldUnits::FromMeters(0.0001f));
        }

        bool Differs(const EntityState* before, const EntityState& state, size_t property)
        {
            return !before || property >= before->Properties.size() || before->Properties[property] != state.Properties[property];
        }
    }

    NetReplication::NetReplication(NetServer& server)
        : m_Mode(Mode::Server), m_Server(&server)
    {
        s_Current = this;
        server.OnClientJoined = [this](const NetClientInfo& client) { OnClientJoined(client); };
        server.OnClientLeft = [this](const NetClientInfo& client, DisconnectReason, const std::string&) { OnClientLeft(client); };
        server.OnMessage = [this](const NetClientInfo& client, uint8_t type, NetReader& reader) { OnServerMessage(client, type, reader); };
    }

    NetReplication::NetReplication(NetClient& client)
        : m_Mode(Mode::Client), m_Client(&client)
    {
        s_Current = this;
        client.OnMessage = [this](uint8_t type, NetReader& reader) { OnClientMessage(type, reader); };
    }

    NetReplication::~NetReplication()
    {
        if (m_Server)
        {
            m_Server->OnClientJoined = nullptr;
            m_Server->OnClientLeft = nullptr;
            m_Server->OnMessage = nullptr;
        }
        if (m_Client)
            m_Client->OnMessage = nullptr;
        if (s_Current == this)
            s_Current = nullptr;
    }

    void NetReplication::SetScene(Scene* scene)
    {
        m_Scene = scene;
        m_LastStates.clear();
        m_Receiver.Reset();
        m_World = {};
        m_ReceivedFirst = false;
        m_ScriptsPending.clear();
        m_PendingRpcs.clear();
    }

    ClientId NetReplication::GetLocalClientId() const
    {
        return m_Client ? m_Client->GetWelcome().Client : InvalidClientId;
    }

    void NetReplication::Update(float deltaTime)
    {
        if (!m_Scene)
            return;
        if (m_Mode == Mode::Server)
        {
            m_SendAccumulator += deltaTime;
            const float interval = 1.0f / m_SnapshotRate;
            if (m_SendAccumulator < interval)
                return;
            // One snapshot however late the frame (a hitch must not make a burst of them).
            m_SendAccumulator = std::fmod(m_SendAccumulator, interval);
            SendSnapshots();
            return;
        }

        // Client: the spawned entities whose scripts started since their state came, and RPCs that were ahead of their entity.
        for (auto it = m_ScriptsPending.begin(); it != m_ScriptsPending.end();)
        {
            auto state = m_World.Entities.find(*it);
            if (state == m_World.Entities.end() || ApplyScriptFields(*it, *state->second, nullptr))
                it = m_ScriptsPending.erase(it);
            else
                ++it;
        }
        const double now = NetNow();
        for (auto it = m_PendingRpcs.begin(); it != m_PendingRpcs.end();)
        {
            if (DeliverRpc(*it))
                it = m_PendingRpcs.erase(it);
            else if (now - it->ReceivedAt > PendingRpcSeconds)
            {
                NOX_CORE_WARN("Net: an RPC for entity {} was dropped: the entity did not arrive within {:.0f} s", it->Entity, PendingRpcSeconds);
                it = m_PendingRpcs.erase(it);
            }
            else
                ++it;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------------- server

    void NetReplication::OnClientJoined(const NetClientInfo& client)
    {
        m_Clients.emplace(client.Id, ClientReplication{});
        if (IScriptBackend* backend = ScriptEngine::GetBackend())
            backend->RaiseNetworkEvent(0, client.Id);
    }

    void NetReplication::OnClientLeft(const NetClientInfo& client)
    {
        m_Clients.erase(client.Id);
        if (IScriptBackend* backend = ScriptEngine::GetBackend())
            backend->RaiseNetworkEvent(1, client.Id);
    }

    void NetReplication::OnServerMessage(const NetClientInfo& client, uint8_t type, NetReader& reader)
    {
        switch (static_cast<MessageType>(type))
        {
        case MessageType::SnapshotAck:
        {
            const uint32_t sequence = reader.ReadU32();
            auto found = m_Clients.find(client.Id);
            if (!reader.IsFailed() && found != m_Clients.end())
                found->second.Sender.Acknowledge(sequence);
            break;
        }
        case MessageType::Rpc:
            ReceiveRpc(client.Id, reader);
            break;
        default:
            break; // (a message of a newer game layer: nothing to do with it)
        }
    }

    void NetReplication::BuildWorld(WorldState& world)
    {
        world.Tick = m_Scene->GetTick();
        IScriptBackend* backend = ScriptEngine::GetBackend();
        std::unordered_map<NetId, EntityStatePtr> states;
        std::vector<std::vector<uint8_t>> fields;
        for (auto handle : m_Scene->GetAllEntitiesWith<NetworkIdentityComponent>())
        {
            Entity entity(handle, m_Scene);
            const NetId id = static_cast<uint64_t>(entity.GetUUID());
            const auto& identity = entity.GetComponent<NetworkIdentityComponent>();
            auto state = std::make_shared<EntityState>();
            state->Owner = identity.Owner;
            if (entity.HasComponent<PrefabInstanceComponent>())
                state->Prefab = static_cast<uint64_t>(entity.GetComponent<PrefabInstanceComponent>().Prefab);
            state->Properties.resize(FirstScriptProperty);
            if (identity.ReplicateTransform && entity.HasComponent<TransformComponent>())
            {
                const auto& transform = entity.GetComponent<TransformComponent>();
                state->Properties[PositionProperty] = EncodePosition(transform.Translation, PositionQuantum());
                state->Properties[RotationProperty] = EncodeRotation(glm::quat(transform.Rotation));
                state->Properties[ScaleProperty] = EncodeScale(transform.Scale);
            }
            if (backend)
            {
                if (const auto* instances = ScriptEngine::GetInstances(entity.GetUUID()))
                {
                    for (ScriptInstanceHandle instance : *instances)
                    {
                        backend->WriteReplicated(instance, fields);
                        for (std::vector<uint8_t>& field : fields)
                            state->Properties.push_back(std::move(field));
                    }
                }
            }
            // Unchanged since the last snapshot: the same state object (the snapshot compares pointers first).
            auto last = m_LastStates.find(id);
            if (last != m_LastStates.end() && *last->second == *state)
                states.emplace(id, last->second);
            else
                states.emplace(id, std::move(state));
        }
        m_LastStates = states;
        world.Entities = std::move(states);
    }

    void NetReplication::SendSnapshots()
    {
        if (m_Clients.empty())
            return;
        WorldState world;
        BuildWorld(world);
        m_LastSnapshotBytes = 0;
        for (auto& [client, replication] : m_Clients)
        {
            NetWriter writer(1024);
            replication.Sender.Write(world, writer);
            m_LastSnapshotBytes += writer.GetSize();
            if (writer.GetSize() > NetTransport::MaxMessageSize)
            {
                if (!m_WarnedOversized)
                    NOX_CORE_ERROR("Net: a snapshot of {} KB is larger than a message can be (512 KB): too many replicated entities "
                                   "for one snapshot (relevancy and budgets come with NET4)", writer.GetSize() / 1024);
                m_WarnedOversized = true;
                continue;
            }
            m_Server->Send(client, writer.GetData(), Delivery::Unreliable);
        }
    }

    uint64_t NetReplication::Spawn(AssetHandle prefab, const glm::dvec3& position, ClientId owner)
    {
        if (m_Mode != Mode::Server || !m_Scene)
            return 0;
        Entity root = m_Scene->Instantiate(prefab, position);
        if (!root)
            return 0;
        if (!root.HasComponent<NetworkIdentityComponent>())
            root.AddComponent<NetworkIdentityComponent>();
        root.GetComponent<NetworkIdentityComponent>().Owner = owner;
        return static_cast<uint64_t>(root.GetUUID());
    }

    bool NetReplication::SetOwner(UUID id, ClientId owner)
    {
        if (m_Mode != Mode::Server || !m_Scene)
            return false;
        Entity entity = m_Scene->GetEntityByUUID(id);
        if (!entity || !entity.HasComponent<NetworkIdentityComponent>())
            return false;
        entity.GetComponent<NetworkIdentityComponent>().Owner = owner;
        return true;
    }

    ClientId NetReplication::GetOwner(UUID id) const
    {
        if (!m_Scene)
            return InvalidClientId;
        Entity entity = m_Scene->GetEntityByUUID(id);
        return entity && entity.HasComponent<NetworkIdentityComponent>() ? entity.GetComponent<NetworkIdentityComponent>().Owner : InvalidClientId;
    }

    // ---------------------------------------------------------------------------------------------------------------------- client

    void NetReplication::OnClientMessage(uint8_t type, NetReader& reader)
    {
        switch (static_cast<MessageType>(type))
        {
        case MessageType::Snapshot:
        {
            WorldState world;
            uint32_t sequence = 0;
            switch (m_Receiver.Read(reader, world, sequence))
            {
            case SnapshotReceiver::Result::Applied:
            {
                ApplyWorld(world);
                NetWriter ack(8);
                WriteSnapshotAck(ack, sequence);
                m_Client->Send(ack.GetData(), Delivery::Unreliable);
                break;
            }
            case SnapshotReceiver::Result::Malformed:
                m_Client->Disconnect(DisconnectReason::ProtocolError, "a malformed snapshot from the server");
                break;
            default:
                break; // late, or its baseline is gone: the next one is sent against an acknowledged baseline
            }
            break;
        }
        case MessageType::Rpc:
            ReceiveRpc(InvalidClientId, reader);
            break;
        default:
            break;
        }
    }

    void NetReplication::ApplyWorld(const WorldState& world)
    {
        if (!m_Scene)
            return;
        if (!m_ReceivedFirst)
        {
            // The level's replicated entities the server removed before this client joined.
            m_ReceivedFirst = true;
            std::vector<Entity> gone;
            for (auto handle : m_Scene->GetAllEntitiesWith<NetworkIdentityComponent>())
            {
                Entity entity(handle, m_Scene);
                if (!world.Entities.contains(static_cast<uint64_t>(entity.GetUUID())))
                    gone.push_back(entity);
            }
            for (Entity entity : gone)
                m_Scene->QueueDestroy(entity);
        }
        for (const auto& [id, state] : world.Entities)
        {
            auto before = m_World.Entities.find(id);
            if (before == m_World.Entities.end())
                ApplyEntity(id, *state, nullptr, false);
            else if (before->second != state)
                ApplyEntity(id, *state, before->second.get(), false);
        }
        for (const auto& [id, _] : m_World.Entities)
        {
            if (world.Entities.contains(id))
                continue;
            m_ScriptsPending.erase(id);
            if (Entity entity = m_Scene->GetEntityByUUID(UUID(id)))
                m_Scene->QueueDestroy(entity);
        }
        m_World = world;
    }

    void NetReplication::ApplyEntity(NetId id, const EntityState& state, const EntityState* before, bool created)
    {
        if (state.Properties.size() < FirstScriptProperty)
            return;
        Entity entity = m_Scene->GetEntityByUUID(UUID(id));
        if (!entity)
        {
            if (state.Prefab == 0)
            {
                if (m_WarnedPrefabs.insert(id).second)
                    NOX_CORE_WARN("Net: the server's entity {} is not in this client's scene and was not spawned from a prefab", id);
                return;
            }
            glm::dvec3 position(0.0);
            if (!state.Properties[PositionProperty].empty())
                DecodePosition(state.Properties[PositionProperty], PositionQuantum(), position);
            entity = m_Scene->Instantiate(AssetHandle(state.Prefab), position, UUID(id));
            if (!entity)
            {
                if (m_WarnedPrefabs.insert(state.Prefab).second)
                    NOX_CORE_ERROR("Net: the server spawned prefab {}, which this client cannot load", state.Prefab);
                return;
            }
            created = true;
            if (!entity.HasComponent<NetworkIdentityComponent>())
            {
                // (Spawned through Network.Spawn from a prefab without one: its body was made as the server's would be -- made
                // again as a proxy that follows the replicated transform.)
                entity.AddComponent<NetworkIdentityComponent>();
                if (IPhysics3DScene* physics = m_Scene->GetPhysics3DScene())
                {
                    if (entity.HasComponent<RigidBody3DComponent>())
                    {
                        physics->DestroyBody(entity);
                        physics->CreateBody(entity);
                    }
                    if (entity.HasComponent<CharacterController3DComponent>())
                        physics->DestroyCharacter(entity);
                }
            }
        }
        entity.GetComponent<NetworkIdentityComponent>().Owner = state.Owner;

        if (entity.HasComponent<TransformComponent>())
        {
            auto& transform = entity.GetComponent<TransformComponent>();
            bool moved = false;
            if (!state.Properties[PositionProperty].empty() && (created || Differs(before, state, PositionProperty)))
                moved |= DecodePosition(state.Properties[PositionProperty], PositionQuantum(), transform.Translation);
            glm::quat rotation;
            if (!state.Properties[RotationProperty].empty() && (created || Differs(before, state, RotationProperty)) &&
                DecodeRotation(state.Properties[RotationProperty], rotation))
            {
                transform.Rotation = glm::eulerAngles(rotation);
                moved = true;
            }
            if (!state.Properties[ScaleProperty].empty() && (created || Differs(before, state, ScaleProperty)))
                moved |= DecodeScale(state.Properties[ScaleProperty], transform.Scale);
            if (moved)
                entity.MarkTransformDirty();
        }

        if (!ApplyScriptFields(id, state, created ? nullptr : before))
            m_ScriptsPending.insert(id);
    }

    bool NetReplication::ApplyScriptFields(NetId id, const EntityState& state, const EntityState* before)
    {
        Entity entity = m_Scene->GetEntityByUUID(UUID(id));
        if (!entity)
            return true;
        IScriptBackend* backend = ScriptEngine::GetBackend();
        const auto* instances = ScriptEngine::GetInstances(UUID(id));
        if (!instances || !backend)
            return !entity.HasComponent<ScriptComponent>() || state.Properties.size() == FirstScriptProperty; // scripts not started yet

        size_t expected = FirstScriptProperty;
        for (ScriptInstanceHandle instance : *instances)
            expected += backend->GetReplicatedFieldCount(instance);
        if (expected != state.Properties.size())
        {
            if (m_WarnedPrefabs.insert(id ^ 0x5c71u).second)
                NOX_CORE_WARN("Net: entity {} has {} replicated properties on the server and {} here (other scripts?): its fields are not applied",
                              id, state.Properties.size(), expected);
            return true;
        }

        size_t property = FirstScriptProperty;
        NetWriter records(64);
        for (ScriptInstanceHandle instance : *instances)
        {
            const uint32_t count = backend->GetReplicatedFieldCount(instance);
            records.Clear();
            for (uint32_t field = 0; field < count; ++field, ++property)
            {
                if (!Differs(before, state, property))
                    continue;
                const std::vector<uint8_t>& value = state.Properties[property];
                records.WriteU16(static_cast<uint16_t>(field));
                records.WriteU16(static_cast<uint16_t>(std::min<size_t>(value.size(), UINT16_MAX)));
                records.WriteBytes(std::span(value.data(), std::min<size_t>(value.size(), UINT16_MAX)));
            }
            if (records.GetSize() > 0)
                backend->ReadReplicated(instance, records.GetData());
        }
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------------- RPCs

    void NetReplication::SendRpc(UUID id, std::string_view className, RpcKind kind, uint32_t method, bool reliable,
                                 std::span<const uint8_t> arguments)
    {
        IScriptBackend* backend = ScriptEngine::GetBackend();
        const auto* instances = ScriptEngine::GetInstances(id);
        if (!backend || !instances)
            return;
        size_t script = instances->size();
        for (size_t index = 0; index < instances->size(); ++index)
        {
            if (backend->GetInstanceClass((*instances)[index]) == className)
            {
                script = index;
                break;
            }
        }
        if (script >= instances->size() || script > UINT8_MAX)
            return;

        NetWriter writer(32 + arguments.size());
        writer.WriteU8(static_cast<uint8_t>(MessageType::Rpc));
        writer.WriteVarU64(static_cast<uint64_t>(id));
        writer.WriteU8(static_cast<uint8_t>(script));
        writer.WriteU8(static_cast<uint8_t>(kind));
        writer.WriteVarU32(method);
        writer.WriteVarU32(static_cast<uint32_t>(arguments.size()));
        writer.WriteBytes(arguments);
        const Delivery delivery = reliable ? Delivery::Reliable : Delivery::Unreliable;

        if (m_Mode == Mode::Client)
        {
            if (kind == RpcKind::Server)
                m_Client->Send(writer.GetData(), delivery);
            return;
        }
        if (kind == RpcKind::Client)
        {
            const ClientId owner = GetOwner(id);
            if (owner != InvalidClientId)
                m_Server->Send(owner, writer.GetData(), delivery);
        }
        else if (kind == RpcKind::Multicast)
            m_Server->Broadcast(writer.GetData(), delivery);
    }

    void NetReplication::ReceiveRpc(ClientId sender, NetReader& reader)
    {
        PendingRpc rpc;
        rpc.Entity = reader.ReadVarU64();
        rpc.Script = reader.ReadU8();
        rpc.Kind = reader.ReadU8();
        rpc.Method = reader.ReadVarU32();
        const uint32_t length = reader.ReadVarU32();
        const std::span<const uint8_t> arguments = reader.ReadBytes(length);
        if (reader.IsFailed() || !reader.IsAtEnd() || rpc.Kind > static_cast<uint8_t>(RpcKind::Multicast))
        {
            if (m_Mode == Mode::Server)
                m_Server->Kick(sender, "a malformed RPC");
            else
                m_Client->Disconnect(DisconnectReason::ProtocolError, "a malformed RPC from the server");
            return;
        }
        rpc.Arguments.assign(arguments.begin(), arguments.end());
        rpc.ReceivedAt = NetNow();

        if (m_Mode == Mode::Server)
        {
            // Never trust the client: only [ServerRpc]s, only on entities that client owns.
            if (rpc.Kind != static_cast<uint8_t>(RpcKind::Server) || GetOwner(UUID(rpc.Entity)) != sender || sender == InvalidClientId)
            {
                NOX_CORE_WARN("Net: client {} sent an RPC for entity {} it does not own (ignored)", sender, rpc.Entity);
                return;
            }
            DeliverRpc(rpc);
            return;
        }
        if (rpc.Kind == static_cast<uint8_t>(RpcKind::Server))
            return; // (a server RPC never comes from the server)
        if (!DeliverRpc(rpc))
            m_PendingRpcs.push_back(std::move(rpc));
    }

    bool NetReplication::DeliverRpc(const PendingRpc& rpc)
    {
        IScriptBackend* backend = ScriptEngine::GetBackend();
        if (!m_Scene || !backend || !m_Scene->GetEntityByUUID(UUID(rpc.Entity)))
            return false;
        const auto* instances = ScriptEngine::GetInstances(UUID(rpc.Entity));
        if (!instances || rpc.Script >= instances->size())
            return false; // (its scripts have not started yet)
        backend->InvokeRpc((*instances)[rpc.Script], rpc.Kind, rpc.Method, rpc.Arguments);
        return true;
    }
}
