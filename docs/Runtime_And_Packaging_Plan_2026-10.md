# Runtime player, runtime asset manager and packaging plan -- October 2026

> Future.md items 7 (runtime asset manager + runtime project, to test without the editor's overhead), 8 (how do we package
> a distribution build -- a PCH? how do modern engines do it?) and 11 (a Minecraft-style game: script or C++, and what to
> expose). Closes Engine plan open decision **D9 "Editor vs runtime asset managers"** and prefab backlog item **P6**
> (exported games need their own prefab lookup).
>
> Status 2026-10-20: **R1-R7 built, all untested** (sections 2b-2e), and section 4b (run-time meshes, jobs, mesh
> colliders and raycasts for scripts, untested). Not done: pack compression, a binary registry, an installer. Section 1
> describes the code as it was on 2026-10-18, before any of it.

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

## 2b. R1 built (2026-10-18, untested)

- `NoxRuntime/` (`Main.cpp`, `RuntimeLayer.{h,cpp}`), built when CMake gets **`-DNOX_BUILD_RUNTIME=ON`** (off by default
  until it has been tried, so it cannot break the editor build). The exe is placed next to `NoxEditor.exe`, which already
  has the managed scripts, Coral, slang, Streamline and NRD binaries; building NoxRuntime builds NoxEditor first for that.
- Run: `NoxRuntime <project.nproj>`; without an argument it looks for a `.nproj` in the working directory, next to the exe,
  then in `../Facerun` (in Visual Studio the debugger is set up to start it in `NoxEditor/` with the Facerun project).
  Working directory: `NoxEditor/` like the editor (engine shaders and fonts are found from there).
- What it does: loads the project, plays `StartScene` exactly like the editor's Play (a copy of the scene asset,
  `OnRuntimeStart`, `OnUpdateRuntime`), the window is the viewport, F3 stats overlay, Esc quits. No ImGui, no editor
  camera, no picking / outline / grid, no undo snapshots, no file watcher.
- Still the editor's asset manager underneath (read-only, assets must have been cooked by the editor once); renderer
  settings are the renderer's defaults (the editor's Settings panel choices are not saved anywhere yet).
- To compare: Play in the editor vs NoxRuntime on the same scene, both with F3 (Ctrl+F3 writes a stats report).

## 2c. R2 first part built (2026-10-18, untested)

- Engine code no longer reaches into the editor's asset manager: `AssetManagerBase` has the read-only lookups
  `GetMetadata`, `FindHandleByPath`, `GetAssetCount` (wrapped by `AssetManager::`), and the five call sites use them --
  the renderer's texture-by-path lookup (now the registry's path index instead of a walk over every entry), its
  "registry grew" check, the two prefab-save paths in `PrefabInstance.cpp`, and C#'s `Scene.Instantiate` (P6: the prefab
  lookup goes through whichever asset manager is active).
- **Changed plan:** a separate `RuntimeAssetManager` moves to R4. Loading (background loads, uploads, texture streaming,
  the unused-asset sweep) lives inside `EditorAssetManager` today and registers textures / materials it discovers while a
  model loads; a runtime manager needs the cooked set and runtime registry of R3 to know everything up front, and the pack
  reader of R4 to read it. Until then the player uses the editor's manager, which works because the editor has cooked
  everything once.
- **`RuntimeAssetManager` built (2026-10-18, untested):** the editor's loading pipeline in read-only mode
  (`EditorAssetManager(ReadOnlyTag)`; `Project::Load(path, true)`, which NoxRuntime uses). Same background loads, uploads,
  texture streaming, unused-asset sweep and memory assets; off: the file watcher and reimports, source hashing after a load,
  folder scans for new cooked files, registry writes (and the start-up check that drops entries without files, with its
  `.bak`), writing a missing `.nmat` (the mesh's embedded material is used), and the texture color-space correction (which
  deletes and recooks a `.ntex`). So the player never changes the project or a packaged game's folder on disk. Engine code
  keeps reaching both through `GetEditorAssetManager()` (the runtime one is a subclass); the pack reader (R4) replaces its
  file reads.

## 2d. R3 + R6 + a first R7 built (2026-10-18, untested): File > Package Game

- **File > Package Game...** (editor, `NoxEditor/src/GamePackager.{h,cpp}`): an output folder (default `<project>/Build/<Name>`)
  and "every scene" (else the start scene). It saves the open scene, then writes:
  - `<Output>/<Name>.exe` -- NoxRuntime, renamed (it must have been built: CMake `-DNOX_BUILD_RUNTIME=ON`), plus everything
    else next to the editor's executable (engine DLLs, the .NET host, `Scripts/` with the managed assemblies, the `cache/`
    folder) except the editor itself, `.pdb / .ilk / .exp / .lib / .log` and `imgui.ini` (R6: framework-dependent).
  - `<Output>/assets/{shaders,fonts,textures}` -- the engine's assets (shaders are still compiled at start: R5 is not done).
  - `<Output>/Game/<Name>.nproj` and `Game/<AssetDirectory>/` with **only the assets the scenes need** (R3,
    `EditorAssetManager::CollectDependencies`): the B3 reference rules read forwards -- what each file names by handle or path,
    what a mesh owns by name, every asset cooked from the same model as a mesh (a model instance spawns them) -- starting at the
    scenes plus every script (which name prefabs by path). Cooked files ship without their sources (a texture's `.ntex`, a mesh's
    `.nsmesh`, with their `.hash` / `.notifies` companions); a `.gltf` placed as itself ships with its buffers and images. The
    registry lists just those (`SerializeAssetRegistry(path, handles)`).
- **Importers take a cooked file as it is when its source is gone** (`MeshImporter::ReadCookedMesh`,
  `TextureImporter::ImportTexture2D / ReadCookedTextureHeader`): what makes a package without sources load. In the editor this
  only matters when a source file was deleted (its cooked file keeps working instead of failing).
- NoxRuntime also looks for the project in `Game/` next to the executable, so double-clicking `<Name>.exe` plays it.
- Not yet: pack files (R4: still one file per asset), precompiled shaders (R5), a Release / editor-free build of the runtime,
  the game assembly's name is still `Facerun.dll` in `ScriptEngine` (one game per engine build for now).

## 2e. R4, R5, R6, R7 built (2026-10-20, untested)

**How a packaged game holds and reads its content.** Nothing is "converted into memory" up front. The assets are cooked to
their final, GPU-ready form (textures block-compressed `.ntex`, meshes `.nsmesh` with meshlets, scenes, prefabs, materials) and
written into one pack file, `Game/Content.npak`, with a table of contents (path -> offset, size). The game opens the pack at
start and keeps only that table in memory; when the asset manager loads an asset it reads that asset's bytes from its offset,
the same background loads, uploads and texture streaming (which reads just the mip range it needs) as in the editor, and
unloads what the scenes no longer use. Shaders ship precompiled, scripts as their built DLLs.

- **Pack files (R4):** `NoxCore/Core/VirtualFileSystem.{h,cpp}`. `.npak`: magic `NOXPAK01`, version, entry count, offset of
  the table of contents; 16-byte aligned file bytes; the table (path, offset, size). Uncompressed (textures are already BC
  compressed; LZ4 per blob can come later). `VFS::Open / ReadBytes / ReadText / ReadRange / Exists / FileSize` read the loose
  file when there is one, else the entry of a mounted pack -- every asset read goes through it (cooked meshes, skeletons,
  animations and their notifies, materials, textures, scenes, prefabs, timelines, node graphs, fonts, the registry). A runtime
  project (`Project::Load(path, true)`) mounts every `*.npak` next to its `.nproj` over its asset directory before the
  registry is read. The editor mounts nothing. File > Package Game writes `Game/Content.npak` (option "Pack files", on).
- **Precompiled shaders (R5):** Slang is delay-loaded on Windows and its session created on the first compile; SPIR-V is
  cached (`.spv`) next to the shader-object binaries for every pipeline kind (ray tracing pipelines compiled on every start
  before), and is the fallback when a driver rejects the shader-object binaries. The package leaves Slang out ("Precompiled
  shaders only", on): every pipeline the editor created is in the cache (the present pipeline is now created in the editor
  too). A shader that never compiled in the editor fails with a clear message -- turn the option off to ship Slang.
- **Texture cook:** a texture never loaded in the editor is cooked while packaging instead of shipping its PNG/JPG.
- **Scripts (R6):** the project's own assembly (`ScriptModulePath`'s name) in `Scripts/<Name>/`, other games' scripts built
  next to the editor left out. **Self-contained .NET** (option): the .NET 9 host resolver and shared framework copied into
  `dotnet/`; Coral (patched at configure time) and the host look there first (`NOX_DOTNET_ROOT`, `DOTNET_ROOT`).
- **Shipping build (R7):** CMake `-DNOX_DIST=ON` with the Release configuration: NoxRuntime without a console window, no
  stats overlay / profiler, the log in the user's folder (`SDL_GetPrefPath`). Package from an editor built that way.
- **Window:** Windowed / Borderless (default) / Fullscreen (exclusive) from the project's `WindowMode`; Alt+Enter toggles;
  `--windowed` on the command line for testing.
- **Renderer settings in the project:** `ProjectConfig::Renderer` (ray tracing resolution, diffuse GI, shadowed lights, OIT,
  TAA) on top of the quality preset, applied in the editor and the game (Render Quality > Project Settings).

Still not done: compression in packs, a binary registry, several packs (DLC / patches are possible already: packs mount in
name order and a loose file wins), an installer.

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

### 4b. Built (2026-10-18, untested): run-time meshes, jobs, mesh colliders and raycasts for scripts

The two general APIs, enough to write a voxel game in C# today (the C++ voxel module stays the path for big worlds):

- **`Nox.Mesh`** (`Mesh.cs`, native `NoxCore/Renderer/DynamicMesh.{h,cpp}`): `new Mesh()`, `SetData(positions, indices,
  normals?, uvs?)` or `SetData(MeshBuilder)`, `Clear()`, `SetColor(r, g, b, a, metallic, roughness)` (alpha < 1 = see-through,
  an emissive overload), `SetMaterial("Materials/X.nmat")`, `Dispose()`. Natively each mesh is a `StaticMesh` with one
  submesh held by the asset manager as a **memory asset** (`AssetManagerBase::AddMemoryAsset`: no file, no registry entry,
  answers like a loaded asset), so a `MeshComponent` naming its handle goes through the normal path: GPU scene, meshlet
  culling, shadows, BLAS / ray tracing, picking, outline. `SetData` validates the arrays, works out smooth normals when none
  are given, tangents (MikkTSpace from UVs, or any orthonormal one without), builds one level of meshlets
  (`MeshImporter::BuildRuntimeGeometry`, no LOD chain: cheap to rebuild), uploads, swaps the submesh in under the same
  handle and frees the old geometry; `Scene::SyncGpuScene` re-registers the entities drawing a changed mesh in the same
  frame (`DynamicMeshes::ConsumeChanged`). Meshes live until `Dispose` or until Play stops (`Scene::OnRuntimeStop` ->
  `DynamicMeshes::DestroyAll`).
- **`Nox.MeshBuilder`**: plain lists (`AddVertex`, `AddTriangle`, `AddQuad`), fillable inside a job.
- **`Nox.Jobs`** (`Jobs.cs`): `Jobs.Run(() => ...)` -> `JobHandle` / `JobHandle<T>` (`IsCompleted`, `TryGetResult`,
  `Complete`), `Jobs.ParallelFor(count, i => ...)`. They run on .NET's own thread pool, so no native thread has to attach to
  CoreCLR; the rule is that a job computes data only. Every new engine call checks it is on the main thread and refuses
  (with a log line saying why) otherwise.
- **`Entity.SetMesh(mesh)`**, **`Scene.CreateEntity(name, position)`**.
- **`Entity.SetMeshCollider(mesh, friction, restitution)`**: a static Jolt `MeshShape` from the mesh's triangles at the
  entity's transform (`IPhysics3DScene::SetMeshCollider` / `RemoveMeshCollider`; removed with the entity). Characters walk on
  it, rigid bodies land on it. Set it again after the data changes or the entity moves.
- **`Physics.Raycast(origin, direction, maxDistance, out RaycastHit)`**: entity, point, normal, distance.

Example: `Facerun/Assets/Scripts/Source/VoxelTerrain.cs`, a 4 x 4-chunk Minecraft-style terrain (fractal value noise,
16 x 16 x 48 blocks per chunk, only faces next to air), generated and meshed in jobs, made solid with mesh colliders; left
click digs, right click places (raycast from the entity named "Camera"), rebuilding the chunk and the neighbours a border
edit uncovers on the main thread.

Not done: uploads wait for the copy (`Renderer::UploadMesh`), so re-meshing many chunks in one frame stalls; vertex colors
(one color or material per mesh: split by material, or use a texture atlas through UVs); an alpha change after `SetData`
keeps the ray tracing structure's opaque flag (set the color first); the mesh collider is static only; the C++ voxel
module of the table above.

## 5. Order

R1 (quick win, testing without editor overhead) -> R2 (also unblocks P6) -> R3 -> R5 -> R6 -> R4 -> R7. The voxel module
(§4) is independent and can start any time after R1, since it is game-side engine code.
