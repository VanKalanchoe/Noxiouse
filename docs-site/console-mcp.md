# Console, Output Log & MCP Server (`NoxCore/Console`, `NoxEditor/src/Mcp`)

The engine console is one registry of commands and variables in NoxCore that every front end uses: the editor's Output Log Cmd line, the dedicated server's terminal, `--exec`, and the editor's MCP server, which lets AI assistants read, edit and play the scene, build its scripts and work with its assets. Plan and status: `docs/MCP_Plan_2026-11.md` (C1 console, MCP0-MCP3 and MCP4 part 1 built; MCP4 part 2 and MCP5 open). How to use them: the manual's **The Engine Console** and **AI Assistants (MCP Server)** pages.

---

## Console and log files

| File | Role |
| :--- | :--- |
| `NoxCore/src/NoxCore/Console/Console.h/.cpp` | The registry: commands (`RegisterCommand`, RAII `Registration`), typed variables (`TConsoleVariable<bool / int32 / float / string>` with ranges, defaults, `OnChanged`), aliases, `Execute(line, maxAccess)` with quoting and `;`, completion, `help` / `find`, access levels Read / Write / Execute / User. |
| `NoxCore/src/NoxCore/Core/LogBuffer.h/.cpp` | The last 20 000 log lines in memory, filled by a spdlog sink every logger writes to; `CopySince(sequence)` for the Output Log and `read_log`. |
| `NoxEditor/src/Panels/OutputLogPanel.h/.cpp` | The Output Log tab: level filters, search, select / copy, auto-scroll, the Cmd line with history, Tab completion and suggestions. |

## MCP server files (`NoxEditor/src/Mcp/`)

| File | Role |
| :--- | :--- |
| `McpProtocol.h/.cpp` | JSON-RPC 2.0 over Streamable HTTP (revisions 2025-06-18 and 2025-03-26): token (constant-time compare), Host / Origin checks, initialize, tools/list filtered by the granted access, tools/call with schema validation; the `Schema` builder; token generation. Testable without the editor. |
| `McpServer.h/.cpp` | cpp-httplib (`NoxCore/vendors/cpp-httplib`, a git submodule) on 127.0.0.1 with an exclusive port; tool calls queued to the editor's main thread (`Pump`), the HTTP thread waiting up to 60 s; `Stop` answers waiting calls. |
| `McpEditor.h/.cpp` | The editor side: the `mcp.*` console entries (User access), `.nox/mcp.ini`, the token file with its `.gitignore`, Tools > MCP Server, per-frame polls for tools that answer later. |
| `McpEditorTools.h/.cpp` | `EditorContext` (the only way tools reach the editor; EditorLayer fills it) and the reading tools: status, log, hierarchy, entities, selection, assets, screenshot, console. |
| `McpEditTools.cpp` | Editing tools: entities, components (about 40 editable types), `apply_edits` (atomic via a scene snapshot), select, save, undo, redo. |
| `McpPlayTools.cpp` | play / simulate / stop / pause, `wait_for`, `inject_input`, the editor camera tools. |
| `McpAssetTools.cpp` | MCP4: `build_scripts` / `get_build_status`, `get_material` / `create_material` / `set_material`, `create_prefab`, `import_asset` (`instantiate_prefab` is an edit tool). |
| `McpMaterial.h/.cpp` | A material (`MaterialData`, `.nmat`) as JSON and back from one field table; the texture paths it names. |
| `NoxEditor/src/ScriptDiagnostics.h/.cpp` | The script build's output as data: each error and warning once with file, line, column, code and message (used by the Output Log and `build_scripts`). |
| `McpValidation.h/.cpp` | Component values as JSON: the example (C++ defaults under current values) as schema, checks, JSON Patch, differences, YAML <-> JSON (ids as strings, 17-digit doubles). |
| `McpInputPlayer.h/.cpp` | `inject_input`'s timing: holds per frame, shared keys, motion spread over the time. |
| `McpToolHelpers.h/.cpp` | Shared helpers: ids, entity summaries, paths, paging. |

## How a tool call runs

1. An HTTP thread checks the token, Host and Origin, parses JSON-RPC and validates the arguments against the tool's schema and the granted access.
2. The call is queued; `McpEditor::Update` (first thing in `EditorLayer::OnUpdate`) runs it on the main thread between frames.
3. Most tools answer at once; a screenshot answers when the GPU readback arrives, `wait_for` / `inject_input` / `play` from per-frame polls.
4. Edits go through `SceneSerializer` as loading does, are read back, and become one undo step (`EditorHistory::FrameInfo::ExternalStep` compares the scene text); a failing `apply_edits` restores the snapshot.

## Input injection

`Input` keeps automation input apart from the devices: `SetInjectedKey`, `SetInjectedMouseButton`, `AddInjectedMouseMotion`, `AddInjectedScroll`, `ClearInjected`. Game scripts read `Input::IsGameKeyDown` / `IsGameMouseButtonDown` / `GetGameMouseDelta` / `GetGameScroll` -- the devices while the viewport has them, plus what was injected -- so an agent drives the game while the editor window is not in front; the editor's own camera and tools read only the devices. Leaving Play and stopping the server release everything.

## Security and removal

Loopback only, a 32-byte token in `<Project>/.nox/mcp-token`, Host / Origin checks against DNS rebinding, User-level settings a client cannot change, every call logged (never the token). CMake `NOX_WITH_MCP=OFF` leaves the `Mcp` folder out; EditorLayer has four `#if NOX_WITH_MCP` spots.

## Tests

`Tests/ToolsTest.cpp` + `Tests/McpTests.inl` (target **NoxToolsTest**): the console (parsing, ranges, access levels, aliases), the protocol (HTTP checks, versions, validation, access), the HTTP server end to end (concurrent clients, exclusive port, Stop with a call waiting), component validation and YAML round trips, and `inject_input`'s timing -- 95 checks, also under AddressSanitizer / UBSan. The editor-side tools are checked by using them in NoxEditor.
