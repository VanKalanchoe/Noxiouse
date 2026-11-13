# Networking plan (2026-11-12; NET0 built and tested 2026-11-13: Bistro served at 60 ticks/s headless; NET1 built and tested 2026-11-13; NET2 built and tested 2026-11-13: players and boxes replicate between a server and two clients; NET3 built 2026-11-13, waiting for the user's test)

How Nox gets multiplayer: the architecture, what has to change in the engine, the phases. Written from the code as it is
(Application, Scene, Jolt, the C# scripting, world partition). Backlog item N5.

## 1. The choice: state replication with an authoritative server (Unreal's model)

| | Deterministic lockstep (RTS, some fighting games) | State replication, authoritative server (UE, Source, Overwatch, Fortnite) |
|---|---|---|
| What is sent | Only the players' inputs; every machine simulates the same game | The server simulates; it sends the state that changed (positions, health, spawns) |
| Bandwidth | Tiny, independent of how much is in the world (10 000 RTS units) | Grows with what each client must see (interest management keeps it bounded) |
| Requirement | Bit-identical simulation on every machine: same floats, same order, same frame rate | None: clients only approximate between corrections |
| Latency feel | Input delay (wait for everyone's input) or rollback (GGPO: re-simulate on late input) | Own character predicted locally (instant); others interpolated ~100 ms in the past |
| Joining mid-game | Hard (send the whole state, then catch up the simulation) | Natural (send the current state of what is relevant) |
| Cheating | Every client knows everything (map hacks); can't cheat the simulation | Server decides; clients only see what is relevant to them |
| Debugging | Desyncs: one diverging bit ruins the game minutes later | Visible, local corrections |

**Recommendation: state replication.** Lockstep needs every machine to compute bit-identical results, and Nox cannot promise that:
the C# scripts run on the .NET JIT (floating point and math library calls can differ between CPUs and runtimes), entity iteration
order and async asset loading are not deterministic, and frame time feeds several systems. Jolt alone could be deterministic (it is
built with `CROSS_PLATFORM_DETERMINISTIC`), but the whole game would have to be. Rollback lockstep can be a later, separate mode for
small 1v1 games with a fixed, deterministic gameplay layer.

## 2. Topology

- **Dedicated server** (headless, no GPU, typically Linux in a data centre): competitive games, many players, anti-cheat.
- **Listen server**: one player's game is also the server (co-op, small sessions); no headless needed.
- Both are the same code: the server is a role, not a different game. Unreal's net roles: **Authority** (the server's copy), **Autonomous
  proxy** (the client's own pawn: predicted), **Simulated proxy** (everyone else's: interpolated).

## 3. Transport: Valve's GameNetworkingSockets (GNS)

Reliable and unreliable messages over UDP, fragmentation, encryption, connection quality stats, message lanes (priorities). Two ways:

| | Open-source GameNetworkingSockets (BSD-3) | Steamworks SDK ISteamNetworkingSockets |
|---|---|---|
| Needs Steam | No | Clients: the Steam client; servers: SteamGameServer (anonymous login works) |
| Connect by | IP:port (LAN, dedicated with public IP) | Steam ID / Steam Datagram Relay (SDR) |
| NAT traversal / P2P | ICE with your own signalling/STUN/TURN | Built in (relays) |
| IP hiding, DDoS protection | No | Yes (traffic through Valve's relays) |
| Lobbies, matchmaking, invites | Your own | ISteamMatchmaking |

Plan: one `NetTransport` interface with three backends -- **loopback** (in process: editor Play with several clients, tests), **GNS
direct** (IP, LAN, dedicated servers, development), **Steam** (SDR, lobbies, invites; when a game ships on Steam). The game code never
sees which. Headless is not required by GNS: it is required by dedicated servers.

## 4. What the engine needs (features)

1. **Network tick**: one fixed simulation tick (e.g. 60 Hz; 30 for large worlds) driving, in order: received inputs -> scripts'
   OnFixedUpdate -> physics -> snapshot. Today scripts (Scene, 60 Hz accumulator) and Jolt (its own accumulator) step separately: they
   become one tick loop, numbered (tick N), so the server and clients talk about the same moments.
2. **Net identity**: a `NetworkIdentity` component -- the entity's net ID (the existing IDComponent UUID for level entities; a
   server-assigned ID for spawned ones), owner (which connection), role. Spawning and destroying a replicated prefab on the server
   creates and destroys it on clients.
3. **Property replication**: script fields marked `[Replicated]` (and built-in components: transform, character state, animator
   parameters, health...) sent when they change, delta-compressed against what the client acknowledged, quantized (positions to mm,
   rotations to smallest-three quaternions). Conditions as UE: owner only, skip owner, initial only. `OnRep` callbacks on clients.
4. **RPCs**: `[ServerRpc]` (client asks the server: "fire"), `[ClientRpc]` (server tells one client), `[MulticastRpc]` (server tells all
   relevant clients: effects, sounds); reliable or unreliable. The server validates every ServerRpc (never trust the client).
5. **Input and prediction** (the own character): the client sends its input per tick (with the last few, for loss), applies it at once
   locally (Jolt CharacterVirtual on the client), keeps the history; the server simulates the same input and sends back the authoritative
   state for tick N; the client compares, and if it differs rewinds to N and re-applies its inputs since (reconciliation). This is what
   makes movement feel instant at 100 ms ping.
6. **Interpolation** (everyone else): clients buffer snapshots and show others ~2 snapshots in the past, interpolating; extrapolate briefly
   on loss.
7. **Lag compensation**: the server keeps ~1 s of hitbox history; a shot is checked against where targets were on the shooter's screen.
8. **Interest management (relevancy)**: each client gets only what is near or important to it. World partition cells (6c) are the grid:
   the server keeps every client's relevant cells loaded (the union -- UE's world partition server streaming) and replicates by distance
   and priority within a bandwidth budget per client (UE's NetPriority / NetUpdateFrequency / dormancy for things that don't change).
9. **Physics**: simulated on the server; replicated bodies are kinematic proxies on clients (interpolated), except the own predicted
   character. Physics-heavy games (vehicles) predict their own vehicle as the character.
10. **Animation**: replicate animator parameters (speed, jump, aim), not poses; montages / one-shots as multicast RPCs; root motion
    predicted with the character.
11. **Level and session flow**: connect (handshake: engine/project version, content hash), load the server's level, spawn the player,
    level changes for everyone (UE's seamless travel), disconnect/timeout, reconnect.
12. **Clock sync**: clients estimate the server tick (round-trip time / 2) to time inputs and interpolation.
13. **Tools**: editor Play with N clients + server (separate windows / viewports, loopback), network emulation (lag, jitter, loss), a
    net profiler (bytes per entity / property / RPC), the stats report's connection section.
14. **Later**: voice chat (Steam or Opus), lobbies/matchmaking/invites (Steam), server browser, anti-cheat hooks, replays (record
    snapshots).

## 5. Headless mode (dedicated server)

What exists assumes a window and a GPU: `Application` always creates the Window, the Renderer and ImGui; the asset loader uploads meshes
and textures through the Renderer (`Renderer::WriteMeshUpload`, `UnloadMesh`, `DeferAssetRelease`); the Scene calls `m_renderer`
(BeginScene, environment, GPU scene); the audio engine opens a device.

1. `ApplicationSpecification::Headless` (`--server`): no Window (SDL without video), no Renderer, no ImGui, no audio device (the audio
   engine off or a null backend), no stats overlay; the main loop runs the net tick and sleeps until the next one (no vsync to pace it).
   Logging to stdout and a file; a small console for commands (kick, change level, status).
2. **Assets without a GPU**: a CPU-only asset path -- meshes kept as CPU data where the server needs them (mesh colliders, hitboxes,
   navmesh), skeletons and animations (server-side animation for hitboxes and root motion), no textures, no materials, no GPU geometry
   streaming. Every `Renderer::` call from Asset/Scene behind "is there a renderer" (Scene already tolerates `SetRenderer(nullptr)`).
3. **Server-only and client-only code**: scripts can ask `Network.IsServer / IsClient`; systems that only make sense with a screen
   (particles, audio, UI, post effects) skip on a headless server.
4. **NoxServer target** (later): a build that does not link Vulkan/D3D12, DLSS, Streamline, Steam Audio -- smaller and portable to
   Linux, where dedicated servers usually run (UE's Server target). First the runtime flag, then the target.
5. Packaging: "Package Server" next to "Package Game" (the same cooked content, minus what only rendering needs).

## 6. Phases

| Phase | What | Test |
|---|---|---|
| NET0 | Headless mode (`NoxRuntime --server project.nproj`): no window/GPU/audio, CPU-only assets, tick loop. **Built 2026-11-13**: `ApplicationSpecification::Headless` / `TickRate` (`--server`, `--tickrate=N`), no Window/Renderer/ImGui/stats overlay, audio engine without a device, the loop sleeping to the tick rate; meshes load without GPU geometry (`Renderer::UploadMesh` without a renderer: empty handles; the async mesh load finishes CPU-only), textures / fonts / sounds / acoustics not loaded; the scene spawns model nodes and prefabs but registers nothing with a GPU and submits nothing; one fixed tick for scripts and Jolt (`Scene::AdvanceTicks`, `TickRate` 60, `GetTick()`; `Physics3DScene::StepTicks`), Box2D still per frame (it has no draw interpolation yet: NET2); a status line every 10 s (the work per update, average against the tick's budget and worst, measured before the sleep) and a console (status, quit); a distribution build attaches / opens a console | Bistro server runs at 60 ticks/s, <1 core, no GPU |
| NET1 | Transport (loopback + GNS direct), sessions (handshake, version, disconnect, timeout), network emulation. **Built 2026-11-13** (`NoxCore/src/NoxCore/Network`): `NetTransport` with `LoopbackTransport` (in process; its own emulation: lag, exponential jitter, loss, reliable resends in order, keep-alives, time-outs) and `GnsTransport` (GameNetworkingSockets 1.6 from vcpkg; IP / IPv6 / host names; its fake lag / jitter / loss; a reliable empty probe after 1 s of silence so a dead peer is found within the time-out whatever the game sends -- GNS alone waits 10 s before it checks a peer it only sends unreliable data to); `NetBuffer` (little-endian, varints, every read checked); `NetProtocol` (Hello / Welcome, `DisconnectReason` carried in the transport's close: quit, server shutdown, kicked, server full, version / content mismatch, handshake time-out, protocol error, timed out, connection failed / lost); `NetServer` (several transports, handshake, max clients, kick, handshake time-out, game messages from type 32) and `NetClient` (states, reconnect-safe); `MakeProjectCompatibility` (protocol + project name + checksum of the script assemblies). Runtime: `--server` / `--listen` / `--connect=<address>`, `--port`, `--max-clients`, `--name`, `--server-name`, `--net-emulation=average|bad`, `--net-lag/jitter/loss`; the frame receives before the scene's update and sends after it; a client plays the server's scene and returns to the start scene when the session ends; the window title shows the session; server console `clients`, `kick`, `netemu`. `Tests/NetTest.cpp` (`NoxNetTest`, `-DNOX_BUILD_TESTS=ON`): 75 checks over loopback and real UDP, all passing (ASan/UBSan clean on Linux) | Two runtimes connect over LAN; lag/loss emulated |
| NET2 | One network tick (scripts + Jolt), NetworkIdentity, replicated spawn/destroy, property replication (delta, quantization), RPCs, C# attributes. **Built 2026-11-13**: `NetworkIdentityComponent` (Inspector, saved; `ReplicateTransform`); `NetSnapshot` -- per-client snapshots as deltas against the client's acknowledged baseline (Quake 3 / Iris model: New / Changed with a property bitmask / Removed records, unreliable, converges under loss; tested: every applied snapshot equals the server's world under 30 % loss with reordering); `NetQuantize` -- positions to 0.1 mm as zigzag varints (exact far away), smallest-three rotations in 7 bytes (0.0033 degrees), scale floats; `NetReplication` -- the server reads every replicated entity 30 times a second (`--net-rate`: prefab, owner, position / rotation / scale, the scripts' [Replicated] fields), the client spawns the server's new entities from their prefabs with the server's UUIDs (children follow via NodeUUID), removes and updates them (OnRep), removes the level's replicated entities the server no longer has, and acknowledges; RPCs (ServerRpc only from the owner, ClientRpc to the owner, MulticastRpc to all, reliable or not; an RPC ahead of its entity waits up to 2 s); `Scene::NetMode` -- on a client the replicated entities are proxies: kinematic bodies swept to the replicated transform, no character simulation; every kinematic body now follows its transform (it used to be overwritten by its body); C# (script API 20): `[Replicated(OnRep)]`, `[ServerRpc]` / `[ClientRpc]` / `[MulticastRpc]`, `Network` (Mode, IsServer, IsClient, IsDedicatedServer, HasAuthority, LocalClientId, Spawn, Get/SetOwner, ClientJoined / ClientLeft), `EntityBehaviour.IsOwner / Owner / HasAuthority / CallServerRpc / CallClientRpc / CallMulticastRpc`; runtime `--scene=<path>`; demo `Scenes/NetDemo.nox` (NetGameMode, NetPlayer, NetBox, NetSpinner; prefabs NetBox, NetPlayer). Clients show the latest snapshot (no interpolation, no prediction yet: NET3) | A server spawns boxes and players; clients see them move |
| NET3 | Character prediction + reconciliation, interpolation of others, clock sync. **Built 2026-11-13**: `NetPrediction` -- `NetClock` (the server tick from snapshot arrivals: follows the earliest quickly, late ones slowly), `InterpolationBuffer` (others drawn two snapshots and a tick in the past between their poses; short extrapolation; teleports past 5 m not swept), `InputFrame` / Input message (type 35: move, jump, rotation; the newest 8 inputs each time), `InputQueue` (the server applies a client's inputs one per tick, repeats the last when starved, catches up past 8), `PredictionHistory` (the client's predicted ticks; off by more than 2 cm from the server's state for the acknowledged input: the server's state is taken and the newer inputs replayed); physics: `CharacterNetState` get/set, `StepCharacter` (one character alone, for replays), per-tick hooks; the snapshot's movement property (the character's simulated position, velocities, ground state, the last input applied); a client simulates the characters it owns (`Scene::IsNetCharacterProxy`) and the scripts move them as in single player; the client status line shows the interpolation delay and the corrections. Tested offline: 0 corrections at 100 ms, 3 in 607 snapshots at 83-150 ms with 5 % loss, a server-only push corrected once; the clock within 0.45 ticks under 50-80 ms jitter; interpolation never backwards | Walking feels instant at 150 ms emulated ping; others smooth |
| NET4 | Interest management with world partition cells, priority / bandwidth budget, dormancy | 64 bots in the streaming test world, bandwidth per client bounded |
| NET5 | Lag compensation, animation replication, physics proxies, multicast effects | Hitscan hits what the shooter saw |
| NET6 | Editor Play with N clients, Steam backend (SDR, lobbies, invites, dedicated server login), NoxServer target, Package Server | Friends join through Steam; a Linux server build |
