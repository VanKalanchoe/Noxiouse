# Runtime player, runtime asset manager and packaging plan -- October 2026

> Future.md items 7 (runtime asset manager + runtime project, to test without the editor's overhead), 8 (how do we package
> a distribution build -- a PCH? how do modern engines do it?) and 11 (a Minecraft-style game: script or C++, and what to
> expose). Closes Engine plan open decision **D9 "Editor vs runtime asset managers"** and prefab backlog item **P6**
> (exported games need their own prefab lookup). Nothing here is built yet.

## 1. Where it stands (checked in code, 2026-10-18)

- **Only one executable:** `NoxEditor` (`Main.cpp` pushes `EditorLayer`, `ApplicationSpecification::isEditor = true`). The
  engine already has a non-editor path in places (`Renderer::getOutputSize` returns the swapchain size when `!m_isEditor`;
  `Scene::OnRuntimeStart / OnUpdateRuntime / OnRuntimeStop` exist and are what Play uses).
- **`RuntimeAssetManager` is an empty class** (`Asset/RuntimeAssetManager.h/.cpp`), and `Project::Load` always creates an
  `EditorAssetManager`.
- **Core code reaches into the editor asset manager** in 5 places, which a runtime build must replace:
  `Renderer.cpp:89, 131` (walks the registry), `PrefabInstance.cpp:1212, 1398` (metadata of a prefab),
  `DotNetBackend.cpp:49` (`Scene.Instantiate` finds a prefab by path -- this is P6).
- **Assets are cooked already** (`.nmesh/.nsmesh/.ntex/.nmat/.nanim/.nskel/.nprefab`, scenes `.nox` YAML), one file per
  asset next to the sources, registry in YAML. Shaders are compiled at run time by Slang with a SPIR-V cache.
- **Scripts:** C# through Coral (HostFXR / CoreCLR), game assembly built by the project.

## 2. How the big engines package (the answer to item 8)

A distribution build is **three things**, none of which is a precompiled header:

1. **A runtime executable without the editor** -- Release build, editor code compiled out (UE: the "Game" target vs the
   "Editor" target; Unity: the player; Godot: export templates).
2. **Cooked content in a few big container files** -- only what the game references, already in GPU-ready formats,
   with a table of contents and usually compression (UE: `.pak` / IoStore `.utoc` + `.ucas`; Unity: `data.unity3d` /
   asset bundles / Addressables; Godot: `.pck`). Loading becomes "seek + read + decompress" with no importer, no YAML,
   no file-per-asset directory walks.
3. **Precompiled shaders and runtime dependencies** -- shader bytecode / pipeline caches shipped with the game (no shader
   compiler on the player's machine), plus the runtimes the game needs (for us: the .NET runtime pieces Coral loads).

**A PCH (precompiled header) is only a compile-speed tool** for building the engine: it changes how fast the code compiles,
not what ships. It is worth adding separately (`target_precompile_headers` on NoxCore with the STL, glm, EnTT, spdlog
headers; measure the build time before and after), but it is unrelated to packaging.

## 3. Phases

| Phase | What | Exit test |
|---|---|---|
| **R1 Runtime player** | New executable target `NoxRuntime` (`NoxRuntime/src/Main.cpp` + `RuntimeLayer`): opens the `.nproj` given on the command line (or next to the exe), loads `ProjectConfig::StartScene`, runs `OnRuntimeStart` / `OnUpdateRuntime` / `OnRuntimeStop`, fullscreen-capable window, `isEditor = false`, no ImGui except the F3 stats overlay. Uses the existing `EditorAssetManager` read-only (no file watcher, no cooking) so it works right away on the cooked project | `NoxRuntime Facerun/Facerun.nproj` plays the start scene; frame time compared to the editor's Play mode in the stats report |
| **R2 Asset manager split (D9)** | `AssetManagerBase` gets the read-only lookups core code needs (`GetMetadata(handle)`, `FindHandleByPath(path, type)`, registry iteration); the 5 call sites above use those instead of `GetEditorAssetManager()`. `RuntimeAssetManager` implements them from a runtime registry (binary, handle -> type + path / pack offset) and loads cooked files only; `Project::Load` picks the manager by `isEditor`. Prefab lookup for `Scene.Instantiate` comes from the runtime registry (closes P6) | Runtime player runs on `RuntimeAssetManager`; the editor is unchanged |
| **R3 Cook for distribution** | Editor menu "Build > Cook Content": starting from the build's scene list, collects every referenced asset (scene -> entities -> meshes -> materials -> textures, prefabs, animation graphs, plus prefabs named in scripts through a "always cook" list), makes sure each is cooked and current, writes the runtime registry | The cooked set equals what the scenes reference; nothing unused is shipped |
| **R4 Pack files** | `.npak`: header, TOC (handle, offset, size, compressed size, type), 4 KB-aligned blobs, LZ4 per blob (textures stay BC-compressed). `RuntimeAssetManager` reads through the async IO path (Engine plan §5.10.3 containers, §5.11) | Start scene loads from `Content.npak` with the loose files deleted |
| **R5 Shaders** | Ship the SPIR-V cache for every pipeline the game uses (collected by running the cook with a "compile all pipelines" pass) and a Vulkan pipeline cache; the runtime never links Slang | Runtime starts with Slang removed from the output folder |
| **R6 Scripts** | Ship the game assembly + `Nox.ScriptCore.dll` + the .NET runtime files Coral needs (framework-dependent first, self-contained later); no hot reload in the player | Scripted gameplay works from the output folder on a machine without the SDK |
| **R7 Build Game** | Editor menu "Build > Build Game": runs R3-R6 into an output folder (`MyGame.exe`, `Data/*.npak`, `DotNet/`, `MyGame.nproj` runtime config), Release config, `NOX_EDITOR` off (editor-only code compiled out) | Zipped folder runs on a clean Windows machine |

## 4. A Minecraft-style game: script or C++? (item 11)

**Both, split by cost.** Every engine that ships voxel games does it this way: the hot, data-parallel parts are engine
code, the rules of the game are scripts. (Unity voxel games use C# with the Jobs system + Burst for exactly the hot parts;
UE ones use a C++ plugin such as Voxel Plugin and Blueprint for gameplay.)

| Part | Where | Why |
|---|---|---|
| Chunk storage (16x16xN blocks, palette compression), world streaming around the player | C++ engine module (`NoxCore/Voxel/`) | millions of blocks, cache-friendly layouts, saved in its own format |
| Terrain generation (noise, caves, biomes, structures) | C++ jobs on the `JobSystem`, parameters from a C# / asset side | thousands of chunks, must use every core; a managed call per block is far too slow |
| Meshing (greedy meshing, face culling, AO per vertex) and GPU upload | C++ jobs -> `UploadManager` -> GPU scene; one mesh per chunk section, BLAS per section for ray tracing | same reason; it must feed the meshlet / BLAS pipeline the renderer already has |
| Block lighting / sky light propagation | C++ jobs | flood fills over many blocks |
| Physics against blocks | C++: a voxel collider for Jolt (heightfield / compound per chunk section) | Jolt is C++ |
| Player controller, block placing / breaking rules, inventory, crafting, UI, mobs' behaviour, game modes | **C# scripts** | game rules change often, hot reload, no engine rebuild |

**What to expose to scripts** (through `InternalCalls.cs`, like the existing rigid body / animator calls):
`VoxelWorld.GetBlock / SetBlock / FillBox`, `VoxelWorld.Raycast(origin, direction) -> hit block + face`,
`VoxelWorld.OnChunkLoaded / OnBlockChanged` events, block type registration (id, textures, solid / transparent, light
level), and generator parameters (seed, noise settings as an asset). Two general APIs are worth adding for any game, not
only voxels: a **dynamic mesh API** (a script fills vertices / indices, the engine builds meshlets and uploads) and later a
**job API** for scripts (schedule work on the worker threads with blittable data, like Unity's Jobs; needs care with
CoreCLR thread attachment, so after the C++ voxel module exists).

## 5. Order

R1 (quick win, testing without editor overhead) -> R2 (also unblocks P6) -> R3 -> R5 -> R6 -> R4 -> R7. The voxel module
(§4) is independent and can start any time after R1, since it is game-side engine code.
