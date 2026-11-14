# Networking (`NoxCore/Network` -- 32 Files)

Server-authoritative multiplayer in Unreal's model: one server owns the game, clients show its world, predict their own character and send their input. Plan and status: `docs/Networking_Plan_2026-11.md` (NET0-NET5 and NET6 part 1 built; Steam sessions N6-S and a separate NoxServer target N6-L open). How to use it: the manual's **Multiplayer: Servers, Clients & Replication** page.

---

## Files

| File (`NoxCore/src/NoxCore/Network/`) | Role |
| :--- | :--- |
| `NetTransport.h/.cpp` | Connections and messages behind one interface; the address picks the backend (`loopback[:port]` in-process, else UDP). Network emulation (lag, jitter, loss) for both. |
| `GnsTransport.h/.cpp` | UDP through Valve's GameNetworkingSockets: reliable / unreliable messages, fragmentation, AES-GCM encryption, keep-alives, time-outs, congestion control, connection quality. |
| `LoopbackTransport.h/.cpp` | A server and its clients in one process (tests), behaving like the network one where the session can tell. |
| `NetBuffer.h` | Message bytes, little-endian, every read checked against the end: what arrives is never trusted. |
| `NetProtocol.h` | The session messages before the game's own: Hello, Welcome, refusals with reasons, disconnect. |
| `NetProject.h/.cpp` | The project's name and the checksum of the script assemblies a client must share with the server (version / content checks). |
| `NetServer.h/.cpp` | Accepts connections, runs the handshake (protocol, project, content, capacity), keeps the clients, kicks, shuts down. |
| `NetClient.h/.cpp` | Connects (IP, host name or loopback), sends Hello, waits for Welcome, retries while a server starts. |
| `NetSession.h/.cpp` | One game session -- dedicated or listen server, or client -- with its replication: what NoxRuntime and the editor's multiplayer Play run. Registers the `net.*` console commands and variables. |
| `NetSnapshot.h/.cpp` | Snapshots with per-entity acknowledged state (Unreal Iris): a change is sent once and again only if its packet was lost; spawn / remove; budgets and priorities. |
| `NetQuantize.h/.cpp` | Transforms on the wire: positions to 0.1 mm as zigzag varints (exact in large worlds), rotations as smallest-three. |
| `NetReplication.h/.cpp` | The replication driver: reads entities with a Network Identity every snapshot, applies them on clients (proxies), RPCs both ways, owners, dormancy, the network debug trace. |
| `NetPrediction.h/.cpp` | Client prediction and reconciliation, the server's input queue, interpolation buffers and the network clock -- scene-free, tested alone. |
| `NetRelevancy.h/.cpp` | Interest management (Unreal's relevancy): always relevant, owner, cull distance on a grid, only-relevant-to-owner, the grace period. |
| `NetPhysicsReplication.h/.cpp` | Dynamic bodies simulated on clients and pulled toward the server's state (Unreal's predictive interpolation). |
| `NetLagCompensation.h/.cpp` | Collider history and the rewound raycast: shots checked against where targets stood on the shooter's screen. |
| `NetAnimation.h/.cpp` | Animator parameters and montages replicated (never poses); the clients animate themselves. |

## A server tick

1. The transport delivers messages; `NetServer` runs handshakes and hands game messages to `NetReplication`.
2. Client inputs go into each client character's input queue (`NetPrediction`); the server steps a client's character once per input as it arrives (Unreal's server moves).
3. The scene updates (scripts, physics, animation); `NetLagCompensation` records the colliders.
4. Every 1 / `--net-rate` s `NetReplication` reads each replicated entity; `NetRelevancy` picks what each client gets; `NetSnapshot` writes the changes the client has not acknowledged, highest priority first, within its byte budget (`--net-client-rate`).

## A client frame

1. Snapshots apply to proxies: interpolated about two snapshots in the past, dynamic bodies pulled toward the server's state (`NetPhysicsReplication`), animator parameters applied (`NetAnimation`).
2. The owned character runs at once from local input; each fixed tick's input is numbered and sent unreliably (repeated in later packets).
3. The server's position for an input is compared with the client's own; more than 2 cm off, the client takes the server's state and replays the newer inputs, gliding the visual error out over about 0.1 s.

## Where it plugs in

- **Scene:** `Scene::SetNetMode` (standalone / server / client) before `OnRuntimeStart`; a client's replicated entities become proxies; the Network Identity component holds the replication settings.
- **C#:** `Nox.Network`, `[Replicated]`, `[ServerRpc]` / `[ClientRpc]` / `[MulticastRpc]` and `EntityBehaviour`'s `HasAuthority`, `IsOwner`, `Call*Rpc` (`NoxScriptCore/Source/Network.cs`); fields and RPCs are found once by reflection, in the same order on every side.
- **NoxRuntime:** `RuntimeLayer` reads `--server` / `--listen` / `--connect` and the other switches, starts the `NetSession` and, headless, reads console lines from the terminal.
- **NoxEditor:** multiplayer Play starts a `NetSession` (listen server, or a client of a dedicated server it launches) and the other players as NoxRuntime processes (`ChildProcess`); File > Package Server writes a server build.
- **Console:** `net.Status`, `net.Emulation`, `net.DebugTrace`, `net.LagCompensation`; the server terminal's `status`, `clients`, `kick`, `quit`.

## Tests

`Tests/NetTest.cpp` (target **NoxNetTest**, `-DNOX_BUILD_TESTS=ON`): handshakes and refusals, emulation, time-outs, shutdown, snapshot deltas and acknowledgements, quantization, prediction and reconciliation, relevancy, lag compensation and the debug trace -- 106 checks; `--offline` skips the socket tests.
