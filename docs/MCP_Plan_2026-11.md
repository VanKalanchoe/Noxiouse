# MCP for NoxEditor -- plan (2026-11-13, revised the same day after a review)

Status (2026-11-14): **C1 console and MCP0-MCP3 built and tested by the user; MCP4 built** (part 1 tested by the user, part 2
waiting for the user's test); MCP5 open. Backlog items C1 and N7.

MCP4 part 2 (2026-11-14): `McpWorkflowTools` -- list_graph_node_types, get_graph / set_graph (`McpGraph`: a NodeGraph as JSON with
links by pin name, property values typed by the node type's own declarations -- default properties, Float / Int / Bool pins,
variable-group literals -- states with nested graphs, transitions with named comparisons; every reference checked, then
GraphCompiler::Compile, whose logged reasons are returned when it refuses; applied through NodeGraphEditorPanel::ApplyExternalEdit:
one undo step in the graph's editor window, opened when it is not, saved), run_tests / get_test_results (NoxToolsTest, NoxNetTest
next to the editor, run from their folder with the output in <Project>/.nox/test-logs; checks and FAIL lines parsed; Execute
access), package_game / get_package_result (the scene saved, PackageGame on the main thread as the dialog does; the output must be
outside the project; the result kept for a call that outlasts the minute; Execute access). NoxToolsTest: 143 checks (ASan /
UBSan). The editor side compiles; it has not run in the editor yet.

MCP4 part 1 (2026-11-14): `McpAssetTools` -- build_scripts (asks ScriptBuilder for a build, or waits for the running one, and
answers when it finished: succeeded, reloaded, the errors and warnings; Execute access: MSBuild runs the project's build; a build
longer than the time-out answers building and get_build_status reads it later), get_build_status, get_material / create_material /
set_material (MaterialData as JSON from one field table, `McpMaterial`, checked against the material's own fields; a texture by uuid
or path must be a texture asset and is stored as the Material Editor stores it; written to the .nmat, Renderer::MarkMaterialChanged;
not an undo step, as in the Material Editor -- the answer carries the previous values), create_prefab (SceneSerializer::
SerializePrefab, as dragging into the Content Browser), instantiate_prefab (an edit tool: undo step, apply_edits), import_asset (a
.gltf with its referenced buffers and images, or a .glb, copied into a folder of its own and cooked with ImportModel; textures,
sounds and fonts copied and registered; files already in the asset folder registered in place; answers the new registry entries).
`ScriptDiagnostics` parses dotnet build output into file / line / column / code / message, each once (MSBuild repeats them in its
summary); ScriptBuilder keeps the last build's result and logs one line per error. NoxToolsTest: 108 checks then (ASan / UBSan). The
editor side compiles; it has not run in the editor yet.

Fixed after the user's MCP4 test (2026-11-14): patch_component could not set MaterialComponent's MaterialAssets ("unknown field") --
a component's fields were learnt by serializing a default component, and the serializer leaves fields out while they are empty or
at their default (MaterialAssets / Parameters, MorphWeights, an audio source's graph parameters, particle curves and burst sound, a
body's physical surface, a capsule's bottom radius, a joint axis's drive, a text's outline and background). Each editable component
now has a sample that sets those on a scratch entity, so the serializer itself shows every field and the shape of list items
(`MergeExample` keeps an item's optional fields when the current list replaces the sample's); map fields take any name ("*"),
fields whose kind varies take any value (null). Also: a one-number list is a list, not a vector (a list of material ids), and a
number past 2^53 in a numeric field is refused as an id written as a number. NoxToolsTest: 119 checks.

Fixed after the user's second MCP4 test (2026-11-14): import_asset answered before a model's background cook finished (no assets
listed) -- it now waits for `EditorAssetManager::IsCookingImport` (up to 45 s). The model importer stored texture paths absolute
(also from the Content Browser), so a moved or cloned project lost its textures: they are relative to the asset folder now, as the
Material Editor stores them (every reader -- renderer, ImportMeshTextures, the packager's dependency walk -- takes both). The
editor's own writes of a material (Material Editor on every change, set_material) were each re-imported by the file watcher, which
replaced the asset in memory with a copy read back: `EditorAssetManager::NoteSourceWritten` records the written file's hash, so
the watcher's event is no change.

Fixed after the user's third MCP4 test (2026-11-14): the helmet placed by the assistant was about 2 cm big and lying on its back.
import_asset cooked at scale 1 while the import dialog defaults to the file being in meters (100 world units per unit in a cm
project): it now takes the dialog's `source_unit` (meters by default) or a custom `import_scale`. A bare MeshComponent shows a
per-mesh asset without the glTF node's transform (the helmet's 90 degree X rotation): the new edit tool place_model places a model
as EditorLayer::PlaceAssets does (ModelInstanceComponent, AtFileLayout). focus_entity waits (5 s at most) for the entity to have
bounds, so a model still loading is framed by its size, not at the 10 cm minimum.

MCP3 (2026-11-14): `McpPlayTools` -- play (standalone, listen server or client with N players, network emulation, debug log; the
call's choices are for that Play only, the user's Play settings stay; multiplayer saves the scene without the dialog, and is refused
for a never-saved scene; answers once the game runs, a client once its server let it in, up to 40 s; Execute access: it runs the
project's scripts), simulate, stop, pause (and step N frames), wait_for (seconds; or a log line with given words after a sequence, a
mode, an entity by name or uuid existing / gone, until all hold or the time-out; met true / false; at most 50 s, under the 60 s call
time-out), inject_input (keys by the C# KeyCode names, mouse buttons, mouse motion and wheel spread over a duration; answers when it
lets go; overlapping calls combine), focus_entity, get_viewport_camera, set_viewport_camera (position + look_at, or focal point /
distance / pitch / yaw). Input injection is `Input`'s automation input: injected keys, buttons, motion and wheel that only game scripts
read (`Input::IsGameKeyDown` & co., used by the C# Input API), not gated by the viewport's focus -- the editor window need not be in
front -- and let go when Play stops or the server stops. `McpInputPlayer` holds the timing (frames, shared keys, motion spread over the
time; tested). Tools that take time answer later through `EditorContext::EveryFrame` polls in `McpEditor::Update`. screenshot
`save`: the full-size PNG also goes to `<Project>/.nox/screenshots/` (path in the answer). Fixed: a float field the scene file wrote as
a whole number (a light's Intensity 5) read as "integer" and refused 12.5 -- every number is "number" now; a field that only takes
whole numbers refuses a fraction when it is written (rolled back). NoxToolsTest: 95 checks (ASan / UBSan). The editor side compiles; it has not run
in the editor yet.

MCP2 (2026-11-14, tested by the user): batch edits, one undo step, rollback of a failing batch, edits refused in Play.

MCP2 (2026-11-14): `McpEditTools` -- create_entity (name, parent, position, a basic shape mesh), delete_entities, rename_entity,
duplicate_entity, set_parent (no cycles, keeps the world place), add_component / remove_component / patch_component (`values` or a
JSON Patch), get_component_schema, apply_edits (a batch, all or nothing: the scene snapshot is restored when an edit fails; "$N"
stands for the uuid edit N answered), select, save_scene, undo, redo. Every write: the component's example (its C++ defaults,
serialized from a scratch scene, under its current values) checks field names and kinds; new ids must name an existing asset or
entity; the values go in through the scene serializer (what loading does), are read back (`adjusted` lists values the engine
changed), and the component is put back as it was when the write fails. Edit mode only. Each change is one undo step named
"MCP: ...": `EditorHistory::FrameInfo::ExternalStep` compares the whole scene text (the per-frame fingerprint only covers ids,
names, parents and transforms). `McpValidation` holds the JSON side (patching, checks, differences, YAML <-> JSON) -- NoxToolsTest
83 checks. The editor-side edits compile; they have not run in the editor yet.

Built: `NoxCore/Console` (registry, variables, access levels read / write / execute / user), the Output Log's Cmd line, the server
terminal and `--exec` on it; `NoxEditor/src/Mcp/` -- `McpProtocol` (JSON-RPC, 2025-06-18 + 2025-03-26, token / Host / Origin /
version checks, schema validation, access filtering), `McpServer` (cpp-httplib on 127.0.0.1, exclusive port, calls run on the main
thread between frames, 60 s time-out, Stop answers waiting calls), `McpEditorTools` (get_editor_status, read_log, get_hierarchy,
find_entities, get_entity, get_selection, list_assets, screenshot, list_commands, run_command), `McpEditor` (Tools > MCP Server, the
mcp.* variables -- user-only --, `.nox/mcp.ini`, `.nox/mcp-token` with its own `.gitignore`, copy-connect-command). Removable: CMake
`NOX_WITH_MCP=OFF` drops the folder; EditorLayer has four guarded spots. Tested: NoxToolsTest 59 checks (console 22, protocol 28, HTTP
end to end 9) under AddressSanitizer / UBSan; the editor tools compile, not yet run in the editor.

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
