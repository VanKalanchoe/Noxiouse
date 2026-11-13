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
        constexpr size_t MovementProperty = 3; // a character's movement state and the last input applied (empty without a character)
        constexpr size_t FirstScriptProperty = 4;

        // The movement property: the character's simulated position (not the drawn one), velocities, ground state, and the newest
        // client input the server applied to it (0: none; a server-owned character).
        std::vector<uint8_t> EncodeMovement(uint32_t lastInput, const IPhysics3DScene::CharacterNetState& state, double quantum)
        {
            NetWriter writer(48);
            writer.WriteVarU32(lastInput);
            const std::vector<uint8_t> position = EncodePosition(state.Position, quantum);
            writer.WriteU8(static_cast<uint8_t>(position.size()));
            writer.WriteBytes(position);
            writer.WriteFloat(state.Velocity.x);
            writer.WriteFloat(state.Velocity.y);
            writer.WriteFloat(state.Velocity.z);
            writer.WriteFloat(state.Horizontal.x);
            writer.WriteFloat(state.Horizontal.y);
            writer.WriteFloat(state.Horizontal.z);
            writer.WriteU8(static_cast<uint8_t>((state.Grounded ? 1 : 0) | (state.WasOnGround ? 2 : 0)));
            return { writer.GetData().begin(), writer.GetData().end() };
        }

        bool DecodeMovement(std::span<const uint8_t> bytes, double quantum, uint32_t& lastInput, IPhysics3DScene::CharacterNetState& state)
        {
            NetReader reader(bytes);
            lastInput = reader.ReadVarU32();
            const uint8_t positionSize = reader.ReadU8();
            if (reader.IsFailed() || !DecodePosition(reader.ReadBytes(positionSize), quantum, state.Position))
                return false;
            state.Velocity = { reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat() };
            state.Horizontal = { reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat() };
            const uint8_t flags = reader.ReadU8();
            state.Grounded = (flags & 1) != 0;
            state.WasOnGround = (flags & 2) != 0;
            return reader.IsAtEnd() && std::isfinite(state.Velocity.x + state.Velocity.y + state.Velocity.z) &&
                   std::isfinite(state.Horizontal.x + state.Horizontal.y + state.Horizontal.z);
        }
        constexpr double PendingRpcSeconds = 2.0;
        constexpr double RelevantTimeout = 5.0;     // Unreal's RelevantTimeout: kept this long after it was last relevant
        constexpr float RelevancyCellMeters = 50.0f; // the relevancy grid's cells (a query looks at the cells within the cull distance)

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
        if (m_HooksInstalled && m_Scene)
            if (IPhysics3DScene* physics = m_Scene->GetPhysics3DScene())
                physics->SetTickCallbacks({}, {});
        if (s_Current == this)
            s_Current = nullptr;
    }

    void NetReplication::SetScene(Scene* scene)
    {
        m_Scene = scene;
        m_LastStates.clear();
        m_NextRead.clear();
        m_StartupIds.clear();
        m_RelevancyEntities.clear();
        m_StreamedLevels.clear();
        for (auto& [_, replication] : m_Clients)
        {
            replication.Sender = SnapshotSender{};
            replication.Relevancy.Clear();
            replication.Relevant.clear();
        }
        m_Receiver.Reset();
        m_World = {};
        m_Hidden.clear();
        m_ReportedLevels.clear();
        m_ScriptsPending.clear();
        m_PendingRpcs.clear();
        m_InputQueues.clear();
        m_Predicted.clear();
        m_Interpolation.clear();
        m_Clock.Reset();
        m_LastSnapshotTick = 0;
        m_HooksInstalled = false; // (the scene's physics is made when it starts: hooked on the first update)
        if (m_Scene && m_Mode == Mode::Client)
            m_Scene->SetNetLocalClient(GetLocalClientId());
        // Server: the scene's own replicated entities (every client loads the same scene: they are matched by id, never spawned there).
        // Those of streamed levels and world cells are known by their LevelComponent.
        if (m_Scene && m_Mode == Mode::Server)
            for (auto handle : m_Scene->GetAllEntitiesWith<NetworkIdentityComponent>())
                m_StartupIds.insert(static_cast<uint64_t>(Entity(handle, m_Scene).GetUUID()));
    }

    ClientId NetReplication::GetLocalClientId() const
    {
        return m_Client ? m_Client->GetWelcome().Client : InvalidClientId;
    }

    void NetReplication::Update(float deltaTime)
    {
        if (!m_Scene)
            return;
        InstallTickHooks();
        // Stats over the last second.
        if (const double now = NetNow(); now - m_StatsSecondStart >= 1.0)
        {
            const float seconds = static_cast<float>(m_StatsSecondStart > 0.0 ? now - m_StatsSecondStart : 1.0);
            m_StatsSecondStart = now;
            m_ReceivedRate = static_cast<float>(m_ReceivedThisSecond) / seconds;
            m_ReceivedThisSecond = 0;
            for (auto& [_, replication] : m_Clients)
            {
                replication.BytesPerSecond = static_cast<float>(replication.BytesThisSecond) / seconds;
                replication.Deferred = replication.DeferredThisSecond;
                replication.BytesThisSecond = 0;
                replication.DeferredThisSecond = 0;
            }
        }
        if (m_Mode == Mode::Client)
        {
            SendInputs();
            ReportLevelVisibility();
        }
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
            uint32_t newest = 0, bits = 0;
            auto found = m_Clients.find(client.Id);
            if (ReadSnapshotAck(reader, newest, bits) && found != m_Clients.end())
                found->second.Sender.Acknowledge(newest, bits);
            break;
        }
        case MessageType::Rpc:
            ReceiveRpc(client.Id, reader);
            break;
        case MessageType::Input:
            ReceiveInputs(client.Id, reader);
            break;
        case MessageType::LevelVisibility:
            ReceiveLevelVisibility(client.Id, reader);
            break;
        default:
            break; // (a message of a newer game layer: nothing to do with it)
        }
    }

    RelevancyEntity NetReplication::MakeRelevancyEntity(Entity entity) const
    {
        const auto& identity = entity.GetComponent<NetworkIdentityComponent>();
        RelevancyEntity relevancy;
        relevancy.Id = static_cast<uint64_t>(entity.GetUUID());
        if (entity.HasComponent<WorldTransformComponent>())
            relevancy.Position = entity.GetComponent<WorldTransformComponent>().GetPosition();
        else if (entity.HasComponent<TransformComponent>())
            relevancy.Position = entity.GetComponent<TransformComponent>().Translation;
        relevancy.CullDistance = static_cast<double>(WorldUnits::FromMeters(std::max(identity.CullDistance, 0.0f)));
        relevancy.Owner = identity.Owner;
        relevancy.Level = entity.HasComponent<LevelComponent>() ? static_cast<uint64_t>(entity.GetComponent<LevelComponent>().Level) : 0;
        relevancy.Priority = identity.Priority;
        relevancy.AlwaysRelevant = identity.AlwaysRelevant;
        relevancy.OnlyRelevantToOwner = identity.OnlyRelevantToOwner;
        return relevancy;
    }

    void NetReplication::BuildWorld(WorldState& world)
    {
        world.Tick = m_Scene->GetTick();
        IScriptBackend* backend = ScriptEngine::GetBackend();
        std::unordered_map<NetId, EntityStatePtr> states;
        std::unordered_map<NetId, double> nextRead;
        std::vector<std::vector<uint8_t>> fields;
        m_RelevancyEntities.clear();
        const double now = NetNow();
        for (auto handle : m_Scene->GetAllEntitiesWith<NetworkIdentityComponent>())
        {
            Entity entity(handle, m_Scene);
            const NetId id = static_cast<uint64_t>(entity.GetUUID());
            auto& identity = entity.GetComponent<NetworkIdentityComponent>();
            m_RelevancyEntities.push_back(MakeRelevancyEntity(entity));
            // A client spawns it from its prefab only when it is not one of the scene's own (a level entity placed as a prefab
            // instance is matched by id like any other).
            const bool startup = m_StartupIds.contains(id) || entity.HasComponent<LevelComponent>();
            const uint64_t prefab = !startup && entity.HasComponent<PrefabInstanceComponent>()
                                        ? static_cast<uint64_t>(entity.GetComponent<PrefabInstanceComponent>().Prefab) : 0;

            // Read now, or keep the last state: dormant (until flushed), or not due at its update rate.
            const bool flush = identity.DormancyFlush;
            identity.DormancyFlush = false;
            bool read = flush || identity.Dormancy == NetworkIdentityComponent::DormancyMode::Awake;
            if (identity.UpdateRate > 0.0f)
            {
                auto next = m_NextRead.find(id);
                const bool due = next == m_NextRead.end() || now >= next->second;
                nextRead[id] = due ? now + 1.0 / identity.UpdateRate : next->second;
                read = read && due;
            }
            auto last = m_LastStates.find(id);
            if (!read && last != m_LastStates.end())
            {
                EntityStatePtr state = last->second;
                if (state->Owner != identity.Owner || state->Prefab != prefab)
                {
                    // (who owns it is always current, dormant or not)
                    auto updated = std::make_shared<EntityState>(*state);
                    updated->Owner = identity.Owner;
                    updated->Prefab = prefab;
                    state = std::move(updated);
                }
                states.emplace(id, std::move(state));
                continue;
            }

            auto state = std::make_shared<EntityState>();
            state->Owner = identity.Owner;
            state->Prefab = prefab;
            state->Properties.resize(FirstScriptProperty);
            if (identity.ReplicateTransform && entity.HasComponent<TransformComponent>())
            {
                const auto& transform = entity.GetComponent<TransformComponent>();
                state->Properties[PositionProperty] = EncodePosition(transform.Translation, PositionQuantum());
                state->Properties[RotationProperty] = EncodeRotation(glm::quat(transform.Rotation));
                state->Properties[ScaleProperty] = EncodeScale(transform.Scale);
            }
            IPhysics3DScene::CharacterNetState character;
            if (IPhysics3DScene* physics = m_Scene->GetPhysics3DScene();
                physics && entity.HasComponent<CharacterController3DComponent>() && physics->GetCharacterNetState(entity, character))
            {
                auto inputs = m_InputQueues.find(id);
                const uint32_t lastInput = inputs != m_InputQueues.end() ? inputs->second.Queue.GetLastProcessed() : 0;
                state->Properties[MovementProperty] = EncodeMovement(lastInput, character, PositionQuantum());
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
            // Unchanged since the last snapshot: the same state object (the snapshots compare pointers first).
            if (last != m_LastStates.end() && *last->second == *state)
                states.emplace(id, last->second);
            else
                states.emplace(id, std::move(state));
        }
        m_LastStates = states;
        m_NextRead = std::move(nextRead);
        world.Entities = std::move(states);
    }

    void NetReplication::SendSnapshots()
    {
        if (m_Clients.empty())
        {
            m_Scene->SetExtraStreamingSources({});
            return;
        }
        WorldState world;
        BuildWorld(world);

        // Relevancy: the grid of this snapshot's entities, each client's viewers (the entities it owns), its relevant world.
        std::vector<uint64_t> levels;
        m_Scene->GetStreamedLevels(levels, false);
        m_StreamedLevels = std::unordered_set<uint64_t>(levels.begin(), levels.end());
        m_Grid.Build(m_RelevancyEntities, m_StreamedLevels, static_cast<double>(WorldUnits::FromMeters(RelevancyCellMeters)));
        std::unordered_map<ClientId, std::vector<glm::dvec3>> viewers;
        std::vector<glm::dvec3> streamingSources;
        for (const RelevancyEntity& entity : m_RelevancyEntities)
        {
            if (entity.Owner != InvalidClientId && m_Clients.contains(entity.Owner))
            {
                viewers[entity.Owner].push_back(entity.Position);
                streamingSources.push_back(entity.Position);
            }
        }
        // The world streams around every client's players on the server too (Unreal's world partition server streaming).
        m_Scene->SetExtraStreamingSources(std::move(streamingSources));

        const size_t budget = std::clamp(static_cast<size_t>(m_ClientRate / m_SnapshotRate), static_cast<size_t>(256),
                                         static_cast<size_t>(NetTransport::MaxMessageSize));
        const double now = NetNow();
        m_LastSnapshotBytes = 0;
        std::vector<RelevantEntity> relevant;
        std::unordered_map<NetId, float> priorities;
        WorldState clientWorld;
        for (auto& [client, replication] : m_Clients)
        {
            replication.Viewers = viewers[client];
            RelevancyViewer viewer{ client, replication.Viewers, &replication.Levels };
            replication.Relevancy.Update(m_Grid, viewer, now, RelevantTimeout, relevant);
            FilterWorld(world, m_Grid, relevant, clientWorld, priorities);
            replication.Relevant.clear();
            for (const auto& [id, _] : clientWorld.Entities)
                replication.Relevant.insert(id);

            NetWriter writer(1024);
            SnapshotLimits limits;
            limits.MaxBytes = budget;
            limits.Priorities = &priorities;
            SnapshotWriteStats stats;
            replication.Sender.Write(clientWorld, writer, limits, &stats);
            replication.DeferredThisSecond += stats.Deferred;
            if (writer.GetSize() > NetTransport::MaxMessageSize)
            {
                if (!m_WarnedOversized)
                    NOX_CORE_ERROR("Net: a snapshot of {} KB is larger than a message can be (512 KB): one entity's replicated state is too large",
                                   writer.GetSize() / 1024);
                m_WarnedOversized = true;
                continue;
            }
            m_LastSnapshotBytes += writer.GetSize();
            replication.BytesThisSecond += writer.GetSize();
            m_Server->Send(client, writer.GetData(), Delivery::Unreliable);
        }
    }

    bool NetReplication::IsRelevantTo(NetId id, ClientId client, const ClientReplication& replication) const
    {
        if (replication.Relevant.contains(id))
            return true;
        if (m_LastStates.contains(id) || !m_Scene)
            return false; // in the snapshots, and not relevant to this client
        // Not in a snapshot yet (spawned since the last one): the rules now.
        Entity entity = m_Scene->GetEntityByUUID(UUID(id));
        if (!entity || !entity.HasComponent<NetworkIdentityComponent>())
            return false;
        RelevancyViewer viewer{ client, replication.Viewers, &replication.Levels };
        return m_Grid.IsRelevant(MakeRelevancyEntity(entity), viewer);
    }

    void NetReplication::ReceiveLevelVisibility(ClientId sender, NetReader& reader)
    {
        const uint32_t count = reader.ReadVarU32();
        if (reader.IsFailed() || count > 100000)
        {
            m_Server->Kick(sender, "a malformed level visibility");
            return;
        }
        std::unordered_set<uint64_t> levels;
        for (uint32_t index = 0; index < count; ++index)
            levels.insert(reader.ReadVarU64());
        if (reader.IsFailed() || !reader.IsAtEnd())
        {
            m_Server->Kick(sender, "a malformed level visibility");
            return;
        }
        if (auto found = m_Clients.find(sender); found != m_Clients.end())
            found->second.Levels = std::move(levels);
    }

    std::vector<NetReplication::ClientNetStats> NetReplication::GetClientStats() const
    {
        std::vector<ClientNetStats> stats;
        for (const auto& [client, replication] : m_Clients)
            stats.push_back({ client, replication.Relevant.size(), replication.Deferred, replication.BytesPerSecond, replication.Levels.size() });
        std::sort(stats.begin(), stats.end(), [](const ClientNetStats& a, const ClientNetStats& b) { return a.Client < b.Client; });
        return stats;
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
            m_ReceivedThisSecond += reader.GetRemaining() + 1;
            WorldState world;
            uint32_t sequence = 0;
            switch (m_Receiver.Read(reader, world, sequence))
            {
            case SnapshotReceiver::Result::Applied:
            {
                // The clock and the snapshots' spacing (how far in the past others are drawn: two snapshots and a tick).
                if (m_LastSnapshotTick != 0 && world.Tick > m_LastSnapshotTick)
                    m_SnapshotSpacingTicks += (std::clamp(static_cast<double>(world.Tick - m_LastSnapshotTick), 1.0, 30.0) - m_SnapshotSpacingTicks) * 0.1;
                m_LastSnapshotTick = world.Tick;
                m_Clock.OnSnapshot(world.Tick, NetNow(), static_cast<float>(Scene::TickRate));
                ApplyWorld(world);
                NetWriter ack(16);
                m_Receiver.WriteAck(ack);
                m_Client->Send(ack.GetData(), Delivery::Unreliable);
                break;
            }
            case SnapshotReceiver::Result::Malformed:
                m_Client->Disconnect(DisconnectReason::ProtocolError, "a malformed snapshot from the server");
                break;
            default:
                break; // late: a newer one was applied already
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
        for (const auto& [id, state] : world.Entities)
        {
            auto before = m_World.Entities.find(id);
            if (before == m_World.Entities.end())
                ApplyEntity(id, *state, nullptr, false);
            else if (before->second != state)
                ApplyEntity(id, *state, before->second.get(), false);
        }
        // What the server no longer sends: removed there, or no longer relevant to this client. A spawned entity goes (it is spawned
        // again when it comes back); a level entity is switched off below (UpdateHidden).
        for (const auto& [id, before] : m_World.Entities)
        {
            if (world.Entities.contains(id))
                continue;
            m_ScriptsPending.erase(id);
            m_Interpolation.erase(id);
            m_Predicted.erase(id);
            if (before->Prefab != 0)
                if (Entity entity = m_Scene->GetEntityByUUID(UUID(id)))
                    m_Scene->QueueDestroy(entity);
        }
        m_World = world;
        UpdateHidden();

        // Every replicated transform at this snapshot's tick, also unchanged ones (standing still is a pose too), for interpolation;
        // the own characters are predicted instead.
        for (const auto& [id, state] : world.Entities)
        {
            if (m_Predicted.contains(id) || state->Properties.size() < FirstScriptProperty || state->Properties[PositionProperty].empty())
                continue;
            PoseSample sample;
            sample.Tick = static_cast<double>(world.Tick);
            if (!DecodePosition(state->Properties[PositionProperty], PositionQuantum(), sample.Position))
                continue;
            DecodeRotation(state->Properties[RotationProperty], sample.Rotation);
            DecodeScale(state->Properties[ScaleProperty], sample.Scale);
            m_Interpolation[id].Add(sample);
        }
    }

    void NetReplication::UpdateHidden()
    {
        // The scene's replicated entities this client does not get (not relevant, or not yet, or removed on the server before it
        // joined) are switched off -- not drawn, not simulated, their scripts not updated -- and back on when the server sends them
        // (ApplyEntity has set their transform and fields by then). An entity switched off in the level itself is left as it is.
        std::vector<Entity> hide, show;
        for (auto handle : m_Scene->GetAllEntitiesWith<NetworkIdentityComponent>())
        {
            Entity entity(handle, m_Scene);
            const NetId id = static_cast<uint64_t>(entity.GetUUID());
            const bool held = m_World.Entities.contains(id);
            const bool hidden = m_Hidden.contains(id);
            const bool inactive = entity.HasComponent<InactiveComponent>();
            if (held && hidden)
                show.push_back(entity);
            else if (!held && !inactive)
                hide.push_back(entity);
            else if (!held && hidden && !inactive)
                hide.push_back(entity); // (made again since: its level was loaded again)
        }
        for (Entity entity : hide)
        {
            entity.AddComponent<InactiveComponent>();
            m_Hidden.insert(static_cast<uint64_t>(entity.GetUUID()));
        }
        for (Entity entity : show)
        {
            if (entity.HasComponent<InactiveComponent>())
                entity.RemoveComponent<InactiveComponent>();
            m_Hidden.erase(static_cast<uint64_t>(entity.GetUUID()));
        }
    }

    void NetReplication::ReportLevelVisibility()
    {
        if (!m_Scene || !m_Client)
            return;
        std::vector<uint64_t> levels;
        m_Scene->GetStreamedLevels(levels, true);
        std::sort(levels.begin(), levels.end());
        if (levels == m_ReportedLevels)
            return;
        // The entities of levels that came in since were made from the level's file: what was applied to their earlier copies is
        // forgotten, so the server's next snapshot applies everything to them.
        std::unordered_set<uint64_t> added;
        for (uint64_t level : levels)
            if (!std::binary_search(m_ReportedLevels.begin(), m_ReportedLevels.end(), level))
                added.insert(level);
        if (!added.empty())
        {
            for (auto handle : m_Scene->GetAllEntitiesWith<NetworkIdentityComponent, LevelComponent>())
            {
                Entity entity(handle, m_Scene);
                if (!added.contains(static_cast<uint64_t>(entity.GetComponent<LevelComponent>().Level)))
                    continue;
                const NetId id = static_cast<uint64_t>(entity.GetUUID());
                m_World.Entities.erase(id);
                m_Interpolation.erase(id);
                m_ScriptsPending.erase(id);
            }
        }
        NetWriter writer(16 + levels.size() * 9);
        writer.WriteU8(static_cast<uint8_t>(MessageType::LevelVisibility));
        writer.WriteVarU32(static_cast<uint32_t>(levels.size()));
        for (uint64_t level : levels)
            writer.WriteVarU64(level);
        m_Client->Send(writer.GetData(), Delivery::Reliable);
        m_ReportedLevels = std::move(levels);
        UpdateHidden(); // (what came in is off until the server sends it)
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
                // (In a streamed world: its level was unloaded here and the server has not heard yet -- it stops sending it.)
                if (m_ReportedLevels.empty() && m_WarnedPrefabs.insert(id).second)
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

        // The transform is set here the first time; afterwards others are drawn between snapshots (Interpolate) and the own
        // character is predicted (Reconcile).
        const bool first = created || !before;
        if (first && entity.HasComponent<TransformComponent>())
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
        UpdateOwnership(entity, id, state);
        if (m_Predicted.contains(id))
            Reconcile(entity, id, state);

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
        {
            // Every client the entity is relevant to (Unreal: the connections with an open channel for it).
            for (const auto& [client, replication] : m_Clients)
                if (IsRelevantTo(static_cast<uint64_t>(id), client, replication))
                    m_Server->Send(client, writer.GetData(), delivery);
        }
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

    // ---------------------------------------------------------------------------------------------------------------- prediction

    void NetReplication::InstallTickHooks()
    {
        if (m_HooksInstalled || !m_Scene)
            return;
        if (IPhysics3DScene* physics = m_Scene->GetPhysics3DScene())
        {
            physics->SetTickCallbacks([this]() { BeforeTick(); }, [this]() { AfterTick(); });
            m_HooksInstalled = true;
        }
    }

    void NetReplication::BeforeTick()
    {
        if (!m_Scene)
            return;
        if (m_Mode == Mode::Server)
        {
            // Each client-owned character is moved by its client's inputs as they arrive: one step per input, each applied once (Unreal's
            // server moves), whatever a script on the server set. Without inputs for half a second (a stalled client) it is stepped with
            // none, so it does not hang in the air.
            IPhysics3DScene* physics = m_Scene->GetPhysics3DScene();
            const double now = NetNow();
            for (auto it = m_InputQueues.begin(); it != m_InputQueues.end();)
            {
                Entity entity = m_Scene->GetEntityByUUID(UUID(it->first));
                if (!entity || !entity.HasComponent<CharacterController3DComponent>() || GetOwner(UUID(it->first)) != it->second.Client || !physics)
                {
                    if (entity && physics)
                        physics->SetCharacterExternallyStepped(entity, false);
                    it = m_InputQueues.erase(it); // gone, or handed to someone else
                    continue;
                }
                physics->SetCharacterExternallyStepped(entity, true);
                auto& controller = entity.GetComponent<CharacterController3DComponent>();
                const std::vector<InputFrame> inputs = it->second.Queue.Take(MaxInputsPerTick);
                if (!inputs.empty())
                    it->second.LastInputAt = now;
                for (const InputFrame& input : inputs)
                {
                    controller.MoveVelocity = input.Move;
                    controller.JumpSpeed = input.Jump;
                    entity.GetComponent<TransformComponent>().Rotation = glm::eulerAngles(input.Rotation);
                    physics->StepCharacter(entity, physics->GetFixedDeltaTime());
                }
                if (!inputs.empty())
                    entity.MarkTransformDirty();
                else if (now - it->second.LastInputAt > 0.5)
                {
                    controller.MoveVelocity = glm::vec3(0.0f);
                    controller.JumpSpeed = 0.0f;
                    physics->StepCharacter(entity, physics->GetFixedDeltaTime());
                }
                ++it;
            }
            return;
        }
        // Client: this tick's input of each own character, as its scripts left it (recorded with its result after the step).
        for (auto& [id, predicted] : m_Predicted)
        {
            Entity entity = m_Scene->GetEntityByUUID(UUID(id));
            if (!entity || !entity.HasComponent<CharacterController3DComponent>())
                continue;
            const auto& controller = entity.GetComponent<CharacterController3DComponent>();
            predicted.PendingInput.Sequence = ++predicted.Sequence;
            predicted.PendingInput.Move = controller.MoveVelocity;
            predicted.PendingInput.Jump = controller.JumpSpeed;
            predicted.PendingInput.Rotation = glm::quat(entity.GetComponent<TransformComponent>().Rotation);
            predicted.Pending = true;
        }
    }

    void NetReplication::AfterTick()
    {
        if (m_Mode != Mode::Client || !m_Scene)
            return;
        IPhysics3DScene* physics = m_Scene->GetPhysics3DScene();
        for (auto& [id, predicted] : m_Predicted)
        {
            if (!predicted.Pending)
                continue;
            predicted.Pending = false;
            Entity entity = m_Scene->GetEntityByUUID(UUID(id));
            IPhysics3DScene::CharacterNetState state;
            if (entity && physics && physics->GetCharacterNetState(entity, state))
                predicted.History.Record(predicted.PendingInput, state.Position);
        }
    }

    void NetReplication::SendInputs()
    {
        for (auto& [id, predicted] : m_Predicted)
        {
            if (predicted.Sequence == predicted.LastSent || predicted.History.GetSize() == 0)
                continue;
            const std::vector<InputFrame> inputs = predicted.History.NewestInputs(InputRedundancy);
            NetWriter writer(16 + inputs.size() * 36);
            WriteInputs(writer, id, inputs);
            m_Client->Send(writer.GetData(), Delivery::Unreliable);
            predicted.LastSent = predicted.Sequence;
        }
    }

    void NetReplication::ReceiveInputs(ClientId sender, NetReader& reader)
    {
        NetId id = 0;
        std::vector<InputFrame> frames;
        if (!ReadInputs(reader, id, frames))
        {
            m_Server->Kick(sender, "malformed inputs");
            return;
        }
        // Only for a character this client owns (never trust the client).
        Entity entity = m_Scene ? m_Scene->GetEntityByUUID(UUID(id)) : Entity{};
        if (!entity || !entity.HasComponent<CharacterController3DComponent>() || GetOwner(UUID(id)) != sender)
            return;
        ServerInputs& inputs = m_InputQueues[id];
        if (inputs.Client != sender)
            inputs = ServerInputs{ sender, InputQueue{}, NetNow() };
        inputs.Queue.Push(frames);
    }

    void NetReplication::UpdateOwnership(Entity entity, NetId id, const EntityState& state)
    {
        // Client: a character this client owns is simulated here (predicted); one it no longer owns is the server's again.
        const ClientId local = GetLocalClientId();
        const bool mine = local != InvalidClientId && state.Owner == local && entity.HasComponent<CharacterController3DComponent>();
        IPhysics3DScene* physics = m_Scene->GetPhysics3DScene();
        if (mine && !m_Predicted.contains(id))
        {
            m_Predicted.emplace(id, Predicted{});
            m_Interpolation.erase(id);
            if (physics)
            {
                physics->DestroyCharacter(entity);
                physics->CreateCharacter(entity); // (Scene::IsNetCharacterProxy is false for it now)
            }
        }
        else if (!mine && m_Predicted.erase(id) && physics)
            physics->DestroyCharacter(entity);
    }

    void NetReplication::Reconcile(Entity entity, NetId id, const EntityState& state)
    {
        auto found = m_Predicted.find(id);
        IPhysics3DScene* physics = m_Scene->GetPhysics3DScene();
        if (found == m_Predicted.end() || !physics || state.Properties[MovementProperty].empty())
            return;
        uint32_t lastInput = 0;
        IPhysics3DScene::CharacterNetState server;
        if (!DecodeMovement(state.Properties[MovementProperty], PositionQuantum(), lastInput, server))
            return;
        Predicted& predicted = found->second;
        if (lastInput == 0 || lastInput <= predicted.Acknowledged)
            return; // (no input applied yet, or this snapshot says nothing newer)
        predicted.Acknowledged = lastInput;

        // Where the server's character was after that input against where this client predicted it: off by more than 2 cm, the
        // server's state is taken and the inputs since replayed (Unreal's ClientAdjustPosition + replaying the saved moves).
        const double tolerance = static_cast<double>(WorldUnits::FromMeters(0.02f));
        std::optional<std::vector<InputFrame>> replay = predicted.History.Reconcile(lastInput, server.Position, tolerance);
        if (!replay)
            return;
        ++m_Corrections;
        auto& controller = entity.GetComponent<CharacterController3DComponent>();
        const glm::vec3 move = controller.MoveVelocity;
        const float jump = controller.JumpSpeed;
        const glm::dvec3 drawnBefore = physics->GetCharacterDrawnPosition(entity);
        physics->SetCharacterNetState(entity, server);
        const float dt = physics->GetFixedDeltaTime();
        IPhysics3DScene::CharacterNetState replayed;
        for (const InputFrame& input : *replay)
        {
            controller.MoveVelocity = input.Move;
            controller.JumpSpeed = input.Jump;
            physics->StepCharacter(entity, dt);
            if (physics->GetCharacterNetState(entity, replayed))
                predicted.History.Rewrite(input.Sequence, replayed.Position);
        }
        controller.MoveVelocity = move; // (the scripts' current input, for the next tick)
        controller.JumpSpeed = jump;
        // Drawn exactly where it was this frame and glided to the corrected path over about 0.1 s (Unreal's network smoothing): a
        // correction is not seen as a snap.
        physics->AddCharacterVisualOffset(entity, drawnBefore - physics->GetCharacterDrawnPosition(entity));
    }

    void NetReplication::Interpolate()
    {
        InstallTickHooks();
        if (m_Mode != Mode::Client || !m_Scene || !m_Clock.IsValid())
            return;
        const double renderTick = m_Clock.GetServerTick(NetNow()) - InterpolationDelayTicks();
        const double teleport = static_cast<double>(WorldUnits::FromMeters(5.0f));
        for (auto it = m_Interpolation.begin(); it != m_Interpolation.end();)
        {
            Entity entity = m_Scene->GetEntityByUUID(UUID(it->first));
            if (!entity || !entity.HasComponent<TransformComponent>())
            {
                it = m_Interpolation.erase(it);
                continue;
            }
            PoseSample pose;
            if (it->second.Sample(renderTick, 6.0, teleport, pose))
            {
                auto& transform = entity.GetComponent<TransformComponent>();
                transform.Translation = pose.Position;
                transform.Rotation = glm::eulerAngles(pose.Rotation);
                transform.Scale = pose.Scale;
                entity.MarkTransformDirty();
            }
            ++it;
        }
    }

    float NetReplication::GetInterpolationDelayMs() const
    {
        return static_cast<float>(InterpolationDelayTicks() * 1000.0 / Scene::TickRate);
    }
}
