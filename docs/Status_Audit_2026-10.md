# Status audit -- where the docs say we are, what the code says, what is missing (2026-10-18, second audit 2026-10-19)

> Item 1 of `Future.md`: "check first all docs md files where we are currently, what's missing". Every `.md` in the repo
> was read and its claims checked against the code at commit `509dd11` ("before unleashing claude"). This file is the
> snapshot; `Backlog_2026.md` stays the living index (the Future.md items are now in it as F1-F14).

## 0. Second audit (2026-10-19): what is broken, untested and missing -- read this first

Every `.md` (the 12 in `docs/` and the 4 at the root) was read again and each open item checked against the code at `69de5f3`
(after the screen-probe GI, light and ray tracing resolution work). Sections 1-5 below are the first audit (2026-10-18), kept
for history. File references are to the code as of this audit.

### 0.1 Broken -- bugs found in code or reported and not fixed

| Area | Bug | Where |
|---|---|---|
| Core | Fixed 2026-10-20: no `NOX_ASSERT` fired in any build (`NOX_DEBUG` was never defined; several asserts passed a string as the condition). Debug builds now define it; a failed assert logs and breaks only under a debugger | `Core/core.h`, `NoxCore/CMakeLists.txt` |
| Renderer | **ReSTIR DI** reported broken by the user (cause not found; possibly the 0.2 mm shadow offsets fixed in `e2506eb`); off in every preset, still selectable | `ReSTIRPasses.cpp:477` |
| Renderer | ReSTIR PT temporal reuse: lighting "rotation" artifact with a static camera; off by default | `EditorLayer.cpp:981` |
| Renderer | ReSTIR DI / PT assume light indices never change between frames (identity mapping): adding / removing a light corrupts their history for a frame | `RTXDI/DI/ReSTIRDITemporal.slang:110`, `PT/ReSTIRPTTemporal.slang:465` |
| Renderer | Meshlet cone culling was removed because it dropped visible meshlets; cause never found (cones still cooked, unread) | `MeshImporter.cpp:57`, `shaderIO.h` |
| Animation | Fixed 2026-10-20: root motion was lost on entities with a character controller (added to the Transform, overwritten by the controller). The controller now gets it as a velocity (`RootMotionVelocity`, horizontal) and moves by it with collisions | `Scene.cpp` (UpdateAnimators), `JoltPhysics3DScene.cpp` |
| Animation | Fixed 2026-10-20: the Fox locomotion `Speed` was sent in cm/s to thresholds in m/s (Walk -> Run always fired). `CharacterMovement.cs` sends m/s through the new `Nox.WorldUnits` | `CharacterMovement.cs` |
| Scripting | Fixed 2026-10-20: a failed hot reload left scripting unloaded. The new assemblies load into their own context first; the running module goes only once they have loaded | `DotNetBackend.cpp` (LoadModule) |
| Scripting | Fixed 2026-10-20: the reload generation never increased and `OnAfterReload` was never called. Every load is a new generation; `OnAfterReload` runs after the new instances' `OnCreate`, and exposed field values are kept across the reload | `ScriptEngine.cpp` (ReloadAssembly) |
| Scripting | Fixed 2026-10-20: reloading while script jobs ran was unsafe. `Nox.Jobs` counts its running jobs; reload and shutdown wait for them (2 s, else the reload is skipped with a log line) | `Jobs.cs`, `DotNetBackend.cpp` (WaitForJobs) |
| Scene | Fixed 2026-10-20: `TextComponent` font was neither saved nor loaded. Fonts (.ttf / .otf) are assets now; the component keeps the font's handle, saved with the scene | `SceneSerializer.cpp`, `Font.h` |
| Scene | Fixed 2026-10-20: `SerializeRuntime` / `DeserializeRuntime` were empty. They write / read a checked container (magic, version, size) around the scene's data; `Deserialize` recognises it, so a packaged scene can keep its name. Not yet a binary encoding of the components | `SceneSerializer.cpp` |
| Asset | Fixed 2026-10-20: `MeshImporter::LoadMesh` was a dead stub with no return value -- removed | `MeshImporter.cpp` |
| Editor | Fixed 2026-10-20: ortho camera constants were raw units (50 m / 100 m in a cm project) and the sky was flat in ortho. Now 2 km back / 4 km deep in meters, zoom at the cursor, and ortho views see the sky of a 60 degree camera looking the same way | `EditorCamera.cpp`, `Skybox.slang` |
| Editor | Fixed 2026-10-20: the Import Scale tooltip said the model was untouched; it now says the scale is baked into the mesh | `ContentBrowserPanel.cpp` |
| Editor | Fixed 2026-10-20: the project "dialog" was a hard-coded `E:/dev/...` path. Startup opens the most recent project (or the sample above the editor's folder), else asks Open / New / Quit until one opens; File > New Project makes the folder layout; recent projects in the user's settings folder | `EditorLayer.cpp` (OpenStartupProject) |
| Editor | ImGui multi-viewport: no cause found for the "broken" note (the Vulkan backend makes the secondary swapchains itself). Opt-in with `NOX_IMGUI_VIEWPORTS=1`, untested; a render target shown in another window is read without waiting for the next frame | `ImGuiLayer.cpp` |
| Physics | Fixed 2026-10-20: 3D physics components were not copied by Duplicate, and bodies / controllers added during Play were not created. Now duplicated; added or removed while physics runs, the body is made again after the update systems | `Scene.cpp` (`SyncPendingPhysics3D`) |
| Units | Fixed 2026-10-20: NRD distances and the DLSS far plane were raw world units. REBLUR's hit-distance scale, the denoising range (sky beyond it), the reflection miss distance and the DLSS far plane follow the world unit; NRD's view Z is 32-bit (half floats ended at 655 m in cm) | `DeviceVK.cpp`, `NRDFrontEnd.slang`, `NRDGuides.slang` |
| Particles | Fixed 2026-10-19 (this audit): lit particles read only the first 8 lights, and point / spot lights come first in the buffer -- with 8 lamps the sun did not light them | `Particles.slang` |

### 0.2 Built but not tested by the user (newest first)

- **2026-10-19:** Ray Tracing Resolution Full / Half (`RTUpsample.slang`; not saved in the project); unit-correct punctual lights
  (**cm projects: lights much brighter, lower old intensities**), range scaled with the entity, sun keeps the soft shadow mask;
  rect area lights; steady night GI (`LightSelection.slang`); one GI for transparency / ray hits (`SceneGI.slang`); thin glass not
  traced; rough reflections from GI; brute-force direct light in every preset. Confirmed: the decal veil fix.
- **Screen-probe GI** (the GI of every preset), DDGI cascades / camera-follow / classification (now its second bounce).
- Quality presets with Auto; Stable Overlays; H2 shadowed transparency; H3b screen-space and H3 traced refraction.
- Procedural sky (S1, S2, S4, sun disc), aerial perspective (S5), volumetric clouds (V4), height fog, environment rotation.
- Particles V1 / V2; script APIs for run-time meshes, jobs, mesh colliders, raycasts (`VoxelTerrain.cs`).
- Runtime player (`NOX_BUILD_RUNTIME`, off by default), `RuntimeAssetManager`, File > Package Game.
- Content browser B1-B3 (rendered thumbnails, disk cache, delete / references / move / unused), basic shapes, F3 first part,
  animation A2-A8 (notifies, layered, additive, montages, root motion, BlendSpace2D, clip preview, Timeline / Sequencer).
- Up to 4 viewports (M1).
- Older and still untested: units U3-U6 (ortho views, import units, physics unit boundary, cm switch), compute skinning
  stage 1, model-instance save / reopen, import I2-I4 and Import Into Level of skeletal models, outliner folders, character
  controller and state-machine transitions in Play, animation graph steps 4-6 workflows.

### 0.3 Missing / open, by area

**Rendering and lights**
- OIT for transparent surfaces and particles (H4); denoising of transparent signals (H5); volume absorption (KHR attenuation).
- LTC area lights (rect and disc lights use the exact diffuse + representative-point specular), emissive meshes as lights
  (RTXDI). (Disc lights: built 2026-10-20.) (Rect lights visible: built 2026-10-20 -- an emissive panel in the raster view, and to the path tracer's camera and
  mirror rays; RT reflections show them through their specular highlight only, as engines do, not as a panel, which would
  count them twice.)
- Screen-probe GI: importance sampling of probe rays, adaptive extra probes, screen-space trace before the ray trace.
- DDGI probe relocation; faster (half-res / checkerboard) ReSTIR GI; ReSTIR PT spatial pass; pairwise MIS / boiling filter for DI.
- Volumetric fog / light shafts (froxels); NVIDIA Reflex; NRD SH denoisers.
- Fallback rung "shadow rays for the N strongest lights" (every light in range traces per pixel, also on Low).
- Half-res reflections / roughness cut as preset rungs (Ray Tracing Resolution exists, no preset sets it); Auto preset checks
  VRAM only.
- Own TAA (DLSS DLAA only today; R1).

**Sky, VFX**
- (Night sky -- moon lighting the scene and the sky, its phases, stars, the sun through the air: built 2026-10-20, untested.)
  (Aerial perspective and height fog on forward surfaces and particles; the path tracer's sun / moon discs and fog; wind,
  time-sliced sky updates and the viewer's altitude from the camera: built 2026-10-20, untested.)
- Cloud temporal reprojection. (Cloud shadows on the scene: built 2026-10-20.)
- Particles: GPU simulation and sorting, collisions, mesh particles, color / size curves, module editor (V3), motion vectors
  for DLSS, particles in the extra viewports and the path tracer; OpenVDB volumes (V5).

**Editor**
- Prefab: P1 nested additions into the outer prefab, P4 re-parenting overrides, P5 overrides of deleted entities, P7 Unpack
  All, prefab / .gltf thumbnails, variant tint.
- Content browser: rename / move of textures, materials, meshes (E2); Reimport and Replace References; neutral thumbnail
  backdrop and streaming feedback from preview views.
- Viewports: lower resolution / realtime off (M3), show flags (M4), selection outline and gizmo in the extra views, camera not
  saved between sessions; ortho zoom at the cursor, wireframe mode, move-to-grid, snap to floor by ray, mesh bounds in the
  Content Browser.
- Thumbnail.cache written into the asset folder. (New-project flow: built 2026-10-20, no project templates.)

**Animation**
- Sync markers (A2); notifies only in graph mode; graph preview and loop ranges (A7); double-click a skeletal mesh to preview;
  Timeline curve editor, material tracks, auto-key, sub-sequences / camera cuts (A8); audio graph (A9).
- Variable node input pins; per-instance skeleton poses (the Skeleton asset's nodes are shared); sockets; clip / graph picker
  filtered by skeleton; jump / fall states (C1, needs clips); multi-skin models spawn as nodes (C3).

**Scripting**
- Debounced build + source watching; `ScriptBehaviour` component model; scripts and asset
  references as assets; richer field metadata; `OnFixedUpdate` / `OnLateUpdate`; camera smoothing; API version check and
  debugger; backend-independent API; Lua (deferred).
- Per-project game assembly (Facerun.dll is hard-coded). (`Nox.WorldUnits` for scripts: built 2026-10-20.)

**Runtime and packaging**
- Pack files (R4), precompiled shaders and pipeline cache (R5), a cook step for textures, self-contained .NET runtime,
  Release / NOX_EDITOR-free build (R7), fullscreen window, a binary registry, renderer settings beyond the preset.
- C++ voxel module and VoxelWorld API; dynamic mesh upload without a stall; vertex colors in `Nox.Mesh`; dynamic mesh colliders.
- No shipping configuration (stats and logging always compiled in).

**Engine architecture and performance** (Engine plan)
- Async compute queue; JobSystem priorities / budgets; overlapped IO; parallel animation; frame pipelining.
- Geometry / cluster page streaming (6b, paused), RT fallback LOD (reverted), discrete LOD for small meshes, world streaming
  (6c), virtual texturing, DirectStorage, IoStore-like containers.
- Hard memory budgets and eviction, out-of-memory handling, sparse binding, BLAS sub-allocation / eviction, RT instance culling.
- Lights in the GPU scene (rewritten every frame); materials reference textures by path; BC4 / BC5 / BC6H cooking; scene
  YAML read on the main thread; model-instance overrides do not keep added components / re-parenting.
- Profiling: frame-time / flame graph, RAM by category, sortable overlay tables, GPU-to-CPU frame matching.

**Import (glTF)**
- Vertex colors, morph targets, `EXT_mesh_gpu_instancing`, `KHR_node_visibility`, materials variants, extras, several scenes,
  physics extensions; level import of a skeletal model's clips.

**Other**
- Metal backend (throws); text: outline / background color (fonts load from memory since 2026-10-20); Box2D physics materials.

### 0.4 Docs that no longer match the code

- **Root docs** (`PROJECT_CONTEXT.md`, `architecture.md`, `GEMINI.md`): written for the visibility-buffer migration. They miss Jolt,
  .NET scripting, the render graph (~70 passes), GPU scene, GI, NoxRuntime; say descriptor buffers (the code uses
  `VK_EXT_descriptor_heap`), C++23 (the build is C++20), "13 debug modes" (23), PBR Neutral tonemapping (default Hable),
  `PathTracer.slang` (the pipeline is `PathTracer/*.slang`), and call the live forward shaders dead.
- `Engine_Architecture_Plan_2026.md` §3b is the pre-plan snapshot; D9 is decided and built; skinned BLAS, BLAS compaction and
  a second view are built.
- `RayTracing_Architecture_Plan_2026.md`: progress table says Phase 5 "next up" and 6-7 not started -- ReSTIR GI / DI / PT and
  RTAO are built; GI modes 0-3, debug views 0-22.
- `Prefab_*`, `Units_*`, `Runtime_And_Packaging_*`: headers still say "plan, nothing built".
- `Animation_Graph_*`: §5 marks steps 4-7 untested while §8 says built; prefab asset and outliner folders listed as open.
- `Scripting_Architecture_and_Status.md`: lists 6 C# files and 3 API groups (21 files and ~12 groups now).
- `Hybrid_Renderer_Plan_2026-10.md` §1 table (transparency, GI rows) and §2 ladder (High = ReSTIR DI / GI) are older than §2c / §8.
- `Backlog_2026.md`: P6 is closed by R2; R5 (DDGI fixed grid) replaced by the camera-following cascades.

## 1. The docs, one by one

| Doc | Last touched | What it covers | State it describes | Stale or wrong (checked against code) |
|---|---|---|---|---|
| `README.md`, `PROJECT_CONTEXT.md`, `GEMINI.md`, `architecture.md` | 2026-09-11 | Overview, NRI rules, visibility-buffer migration record | Phases 0-7 of the VisBuffer migration done | **Stale overview.** Says Box2D is "the" physics engine and Taskflow is vendored but unused. Today: Jolt (3D) next to Box2D, Taskflow drives the frame (`JobSystem`), C# scripting through Coral, render graph, GPU scene. The NRI zero-leak and Slang rules are still current. |
| `Engine_Architecture_Plan_2026.md` | 2026-09-25 | Threading, render graph, GPU scene, memory, async IO, streaming, profiling | Phases 0-5 and 6a built; 6b-1 (cluster DAG + GPU cut) built, 6b streaming paused, RT fallback LOD reverted; 6c (world streaming) and 7 not started | §3b "current state" is still the *pre-plan* snapshot (lists "single-threaded main loop", "no profiler", which phases 0-2 removed). **D9 "Editor vs runtime asset managers"** is still open, and the code shows why: `RuntimeAssetManager` is an empty class (see F7). |
| `RayTracing_Architecture_Plan_2026.md` | 2026-09-12 | Lighting, GI, denoising, DLSS/RR, path tracer | Phase 5 Track B (ReSTIR GI) "next up", phases 6-8 not started | **Behind the code.** ReSTIR GI, ReSTIR DI and ReSTIR PT passes and shaders exist (`ReSTIRPasses.cpp`, `RTXDI/DI`, `RTXDI/GI`, `RTXDI/PT`), RTAO exists (`RTAO.slang`, phase 7 part 1) and SSAO was added. **Not in code:** LTC area lights, volumetric froxels, Reflex. The progress table (§6) needs a refresh. |
| `Animation_Graph_Architecture_Plan_2026.md` | 2026-09-26 | Node-graph core, animation graph, character controller, UE5-style import | Steps 1-7 built (7b/7c untested in Play), step 8 (audio graph) future; import stages A/B done, C became the prefab plan | Node types in code: `Clip`, `Blend2D`, `GetParameter`, `StateMachine`, `Output`. **Missing for F3:** no 1D blend space, no layered blend / per-bone mask, no additive, no sync groups or notifies, no root motion, no sequencer/timeline, no way to edit or author clips. "Step 8 audio graph" is the only planned phase left. |
| `Prefab_Architecture_Plan_2026.md` | 2026-09-26 | Prefab asset, instances, overrides, nested prefabs, variants | P1-P6c built and tested | Open items section 8 (P1, P4-P7) match the backlog. |
| `Units_And_World_Tools_Plan_2026.md` | 2026-09-28 | cm world, grid, snap, ortho views, measure, physics unit boundary | U1-U6 built; U2 confirmed by the user; U3-U6 untested | Consistent with code (`WorldUnitsPerMeter` default 100). Side effects worth knowing: the ortho camera sits a fixed 5000 units back (not unit-scaled), which matters for anything that places geometry by NDC depth (see F9). |
| `Scripting_Architecture_and_Status.md` | 2026-09-22 | C#/.NET through Coral, lifecycle, hot reload, exposed fields | Runtime integrated; "Next" list of 9 items | Next-list item 6 is half done: `AddForce` / `AddImpulse` / `SetLinearVelocity` exist (`InternalCalls.cs`), `FixedUpdate` does not. No job-system or chunk/voxel API is exposed (relevant to F11). |
| `Backlog_2026.md` | 2026-09-28 | Index of everything open | N4 (units) in progress; P/C/I/R/E items | Kept. R4 (stats overlay) is answered by this audit (F9). |
| `Future.md` | 2026-10-18 | The user's new wish list (13 items) | -- | Mapped below. |

## 2. Features that exist but no doc tracks

- **Editor:** world grid (Godot port), snap toolbar, measure tool, reference figure, ortho views, fly camera, frame
  selected (F / double-click), outliner folders, physics collider wireframes, prefab mode.
- **Rendering:** SSAO + blur, RTAO, auto exposure, NRD front end, the RTXPT-based path tracer split into
  `PathTracer/` modules, ReSTIR DI/GI/PT, compute skinning and animated skinned BLAS for shadows (commit `e6670d0`;
  the Engine plan §9 still lists the animated BLAS as deferred).
- **Assets:** texture streaming with GPU mip feedback, cluster-LOD cook, async upload manager.

## 3. Things nobody has (and the Future.md items that ask for them)

| Missing today | Code evidence | Future.md |
|---|---|---|
| Undo / redo | no command history anywhere in `NoxEditor/src` | 12 |
| Primitive shapes | the only cube/sphere are hand-imported glTFs in `Facerun` | 10 |
| More than one viewport | one `Viewport` panel, one `EditorCamera`; the renderer has one `CullView` ("A second real view" in Engine plan §9 waits for exactly this) | 5 |
| Procedural sky | `Skybox.slang` samples the HDR environment cubemap only; no atmosphere model in code | 6 |
| Runtime player and runtime asset manager | `RuntimeAssetManager` is an empty class; only `NoxEditor` is an executable; `Scene.Instantiate` resolves prefabs through the editor registry (P6) | 7 |
| Packaging / distribution build | no pack format, no cook-all step, no runtime target | 8 |
| Material editor window, mesh/material thumbnails | `ThumbnailCache` shows textures and a material's base-color texture only; materials are edited in the Properties panel | 2 |
| Content browser folder tree and search | flat grid with a breadcrumb and `<` back button | 2 |
| Animation authoring (sequencer, clip editing) | see the animation row above | 3 |
| VFX / particles / volumetric clouds | nothing in code | 13 |

## 4a. Where every Future.md item stands now (updated 2026-10-18, end of the implementation round)

Everything below is built and pushed but **untested unless it says confirmed** -- the user builds and tests. Details in the
plan each row names.

| # | Item | State now | Open |
|---|---|---|---|
| F1 | Docs audit | done (this file, kept current here) | -- |
| F2 | Modern content browser | folder tree, search, filters, Material Editor window with a live sphere, rendered thumbnails (materials on spheres, meshes), thumbnail cache on disk (`Editor_Features_Plan_2026-10.md` §2) | prefab / model thumbnails, a neutral backdrop |
| F3 | Animation | Sequence Player node, BlendSpace1D / 2D, notifies, layered blend, additive, montages, root motion, clip preview window, Sequencer with Timeline assets and C# control (§3) | audio graph, clip trimming |
| F4 | Hybrid renderer | DLSS outline jitter fixed (**confirmed**), Stable Overlays, quality presets with Auto, RT shadows on transparency, screen-space and ray traced glass refraction, DDGI cascades, Lumen-style screen-probe GI as every preset's GI (`Hybrid_Renderer_Plan_2026-10.md` §2-4, §2c) | OIT (H4), transparent-signal denoising (H5), volume absorption, screen-probe importance sampling |
| F5 | Up to 4 viewports | movable / resizable, 1 / 2 / 3 / 4 layouts, independent cameras (`Editor_Features_Plan_2026-10.md` §4) | -- |
| F6 | Procedural sky | Hillaire atmosphere, sun disc, aerial perspective, volumetric clouds, height fog; HDR environment rotation applied (§5) | night sky |
| F7 | Runtime asset manager + runtime project | NoxRuntime player, `RuntimeAssetManager` (read-only pipeline), engine code off the editor manager (`Runtime_And_Packaging_Plan_2026-10.md` §2b-2c) | pack-file reads (R4) |
| F8 | Packaging | File > Package Game: runtime exe, engine assets, only the assets the scenes need, registry subset (§2d); PCH answered (compile speed only) | pack files (R4), precompiled shaders (R5), per-project script assembly name |
| F9 | Stats overlay text | fixed | -- |
| F10 | Basic shapes | built | -- |
| F11 | Minecraft-style game | answered (§4); script APIs built: run-time meshes, jobs, mesh colliders, raycasts, `VoxelTerrain.cs` example (§4b) | the C++ voxel module for big worlds |
| F12 | Undo / redo | built (Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y, **confirmed**) | -- |
| F13 | VFX, particles, clouds | clouds in the sky; Particle Emitter with CPU simulation, lit and soft rendering, flipbooks, C# control (`Hybrid_Renderer_Plan_2026-10.md` §6b) | GPU simulation, OIT, collisions, module editor (V3), VDB volumes (V5) |
| F14 | Unregister meshes of deleted entities? | answered (no, below); stale registry entries pruned at load | -- |

## 4. Future.md items: where each one stands

| # | Item | Where it stands | Size | What happens with it now |
|---|---|---|---|---|
| F1 | Docs audit | this file | -- | done |
| F2 | Modern content browser | flat grid, no tree, no search, texture-only thumbnails | M (UI) + L (rendered thumbnails) | **first part built this round**: folder tree, clickable path, search, type filter, tile size, Material Editor window; rendered sphere / mesh thumbnails planned in `Editor_Features_Plan_2026-10.md` §2 |
| F3 | Animation graph: player node, sequencer, editing | five node types, no authoring | L | **first part built this round**: Clip node as a Sequence Player (Speed pin, Start Position), BlendSpace1D node; notifies, layered blend, montages, root motion, preview window and a Sequencer planned in `Editor_Features_Plan_2026-10.md` §3 |
| F4 | Hybrid renderer near path-tracer quality, RT-only fallbacks, DLSS edge jitter, robust transparency/transmission | hybrid renderer works; DLSS edge jitter cause found (below) | XL | plan in `Hybrid_Renderer_Plan_2026-10.md`; root cause of the jitter written down there |
| F5 | Up to 4 viewports | single viewport and camera | L (renderer needs per-view resources) | plan in `Editor_Features_Plan_2026-10.md` |
| F6 | Procedural sky (Hosek-Wilkie / Hillaire) | HDR IBL only -> **Hillaire sky built** (2026-10-18, untested) | M | `Hybrid_Renderer_Plan_2026-10.md` §5b (with aerial perspective); open: night |
| F7 | Runtime asset manager + runtime project | empty class | L | plan in `Runtime_And_Packaging_Plan_2026-10.md` |
| F8 | How to package a distribution build | nothing | L | answered in `Runtime_And_Packaging_Plan_2026-10.md` |
| F9 | Stats overlay text broken | cause found | S | **fixed this round**: drawn in screen space (below) |
| F10 | Basic shapes | none | S | **built this round** |
| F11 | Minecraft clone: script or C++? | no voxel/job API exposed | -- | answered in `Runtime_And_Packaging_Plan_2026-10.md` §4 |
| F12 | Undo / redo | none | M | **built this round** |
| F13 | VFX, particles, clouds | none | L | answered and planned in `Hybrid_Renderer_Plan_2026-10.md` §6 |
| F14 | Unregister a mesh when its entity is deleted? (added 2026-10-18) | registry never shrinks; files deleted outside the editor leave entries behind | S | answered below; stale-entry clean-up **built** |

### F14 answer: deleting an entity should not unregister its mesh

The registry is the list of the project's **asset files**, not of what a scene uses -- like UE's Content Browser or
Unity's AssetDatabase. Unregistering a mesh because one entity using it was deleted would break:
- every other entity, scene and prefab that uses the same mesh (they reference it by handle),
- undo: Ctrl+Z brings the entity back with the same handle, which must still resolve,
- dragging the mesh into the scene again from the Content Browser.

The memory side is already handled: `EditorLayer::UnloadUnusedAssets` (mark and sweep) unloads meshes, materials and
textures no live scene references, and frees their GPU memory a few frames later. An unused asset costs one registry
line and its files on disk, nothing at run time.

What *is* pollution, and what to do about it:
1. **Entries whose files are gone** (deleted in the file explorer, leftovers of older imports): **built** -- when the
   registry loads, entries whose file and source file both no longer exist are removed (logged; the previous registry is
   kept as `.bak`; nothing is removed when more than half of the registry is missing, which means a moved asset folder).
2. **Deleting an asset on purpose**: Content Browser right-click > Delete, with a reference check (UE shows what uses the
   asset and offers Force Delete / Replace References): planned as B3 in `Editor_Features_Plan_2026-10.md`.
3. **Finding unused assets**: a report of registered assets no scene or prefab references (UE: the Reference Viewer /
   size map), so they can be deleted on purpose: planned with B3.

### F9 root cause (stats overlay)

The overlay was drawn as world-space quads on a plane placed at NDC depth 0.5 in front of the camera and then sent
through the camera's view-projection like any other 2D content. That ties its look to the camera:

1. **Ortho views (U3, 2026-09-27):** the ortho camera sits 5000 units back with near 0.1 / far 10000, so depth 0.5
   is ~5000 units away: the overlay plane lands at the focal point, *inside* the scene. Geometry in front of it
   hides parts of the panels and text, and the world grid (same depth range) runs through them.
2. **Float precision:** each glyph corner is a world position next to the camera (`camera + tiny offset`), built in
   float on the CPU and multiplied by `view` (which subtracts the camera position again) in the shader. The same
   cancellation the 2026-09-25 jitter fix removed from the sky shows up here: glyph corners snap to the float grid
   around the camera position, so text wobbles and distorts as the camera moves, worse the farther the camera is
   from the origin.
3. **Depth test against the scene:** anything closer than 2x near clip occluded the overlay.

Fix: `Renderer2D` has a screen-space batch (quads and text in output pixels, its own per-frame uniform buffer with
a pixel orthographic projection, depth 1.0 so the scene never occludes it) and the overlay draws into it. No camera
math is left in `StatsOverlay`.

### F4 finding (DLSS edge jitter)

`Entity/Depth Blit` (`UpscalePasses.cpp`) copies the *jittered* render-resolution entity-ID and depth buffers to
display resolution with a nearest blit. Everything that reads `EntityHi` / `DepthHi` therefore moves by the
sub-pixel camera jitter every frame: the selection outline (edge detection on `EntityHi`), picking, and the 2D
overlay's depth test (grid, gizmo lines, collider wireframes). With DLSS off there is no jitter, which is why it
only shows with DLSS on. The fix options are in `Hybrid_Renderer_Plan_2026-10.md` §3.

## 5. Recommended order after this round

1. Test what was built this round (all untested): F9, F10, F12, the content browser / Material Editor (F2) and the two
   animation nodes (F3). What to try is listed in `Editor_Features_Plan_2026-10.md` §1.
2. F4 DLSS outline jitter (small, self-contained, highest annoyance).
3. F7/F8 runtime player: it unblocks P6 (prefab lookup in exported games) and gives a clean place to measure
   performance without the editor.
4. F6 sky (self-contained renderer work).
5. F5 multi-viewport (needs the "second real view" renderer work from the Engine plan §9).
6. F3 animation authoring, then F13 VFX.
