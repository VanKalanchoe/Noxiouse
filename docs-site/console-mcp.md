# Console, Output Log & MCP Server (`NoxCore/Console`, `NoxMcp`, `NoxEditor/src/Mcp`)

The engine console is one registry of commands and variables in NoxCore that every front end uses: the editor's Output Log Cmd line, the dedicated server's terminal, `--exec`, and the MCP servers. The editor's MCP server lets AI assistants read, edit and play the scene, build its scripts, work with its assets and graphs, run the tests and package the game; NoxRuntime's (`--mcp`, MCP5) lets them look at and drive a running game -- a player window or a dedicated server -- and the editor forwards to the players its multiplayer Play starts (`runtime_call`). Plan and status: `docs/MCP_Plan_2026-11.md` (C1 console and MCP0-MCP5 built). How to use them: the manual's **The Engine Console** and **AI Assistants (MCP Server)** pages.

---

## Console and log files

| File | Role |
| :--- | :--- |
| `NoxCore/src/NoxCore/Console/Console.h/.cpp` | The registry: commands (`RegisterCommand`, RAII `Registration`), typed variables (`TConsoleVariable<bool / int32 / float / string>` with ranges, defaults, `OnChanged`), aliases, `Execute(line, maxAccess)` with quoting and `;`, completion, `help` / `find`, access levels Read / Write / Execute / User. |
| `NoxCore/src/NoxCore/Core/LogBuffer.h/.cpp` | The last 20 000 log lines in memory, filled by a spdlog sink every logger writes to; `CopySince(sequence)` for the Output Log and `read_log`. |
| `NoxEditor/src/Panels/OutputLogPanel.h/.cpp` | The Output Log tab: level filters, search, select / copy, auto-scroll, the Cmd line with history, Tab completion and suggestions. |

## The shared MCP library (`NoxMcp/src/Mcp/`, MCP5)

A static library (`NoxMcp/CMakeLists.txt`) linked by NoxEditor, by NoxRuntime (not in `NOX_DIST` builds) and by NoxToolsTest; built when `NOX_WITH_MCP` is ON.

| File | Role |
| :--- | :--- |
| `McpProtocol.h/.cpp` | JSON-RPC 2.0 over Streamable HTTP (revisions 2025-06-18 and 2025-03-26): token (constant-time compare), Host / Origin checks, initialize, tools/list filtered by the granted access, tools/call with schema validation (no `initialize` needed first); the `Schema` builder; token generation. |
| `McpServer.h/.cpp` | cpp-httplib (`NoxCore/vendors/cpp-httplib`, a git submodule) on 127.0.0.1 with an exclusive port; tool calls queued to the host's main thread (`Pump`), the HTTP thread waiting up to 60 s; `Stop` answers waiting calls. |
| `McpClient.h/.cpp` | `CallServer(port, token, method, params)`: one JSON-RPC request to another Nox MCP server on this machine (blocking, run on a thread of its own); `ToolResultFromJson` forwards a tools/call result unchanged (text, structured content, images, isError); `ServerProbe`: asks a game's server for its tools every half second until it answers (ready) or 60 s pass (not listening: an old NoxRuntime ignores `--mcp`), and logs which. Used by the editor's `runtime_call` and multiplayer Play. |
| `McpProjectFiles.h/.cpp` | The project's MCP files: `<Project>/.nox` with its own `.gitignore`, and the token `.nox/mcp-token` (64 hex digits, owner-only on Linux / macOS) -- one token per project, shared by the editor and the games it starts. |
| `McpHost.h` | `HostContext`: what the shared tools may ask of the program they run in (active scene, mode, view capture, granted access, pause, per-frame polls, whether a local player reads input); the editor's `EditorContext` extends it. `CreateGameTools`, `KeyScancode`, `KeyNames`. |
| `McpGameTools.cpp` | The tools both hosts offer: `read_log`, `get_hierarchy`, `find_entities`, `get_entity`, `screenshot` (only with a view), `list_commands`, `run_command` (its output, plus the log lines the command wrote while it ran: `logged`), `pause`, `wait_for`, `inject_input` (only with a local player). |
| `McpSceneHelpers.h/.cpp` | Ids as strings, entity summaries and paths, paging, find by uuid or name, `RequireScene`, `Now`. |
| `McpValidation.h/.cpp` | Component values as JSON: the example (C++ defaults under current values) as schema, checks, JSON Patch, differences, YAML <-> JSON (ids as strings, 17-digit doubles). |
| `McpInputPlayer.h/.cpp` | `inject_input`'s timing: holds per frame, shared keys, motion spread over the time. |

## The editor's MCP files (`NoxEditor/src/Mcp/`)

| File | Role |
| :--- | :--- |
| `McpEditor.h/.cpp` | The editor side: the `mcp.*` console entries (User access), `.nox/mcp.ini`, Tools > MCP Server, per-frame polls for tools that answer later; registers the shared game tools and the editor's own. |
| `McpEditorTools.h/.cpp` | `EditorContext` (extends `HostContext`; the only way tools reach the editor, EditorLayer fills it) and `get_editor_status` (with the games Play started with MCP servers), `get_selection`, `list_assets`. |
| `McpEditTools.cpp` | Editing tools: entities, components (about 40 editable types), `apply_edits` (atomic via a scene snapshot), select, save, undo, redo, `instantiate_prefab`, `place_model`. A component's fields come from the serializer: its defaults, plus a sample that sets the fields the file only writes when set (MaterialAssets, morph weights, a joint axis's drive, a text outline ...) so they and the shape of list items are known. |
| `McpPlayTools.cpp` | play / simulate / stop, the editor camera tools, and `runtime_call` (MCP5): a tool of a game multiplayer Play started with `--mcp`, called over `McpClient` on a detached thread and answered from a per-frame poll. |
| `McpAssetTools.cpp` | MCP4: `build_scripts` / `get_build_status`, `get_material` / `create_material` / `set_material`, `create_prefab`, `import_asset`. |
| `McpMaterial.h/.cpp` | A material (`MaterialData`, `.nmat`) as JSON and back from one field table; the texture paths it names. |
| `McpWorkflowTools.cpp` | MCP4 part 2: `list_graph_node_types`, `get_graph` / `set_graph` (checked, compiled, applied through the graph editor window's history), `run_tests` / `get_test_results` (NoxToolsTest, NoxNetTest), `package_game` / `get_package_result`. |
| `McpGraph.h/.cpp` | A node graph (`NodeGraph`) as JSON and back: links by pin name, property values typed by the node type's declarations, states and transitions, every reference checked; a domain's node types as JSON. |
| `McpToolHelpers.h/.cpp` | The editor tools' helpers: asset type names, assets by uuid or path, paths under the asset folder (the scene helpers come from NoxMcp). |
| `NoxEditor/src/ScriptDiagnostics.h/.cpp` | The script build's output as data: each error and warning once with file, line, column, code and message (used by the Output Log and `build_scripts`). |

## NoxRuntime's MCP server (`NoxRuntime/src/McpRuntime.h/.cpp`, MCP5)

`McpRuntime::FromCommandLine` starts it when the command line has `--mcp`: port `--mcp-port` (default 8770), access `--mcp-access=read|write|execute` (default write), the project's token. Its tools: the shared game tools plus `get_runtime_status` (project, scene, network role and session, player name, frame rate, entities, paused, newest log sequence) and `quit` (Execute; answers, then the game quits a quarter of a second later). `RuntimeLayer::OnUpdate` runs its calls first each frame; its destructor removes it before the session and the scene. Pause works in a standalone game only (a networked game's other side keeps running). A dedicated server offers neither `screenshot` (it draws nothing) nor `inject_input` (no local player); its `status` and `clients` commands answer through their output, so `run_command` returns them. Without MCP in the build (`NOX_DIST`, `NOX_WITH_MCP=OFF`) the file is not compiled and `--mcp` only logs that it is not there.

The editor's Play setting **Players' MCP servers** (or `play` with `player_mcp: true`) passes `--mcp --mcp-port=<editor port + n> --mcp-access=<the editor's level>` to player n (2..8) and `+ 1` to the dedicated server, after making the token once (so the games do not each make one). A `ServerProbe` per game checks that its server really answers and logs `MCP: player 2's MCP server answers on ...`, or after 60 s warns that it never did (an old NoxRuntime, built before MCP5, ignores `--mcp`; building NoxEditor does not build NoxRuntime). `EditorContext::Runtimes` lists them with whether each process still runs and its server's state (starting / ready / not listening); `runtime_call` picks one by player number (0: the dedicated server) and says plainly when its server is still starting or never opened.

## How a tool call runs

1. An HTTP thread checks the token, Host and Origin, parses JSON-RPC and validates the arguments against the tool's schema and the granted access.
2. The call is queued; the host runs it on its main thread between frames (`McpEditor::Update` first in `EditorLayer::OnUpdate`, `McpRuntime::Update` first in `RuntimeLayer::OnUpdate`).
3. Most tools answer at once; a screenshot answers when the GPU readback arrives, `wait_for` / `inject_input` / `play` / `runtime_call` from per-frame polls.
4. Edits go through `SceneSerializer` as loading does, are read back, and become one undo step (`EditorHistory::FrameInfo::ExternalStep` compares the scene text); a failing `apply_edits` restores the snapshot.

## Input injection

`Input` keeps automation input apart from the devices: `SetInjectedKey`, `SetInjectedMouseButton`, `AddInjectedMouseMotion`, `AddInjectedScroll`, `ClearInjected`. Game scripts read `Input::IsGameKeyDown` / `IsGameMouseButtonDown` / `GetGameMouseDelta` / `GetGameScroll` -- the devices while the viewport has them, plus what was injected -- so an agent drives the game while its window is not in front (the editor or a NoxRuntime window); the editor's own camera and tools read only the devices. Leaving Play and stopping a server release everything.

## Security and removal

Loopback only, a 32-byte token in `<Project>/.nox/mcp-token`, Host / Origin checks against DNS rebinding, User-level editor settings a client cannot change, every call logged (never the token). A game's server runs only when its command line asks (`--mcp`) and never in a shipping build. CMake `NOX_WITH_MCP=OFF` leaves the `NoxMcp` library, the editor's `Mcp` folder and `McpRuntime` out; EditorLayer has a few `#if NOX_WITH_MCP` spots, RuntimeLayer `#if NOX_RUNTIME_MCP`.

## Tests

`Tests/ToolsTest.cpp` + `Tests/McpTests.inl` (target **NoxToolsTest**, links NoxMcp): the console (parsing, ranges, access levels, aliases), the protocol (HTTP checks, versions, validation, access), the HTTP server end to end (concurrent clients, exclusive port, Stop with a call waiting), the client `runtime_call` uses against a real server (tools/list without initialize, a call with an image forwarded unchanged, tool errors, refused access, wrong token, a server that is gone), the server probe (ready, not listening, cancelled), the token file (made, read again, rotated, `.gitignore`), component validation and YAML round trips, `inject_input`'s timing, material and graph codecs -- 162 checks, also under AddressSanitizer / UBSan. The tools themselves (editor and runtime) are checked by using them in NoxEditor and NoxRuntime.
