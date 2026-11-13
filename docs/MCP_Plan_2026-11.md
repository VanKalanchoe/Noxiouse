# MCP for NoxEditor -- plan (2026-11-13)

Status: **planned, nothing built** except the two pieces it builds on, made the same day: the in-memory log (`LogBuffer`) and the
editor's Output Log tab. Backlog item N7.

## 1. What MCP is

The Model Context Protocol is an open protocol (JSON-RPC 2.0) through which an AI client -- Claude Code, Claude Desktop, other
editors' assistants -- calls **tools** ("create an entity", "press Play"), reads **resources** ("the open scene", "the log") and
uses **prompts** that a **server** offers. The server is a program you own: here, a small part of NoxEditor. Two transports:

- **Streamable HTTP**: the server listens on a URL (`http://127.0.0.1:8765/mcp`); the client posts JSON-RPC requests and can receive a
  stream of messages back. Claude Code: `claude mcp add --transport http nox http://127.0.0.1:8765/mcp --header "Authorization: Bearer <token>"`.
- **stdio**: the client starts the server as a child process and talks over stdin / stdout (for clients that only do that, a tiny
  `NoxMcpBridge` executable forwards stdio to the editor's HTTP endpoint).

The model never touches the engine directly: it can only do what a tool offers, with the arguments the tool's JSON schema allows, and
sees only what the tool returns (text, JSON, images).

## 2. What works today without MCP, and what MCP adds

**Today** (Claude Code in the repository, as in this project's sessions): reading and editing every text file -- C# scripts, scenes
(`.nox` YAML), prefabs, materials, animation / audio graphs, the engine's C++ -- building, running the offline tests (NoxNetTest),
reading the logs you upload, writing test guides. What it cannot do: see or touch the **running** editor -- its live scene, the
selection, the viewport, Play -- so every check of behaviour goes through you ("press Play, send the log").

**With MCP** the assistant drives the running NoxEditor itself: looks at the scene as it is, makes and changes entities and components
(each change an undo step), imports assets, saves, presses Play / Stop (also multiplayer Play), waits for something to happen,
reads the Output Log, takes screenshots of the viewport, injects input for scripted gameplay checks, builds scripts and reads the
compiler's errors. The loop "change -> run -> look -> fix" no longer needs you in the middle for every step.

## 3. Architecture

```
Claude Code / Desktop --HTTP (JSON-RPC)--> McpServer thread (NoxEditor) --queue--> main thread: EditorLayer runs the tool between frames
                                                   <---------------- result (JSON / text / PNG) ----------------
```

- **McpServer** (`NoxEditor/src/Mcp/`): an HTTP listener on 127.0.0.1 on its own thread (cpp-httplib, MIT, one header, vendored),
  JSON with nlohmann/json (already vendored in tinygltf). It implements `initialize`, `ping`, `tools/list`, `tools/call`,
  `resources/list`, `resources/read`, notifications, and the protocol's version negotiation.
- **Main thread only**: the editor, ImGui, the scene and the asset manager are not thread-safe. A tool call is queued and run at the
  start of `EditorLayer::OnUpdate` (between frames); the HTTP thread waits for its result (with a time-out). Tools that wait for the
  game ("Play for 5 s", "wait until the log says X") hold their reply while frames keep running.
- **Tool registry**: each tool = name, description, JSON schema of its arguments, a handler. `tools/list` is generated from it.
- **Undo**: an MCP edit happens outside any ImGui interaction, so EditorHistory takes it as one step at the end of the frame; the
  step is named "MCP: <tool>" (a name hook in `EditorHistory::FrameInfo`). Ctrl+Z undoes what the assistant did, like your own edits.
- **Components without hand-written tools**: `SceneSerializer::EntityToNode` / `DeserializeEntityComponents` already turn any
  entity's components to and from YAML. A YAML <-> JSON bridge gives `get_entity` / `set_component` every component the scene file
  knows, current and future; script fields through their `[Expose]` metadata.
- **Security**: off by default (Edit > Preferences > MCP server, with the port); listens on 127.0.0.1 only; a random token written
  to `<Project>/.nox/mcp-token` must be sent as `Authorization: Bearer`; the `Origin` header is checked (the spec's DNS-rebinding
  rule); every tool call is written to the Output Log ("MCP: set_component Player.Transform"); destructive tools (delete assets or
  files, overwrite scenes) ask in the editor unless allowed in the preferences. Text inside scenes, logs or assets is data for the
  model, never instructions.

## 4. Phases and tools

| Phase | What | Tools / resources |
|---|---|---|
| MCP0 | Server, handshake, preferences toggle, token, status in the status bar | `editor_status` (project, scene, mode, selection, fps) |
| MCP1 | Read the editor | `get_hierarchy` (paged, filtered: 400 boxes must not flood the model), `get_entity` (components as JSON), `find_entities` (name / component / script / level), `list_assets` / `search_assets`, `get_asset` (type, path, dependencies), `read_log` (since a sequence, by level / text: the `LogBuffer`), `get_selection`, `get_stats` (frame time, draw / memory stats, net status); resources: the open scene's YAML, Nox.log, the docs |
| MCP2 | Edit (undoable) | `create_entity`, `delete_entities`, `duplicate`, `set_parent`, `set_component` (merge a JSON patch), `add_component` / `remove_component`, `set_script_field`, `instantiate_prefab`, `select`, `open_scene`, `save_scene`, `undo` / `redo`, `create_material` / `set_material`, `import_asset` (glTF, textures, sounds with their import settings), node graphs as JSON (`get_graph` / `set_graph`: animation and audio graphs' nodes, pins, links) |
| MCP3 | Play and look | `play` / `stop` / `simulate` (standalone, listen server, client with N players, network debug log), `wait_for` (seconds, a log line matching text, an entity condition), `screenshot` (the viewport read back to a PNG image result; a camera to shoot from, or focus an entity), `inject_input` (keys / mouse held for a time in Play: scripted gameplay checks), `set_camera` |
| MCP4 | Code and build | `build_scripts` (the ScriptBuilder's errors as structured file / line / message), `reload_scripts`, `run_net_tests`, `package_game` / `package_server` |
| MCP5 | NoxRuntime | the same server in NoxRuntime (`--mcp`): status, the server console's commands, screenshots and input in a client -- automated multiplayer tests without the editor |

## 5. What has to change in the engine

1. **Main-thread command queue** in EditorLayer (tool calls run between frames; replies through futures).
2. **Dependencies**: cpp-httplib (new, one header); nlohmann/json (reuse tinygltf's copy).
3. **YAML <-> JSON** for components (on top of SceneSerializer).
4. **Viewport screenshots as tool results**: the pieces exist -- `Renderer::ReadbackTexture` and the project screenshot
   (`EditorLayer.cpp`, viewport -> PNG with stb_image_write); the tool needs the PNG in memory (base64 image content) instead of a
   file, and a callback that completes the waiting tool call a frame or two later.
5. **Input injection**: `Input::IsKeyDown` / mouse state consult an injected state too (Play only), so scripts see injected keys.
6. **EditorHistory step names** for edits from outside the UI.
7. **ScriptBuilder diagnostics** as data (file, line, column, message), not only log lines.
8. **LogBuffer** -- done (2026-11-13): the recent log lines in memory with sequence numbers, readable from any thread.

## 6. What it cannot do (or not well)

- **Not real time**: every call is a round trip (a tenth of a second to seconds). It can run scripted input sequences and check the
  result; it cannot play reactively like a person (aiming, dodging).
- **Vision is snapshots**: screenshots, not video. It can tell "the character is missing / the lighting is black / the UI overlaps",
  not subtle motion problems (flicker, ghosting, a T-pose pop lasting one frame) -- those still need you, or frame sequences and
  numbers (the profiler, the network debug log).
- **Engine C++ changes** are file edits + a rebuild + an editor restart: done the way it is today, not through MCP (the MCP
  connection drops while the editor restarts).
- **Only what a tool exposes**: ImGui-only interactions (dragging the gizmo, laying out a node graph canvas by hand) are out of
  reach until a tool covers their data; the generic component JSON covers most of the scene.
- **No binary authoring**: it cannot paint textures, model meshes or record sounds; it can import files it is given and make
  procedural content through scripts.
- **GPU debugging / deep profiling** only through exported data (profiler captures, stats), not RenderDoc-style inspection.
- **Big scenes**: answers must be paged and filtered to fit the model's context.
- **Where the assistant runs**: a Claude Code session on your machine (or one attached to it) reaches `127.0.0.1`; a cloud session
  like this repository's cannot see your editor unless connected to your computer.
- **Trust**: whatever a tool allows, the model can do -- the toggle, the token, the confirmations and the Output Log trail are there
  for that.

## 7. Testing

- **Offline, in this container**: the protocol layer (McpServer with a fake tool registry) against JSON-RPC requests -- initialize,
  version negotiation, tools/list schemas, errors, token / Origin refusal, time-outs -- like NoxNetTest.
- **On your machine**: NoxEditor with the MCP server on, Claude Code connected with `claude mcp add ...`; each phase ships a test
  guide naming the program for every step (NoxEditor, Claude Code).
