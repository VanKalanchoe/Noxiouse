# MCP for NoxEditor -- plan (2026-11-13, revised the same day after a review)

Status: **planned, nothing built** except the pieces it builds on: the in-memory log (`LogBuffer`) and the editor's Output Log tab
(2026-11-13). Backlog items: C1 Console (built first, §4) and N7 MCP.

Revision note: a review (2026-11-13) kept the architecture and asked for a shared command layer, schema-aware component edits,
operation ids for long work, an explicit protocol revision, a stricter local-server security model, more read tools and early
screenshots. Accepted, with the changes argued in §12; the shared command layer is the engine console (§4), which the editor
needs on its own anyway.

## 1. What MCP is

The Model Context Protocol is an open protocol (JSON-RPC 2.0) through which an AI client -- Claude Code, Claude Desktop, other
editors' assistants -- calls **tools** ("create an entity", "press Play"), reads **resources** ("the open scene", "the log") and
uses **prompts** that a **server** offers. The server is a program you own: here, a small part of NoxEditor. Transports:

- **Streamable HTTP**: the server listens on a URL (`http://127.0.0.1:8765/mcp`); the client posts JSON-RPC requests. Claude Code:
  `claude mcp add --transport http nox http://127.0.0.1:8765/mcp --header "Authorization: Bearer <token>"`.
- **stdio**: the client starts the server as a child process (later: a tiny `NoxMcpBridge` forwarding stdio to the editor).

MCP is only the interface. The planning -- "look, decide, call a tool, check the result, go on" -- is done by the AI client. A Nox
agent of its own (a chat panel in the editor with its own model calls) would be a separate project on top of the same tools (§12).

## 2. What works today without MCP, and what MCP adds

**Today** (Claude Code in the repository): reading and editing every text file -- C# scripts, scenes (`.nox` YAML), prefabs,
materials, graphs, the engine's C++ -- building, running the offline tests, reading the logs you upload, writing test guides. It
cannot see or touch the **running** editor, so every behaviour check goes through you ("press Play, send the log").

**With MCP** the assistant drives the running NoxEditor: looks at the scene as it is, changes it (each change an undo step), imports
assets, saves, presses Play (also multiplayer), waits for something to happen, reads the Output Log, takes viewport screenshots,
injects input, runs console commands, builds scripts and reads the compiler's errors.

## 3. Architecture

```
MCP client --HTTP JSON-RPC--> McpServer thread --> tool registry + validation --> command queue --> main thread, between frames:
(Claude Code)                 (NoxEditor/src/Mcp)  (schemas, permissions)          (operations)      EditorLayer -> scene / assets / console
          <---------------------------------- result (JSON / text / PNG), or an operation id for long work ----------------------
```

- **McpServer** (`NoxEditor/src/Mcp/`): HTTP on 127.0.0.1 on its own thread (cpp-httplib, MIT, one header), JSON with nlohmann/json
  (already vendored in tinygltf). It parses, authenticates and validates; it never touches the scene.
- **Main thread only**: tool handlers run at the start of `EditorLayer::OnUpdate`, between frames. The HTTP thread waits for short
  results; the main thread never waits for anything (long work becomes an operation, §6).
- **One command layer for everything**: the engine console's registry (§4) holds the editor's and the engine's commands with typed
  arguments. An MCP tool is either a thin adapter over a registered command or a read query; the console input line, the dedicated
  server's terminal, C# scripts, tests and MCP all run the same commands. Nothing scene-changing lives in an HTTP handler.
- **Undo**: EditorHistory snapshots the scene each frame, so every mutation made between frames is one undo step; MCP names it
  ("MCP: create_entity Cube"). Commands therefore need no hand-written Undo() each -- that is the snapshot design's advantage.
- **Public ids**: entities by their UUID (`IDComponent`), assets by their AssetHandle; never `entt::entity` (internal, reused).

## 4. The console (built before MCP: backlog C1)

Unreal's console: the `Cmd` line at the bottom of the Output Log and the `~` console in the game. Not an operating-system shell (Unreal
has none either; Claude Code or your own terminal does that, outside the engine's permissions).

- **Registry in NoxCore** (so NoxRuntime and the dedicated server have it too): commands (`name`, help text, typed arguments,
  handler, where it may run: editor / game / server) and console variables (CVars: `r.ShadowQuality 2`, `net.DebugTrace 1`) with a
  type, default, range, help and change callbacks; `help`, `find <text>`, values printed when typed without an argument; values set
  from the command line (`-ExecCmds="net.DebugTrace 1"`) and an optional `Console.ini` per project.
- **Front ends**: an input line under the Output Log (history with Up / Down, Tab completion of names and values, the command and its
  output in the log); the `~` console in NoxRuntime development builds; the dedicated server's terminal (its commands -- status,
  clients, kick, netemu, lagcomp, netdebug, quit -- move onto the registry: one implementation).
- **C#**: `[ConsoleCommand("demo.spawnbots", Help = "...")]` on a static method and `ConsoleVariable<T>` fields, so game code adds
  commands like Unreal's `Exec` functions.
- **MCP**: `list_commands` and `run_command` expose every registered command at once -- the console is how MCP scales without a tool
  per feature, and each command is also usable by you by hand.

## 5. Component editing: validated, partial, by schema

`SceneSerializer::EntityToNode` / `DeserializeEntityComponents` give every component as YAML, which becomes JSON. Writes are not
deserialized blindly into live components:

- **Partial updates**: a JSON-Patch subset (`replace`, `add`, `remove` on paths such as `/Transform/Translation`) applied to the
  component's current data; omitted fields keep their values.
- **Schema**: `get_component_schema` describes each component's fields (type, default, range, read-only / runtime-only, asset
  type for references). Generated from a small field table per component (the Properties panel's ranges and asset kinds), with
  the serializer's output as the check that every serialized field is described (a test fails when a field has no schema).
- **Validation before writing**: unknown fields and wrong types refused, ranges clamped or refused, asset references checked to
  exist and be of the right type, read-only and runtime-only fields refused; the patched component goes through a scratch entity
  first and only replaces the live one when it deserializes and re-serializes to what was asked.
- **Batches are atomic**: `apply_edits` takes several edits; if one fails, the scene goes back to the snapshot taken before the
  batch (EditorHistory's), and the error names the failed edit and why.

## 6. Operations: short calls answer at once, long work gets an id

- Most tools (reads, single edits) answer in the same request within a frame or two.
- Long work -- Play for N seconds, `wait_for`, building scripts, importing, packaging, screenshots that need a GPU read-back --
  returns an **operation id** at once (`Queued -> Executing -> Completed / Failed`, `Queued -> Cancelled`), with
  `get_operation_status` / `cancel_operation`; results are kept for a while after they finish.
- A client may pass its own id; resending the same id returns the same operation instead of running it twice (idempotent retry).
- Limits: a bounded queue (refusal when full), a time-out per operation, operations of a client that disconnected cancelled.
- Progress notifications over a stream come later, once plain request / response is solid.

## 7. Protocol

- **Revision**: MCP 2025-06-18 (Streamable HTTP, `MCP-Protocol-Version` header, no JSON-RPC batching). `initialize` negotiates:
  the client's requested revision when supported, else the server's; newer revisions are added deliberately, one at a time.
- Lifecycle: `initialize` -> `notifications/initialized` -> requests; `ping`; `notifications/cancelled`; JSON-RPC errors with the
  standard codes plus tool errors as `isError` results (the model reads them and corrects itself).
- Responses as plain `application/json` first; SSE streams and the stdio bridge after that works.

## 8. Security

- Off by default (Preferences > MCP server: on / off, port, permissions); bound to 127.0.0.1 only.
- **Token**: 32 random bytes from the operating system's cryptographic generator, in `<Project>/.nox/mcp-token` (file readable by
  the user only where the OS allows; `.nox/` is in `.gitignore`); never written to a log or an error; a **Rotate token** button.
- **Headers**: `Host` must be `127.0.0.1:<port>` or `localhost:<port>`; an `Origin` header, when present, must be one allowed
  (non-browser clients send none -- accepted); both against DNS rebinding.
- **Permissions**: Read, Write (scene and asset edits), Execute (Play, builds, packaging, console commands that run things); each
  tool and command declares which it needs; the preferences grant them. Deleting assets or files, overwriting scenes and packaging
  ask in the editor unless allowed.
- Every call is written to the Output Log ("MCP: set_component Player /Transform/Translation"), so you see what was done.
- Content from the project -- entity names, logs, asset metadata, scene text -- is data, never instructions, for the agent.

## 9. Tools by phase

| Phase | What | Tools / resources |
|---|---|---|
| MCP0 | Server, `initialize` / version negotiation, token, Host / Origin checks, preferences, status bar indicator; offline protocol tests with a fake registry | `get_editor_status` |
| MCP1 | Read, and see | `get_hierarchy` (paged, filtered), `get_entity` (components as JSON), `find_entities`, `get_entity_bounds`, `get_selection`, `list_assets` / `search_assets` (paged), `get_asset`, `get_asset_dependencies`, `read_log` (since a sequence, by level / text: the LogBuffer), `get_stats`, `get_viewport_camera`, `get_component_schema` / `get_scene_schema`, `get_project_documentation`, **`screenshot`** (moved up: it is read-only and the read-back exists), `list_commands`; resources: the open scene's YAML, Nox.log, the docs |
| MCP2 | Edit (validated, undoable) | `create_entity`, `delete_entities`, `rename`, `duplicate`, `set_parent`, `patch_component`, `add_component` / `remove_component`, `set_script_field`, `instantiate_prefab`, `apply_edits` (atomic batch), `select`, `open_scene`, `save_scene`, `undo` / `redo`, `run_command`, `get_operation_status` / `cancel_operation` |
| MCP3 | Play and verify | `play` / `stop` / `simulate` (standalone, listen server, client with N players, network debug log), `wait_for` (seconds, a log line, an entity condition), `inject_input`, `set_camera` / `focus_entity` |
| MCP4 | Wider workflows | `build_scripts` (errors as file / line / message), `reload_scripts`, `run_tests`, materials, prefabs, graphs as JSON (`get_graph` / `set_graph`), `import_asset`, `package_game` / `package_server` |
| MCP5 | NoxRuntime | the same server in NoxRuntime (`--mcp`), its own permissions and lifecycle: automated multiplayer tests without the editor |

**Acceptance test (end of MCP3)**: Claude Code, connected to a running NoxEditor, makes a scene with a camera, a cube with a new
material and a directional light; reads the hierarchy and the values back; presses Play; takes a screenshot; reads the screenshot
and the log; fixes something wrong it saw or that was reported. Reliable -> the architecture works.

## 10. What has to change in the engine

1. The **console registry** and its front ends (§4) -- first, useful without MCP.
2. **Command queue and operations** in EditorLayer (§6).
3. **Dependencies**: cpp-httplib; nlohmann/json (tinygltf's copy).
4. **Component schemas** (field tables) and the YAML <-> JSON bridge with patching and validation (§5).
5. **Screenshots in memory**: `Renderer::ReadbackTexture` and the project screenshot code exist; the PNG goes into the tool result
   (base64 image) instead of a file, completing the operation a frame or two later.
6. **Input injection** in `Input` (Play only).
7. **EditorHistory step names** for edits from outside the UI, and its snapshot for batch rollback.
8. **ScriptBuilder diagnostics** as data.
9. **LogBuffer** -- done (2026-11-13).

## 11. Does it scale?

- **Features**: yes, if it grows through the console registry and the generic component access: a new engine feature adds a command
  or a component field, not a new MCP tool. The tool list stays around 30-40 entries, which keeps `tools/list` small in the model's
  context (each tool's description costs tokens in every conversation).
- **Scene and project size**: yes, with pagination, filters, stable ids and summaries by default (400 boxes are "400 x NetBox",
  not 400 JSON objects, unless asked).
- **Throughput**: one main-thread queue, a few commands per frame. Fine for one agent and for you working beside it; two agents
  at once serialize (by design: the scene has one writer). `apply_edits` batches avoid a round trip per edit.
- **Several editors**: one server per editor instance, each on its own port, written to `<Project>/.nox/mcp-port` for discovery.
- **Limits that do not scale by design**: vision (one screenshot per call), real-time play, and anything without a command or a
  component field.

## 12. On the review's points

Accepted: shared command layer (as the console), schema-aware partial edits, UUIDs, operation ids with idempotent retry, atomic
batches, explicit protocol revision and lifecycle, Host / Origin / token / permission rules, the extra read tools, screenshots early,
the acceptance test, the agent kept separate from the tools.

Changed:
- **Folders**: `NoxEditor/src/Mcp/` (transport, tools) and the registry in `NoxCore/src/NoxCore/Console/` instead of new top-level
  `NoxAI/` / `NoxAgent/` trees: the commands belong to the engine, the MCP adapter to the editor; an agent project gets its own
  folder if it is ever started.
- **Undo**: no per-command Undo() -- EditorHistory's snapshots already make every between-frames mutation undoable and give the
  rollback for batches.
- **Operation ids only for long work**: reads and single edits answer directly (an id for every call doubles the round trips).

## 13. What it cannot do (or not well)

- **Not real time**: a call takes a tenth of a second to seconds; scripted input sequences yes, reactive play no.
- **Vision is snapshots**, not video: it can tell "missing / black / overlapping", not flicker, ghosting or a one-frame pop.
- **Engine C++ changes** are file edits, a rebuild and an editor restart (the connection drops while the editor restarts).
- **Only what a command, tool or component field exposes**; ImGui-only interactions (gizmo dragging, canvas layout) are out of reach.
- **No binary authoring** (textures, meshes, sounds); it imports files it is given and makes procedural content through scripts.
- **GPU debugging / deep profiling** only through exported data.
- **Where the assistant runs**: a Claude Code session on your machine (or attached to it) reaches `127.0.0.1`; a cloud session does
  not unless connected to your computer.

## 14. Testing

- **Offline, in this container**: the console registry (parsing, CVars, completion), the protocol layer (initialize, negotiation,
  tools/list schemas, errors, token / Host / Origin refusals, operations, idempotent retry, time-outs) and the component patch /
  validation against scenes, like NoxNetTest.
- **On your machine**: each phase ships a test guide naming the program for every step (NoxEditor, NoxRuntime, Claude Code).
