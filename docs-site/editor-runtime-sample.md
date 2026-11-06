# NoxEditor, NoxRuntime, Facerun Sample, Scripts & Architecture Docs (`66` Files)

This page documents every file in **`NoxEditor/src/`** (31 files), **`NoxRuntime/src/`** (3 files), **`Facerun/`** (18 files), **`scripts/`** (2 files), and **`docs/`** (12 files).

---

## 1. `NoxEditor` — Authoring Environment & 11 Editor Panels (`NoxEditor/src/` — 31 Files)

### Plain-English Overview of Editor Capabilities
- **Multi-Viewport Layout (1 to 4 Independent Viewports)**: Switch between Single, 2-Vertical, 2-Horizontal, 3-Top/Bottom, and 4-Quad layouts. Each viewport has its own `EditorCamera` (Perspective or Ortho Top/Bottom/Front/Back/Left/Right), shading mode (`Lit`, `Wireframe`, `LitWireframe`, `Unlit`), and camera bookmark slots (`Ctrl+0..9` to save, `0..9` to recall).
- **Gizmos, Surface Snapping & Measurement Tool**:
  - Translate (`W`), Rotate (`E`), Scale (`R`), and Universal (`T`) ImGuizmo manipulators with World/Local space toggle and grid/angle/scale snapping.
  - **Surface Snap (Hold `V`)** & **End Key Snap-to-Floor**: Uses GPU raycasting (`EditorRaycast.slang`) against the scene TLAS to snap entities flush onto underlying geometry.
  - **3D Measurement Ruler (`M`)**: Click two points in the viewport to measure exact 3D distance in meters/centimeters with an in-world label.
- **Full Undo / Redo Stack (`EditorHistory`)**: Captures up to 100 entries (`EntitySnapshot`, `CreateDeleteAction`, `HierarchyAction`, `PropertyAction`) via `Ctrl+Z` / `Ctrl+Y` (`Ctrl+Shift+Z`).
- **Play-In-Editor (PIE), Physics Simulation & Prefab Isolation Mode**:
  - **Play** clones the active scene (`Scene::Copy`), starts C# scripts + 3D/2D physics, and supports **Pause**, **Step Frame**, **F8 Detach/Possess Editor Camera**, and live C# hot reload (`Ctrl+Alt+F11`).
  - **Simulate** runs 2D/3D physics while keeping full editor camera and gizmo control.
  - **Prefab Mode** opens any `.nprefab` in an isolated preview scene and saves changes back to the asset while refreshing all live instances in the level.
- **One-Click Game Packager (`GamePackager`)**: **File > Package Game...** traverses transitive asset dependencies (`CollectDependencies`), packs cooked assets + `.noxbin` binary scenes + precompiled `.spv` shaders into a single `Game.npak` archive, builds C# assemblies in Release mode, copies `NoxRuntime.exe` (renamed to `<ProjectName>.exe`) + runtime DLLs, and optionally bundles a private self-contained .NET 9 runtime (`dotnet/`).

### File-by-File Reference: `NoxEditor/src/` (31 Files)

| File | Plain-English Summary & Key Responsibilities |
| :--- | :--- |
| [`Main.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Main.cpp) | Entry point (`main`) creating the `NoxEditor` `Application`, loading the `.nproj` project (default `Facerun/Facerun.nproj`), and pushing `EditorLayer`. |
| [`EditorLayer.h`](file:///E:/dev/noxiouse/NoxEditor/src/EditorLayer.h) & [`EditorLayer.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/EditorLayer.cpp) | Master editor coordinator (`EditorLayer`). Manages dockspace, menus, Play/Simulate/Edit/Prefab modes, 4 viewports, gizmos, GPU picking, box marquee selection, surface snapping, measurement ruler, Renderer Settings drawer, and all 11 editor panels. |
| [`EditorHistory.h`](file:///E:/dev/noxiouse/NoxEditor/src/EditorHistory.h) & [`EditorHistory.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/EditorHistory.cpp) | Implements the 100-step Undo/Redo command history (`RecordModify`, `RecordCreate`, `RecordDelete`, `RecordHierarchy`, `Undo`, `Redo`). |
| [`ScriptBuilder.h`](file:///E:/dev/noxiouse/NoxEditor/src/ScriptBuilder.h) & [`ScriptBuilder.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/ScriptBuilder.cpp) | Watches `.cs` files via `NOXWatcher`, invokes `dotnet build --nologo` on a background thread, parses MSBuild diagnostics (`file(line,col): error CSxxxx`), and triggers `ScriptEngine::ReloadAssembly()` on success. |
| [`GamePackager.h`](file:///E:/dev/noxiouse/NoxEditor/src/GamePackager.h) & [`GamePackager.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/GamePackager.cpp) | Packages a standalone shipping build (`GamePackager::Package`): builds `Game.npak` (`NPAK` v1), serializes binary `.noxbin` scenes, bundles precompiled `.spv` shaders, copies `NoxRuntime.exe`, and optionally bundles self-contained `.NET 9`. |
| [`StreamingTestWorld.h`](file:///E:/dev/noxiouse/NoxEditor/src/StreamingTestWorld.h) & [`StreamingTestWorld.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/StreamingTestWorld.cpp) | **Tools > Generate Streaming Test World (1 km²)**: Procedurally generates a 1 km × 1 km city grid (676 blocks, ~6,500 buildings, street lights, parks, and a player character), partitions it into World Partition cells, and starts `HLODBuilder`. |
| [`Panels/SceneHierarchyPanel.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/SceneHierarchyPanel.h) & [`SceneHierarchyPanel.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/SceneHierarchyPanel.cpp) | **Scene Hierarchy** (search/filter, multi-select, drag-drop parenting, prefab creation/override badges) + **Properties Inspector** for all 47 ECS components (including C# `[Expose]` fields with unit conversion and Prefab property override Revert/Apply). |
| [`Panels/ContentBrowserPanel.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/ContentBrowserPanel.h) & [`ContentBrowserPanel.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/ContentBrowserPanel.cpp) | **Content Browser**: folder tree, type filter pills, fuzzy search, async thumbnail grid, drag-and-drop asset spawning, glTF/Texture import settings editor, Reference Viewer modal, Delete with reference replacement, and Unused Asset finder. |
| [`Panels/ThumbnailCache.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/ThumbnailCache.h) & [`ThumbnailCache.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/ThumbnailCache.cpp) | Renders and caches 128×128 PNG thumbnails in `<Project>/Cache/Thumbnails/<AssetUUID>.png` (ray-marching a lit 3D preview sphere for `.nmat` materials and rendering `.nsmesh`/`.nmesh` via `AssetPreviewRenderer`). |
| [`Panels/AssetPreviewRenderer.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/AssetPreviewRenderer.h) & [`AssetPreviewRenderer.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/AssetPreviewRenderer.cpp) | Manages dedicated `Scene` worlds and `ExtraViewSlot` render targets (`Thumbnail`, `MaterialPreview`, `AnimationPreview`) for live 3D asset previews. |
| [`Panels/MaterialEditorPanel.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/MaterialEditorPanel.h) & [`MaterialEditorPanel.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/MaterialEditorPanel.cpp) | **Material Editor**: interactive 3D preview (Sphere, Cube, Plane, Cylinder, or Custom Mesh) + full authoring of PBR, UV transforms, Transmission/Volume, and all OpenPBR/glTF extension lobes (Clearcoat, Sheen, Specular, Iridescence, Anisotropy, Dispersion). |
| [`Panels/NodeGraphEditorPanel.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/NodeGraphEditorPanel.h) & [`NodeGraphEditorPanel.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/NodeGraphEditorPanel.cpp) | **Visual Animation Graph Editor** (`.nanimgraph`): breadcrumb navigation into nested `StateMachine` subgraphs, interactive 1D/2D Blend Space grid widget, live parameter sliders, state transition rule inspector, and an embedded 3D skeletal preview viewport. |
| [`Panels/NodeGraph/INodeGraphCanvasBackend.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/NodeGraph/INodeGraphCanvasBackend.h) | Abstract interface decoupling `NodeGraphEditorPanel` from the underlying ImGui node-canvas library. |
| [`Panels/NodeGraph/ThedmdCanvasBackend.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/NodeGraph/ThedmdCanvasBackend.h) & [`ThedmdCanvasBackend.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/NodeGraph/ThedmdCanvasBackend.cpp) | Implements `INodeGraphCanvasBackend` over `thedmd/imgui-node-editor`, including custom state-machine boxes and directional transition arrows. |
| [`Panels/NodeGraph/NodeGraphPropertyEditor.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/NodeGraph/NodeGraphPropertyEditor.h) & [`NodeGraphPropertyEditor.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/NodeGraph/NodeGraphPropertyEditor.cpp) | Renders type-aware ImGui property widgets (sliders, asset pickers, bone pickers, parameter bindings) for `NodeGraphValue` properties. |
| [`Panels/AnimationClipPanel.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/AnimationClipPanel.h) & [`AnimationClipPanel.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/AnimationClipPanel.cpp) | **Animation Clip & Notify Editor**: 3D skeletal preview with bone/socket axes, timeline scrubber, `Sync:` footstep phase marker authoring, and `.nanim.notifies` saving. |
| [`Panels/TimelinePanel.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/TimelinePanel.h) & [`TimelinePanel.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/TimelinePanel.cpp) | **Sequencer Dope-Sheet & Curve Editor** (`.ntimeline`): multi-track keyframe authoring across all 11 track types, `SubTimeline` clip bars, keyframe interpolation modes, and live scene scrubbing. |
| [`Panels/RenderGraphPanel.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/RenderGraphPanel.h) & [`RenderGraphPanel.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/RenderGraphPanel.cpp) | **Render Graph Inspector**: Pass × Resource lifetime matrix, physical transient memory aliasing overlay (`phys #N` colors), **Live Texture & Mip Inspector** with cursor RGBA float probe (`TextureInspect.slang`), and GraphViz `.dot` export. |
| [`Panels/ProfilerPanel.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/ProfilerPanel.h) & [`ProfilerPanel.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/ProfilerPanel.cpp) | **Profiler Panel**: 240-frame interactive frame-time histogram (click any bar to freeze and inspect that frame), CPU Scopes table, GPU Passes table, Memory & Streaming budgets, Thread Gantt Timeline, and JSON / Chrome Trace export. |
| [`Panels/WorldPartitionPanel.h`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/WorldPartitionPanel.h) & [`WorldPartitionPanel.cpp`](file:///E:/dev/noxiouse/NoxEditor/src/Panels/WorldPartitionPanel.cpp) | **World Partition & HLOD Panel**: 1-click Split/Merge scene into `.noxcell` grid cells, Bake/Clear HLODs across levels `L0..LN`, and an interactive 2D top-down mini-map showing cell states (`Unloaded`, `Loading`, `Loaded`, `Activated`), HLOD visibility rings, and streaming sources. |

---

## 2. `NoxRuntime` — Standalone Shipping Game Player (`NoxRuntime/src/` — 3 Files)

| File | Plain-English Summary & Key Responsibilities |
| :--- | :--- |
| [`Main.cpp`](file:///E:/dev/noxiouse/NoxRuntime/src/Main.cpp) | Entry point (`main` + `WinMain` for `WIN32` subsystem builds). Parses `--project` and `--pak` arguments, auto-discovers `<ExeName>.npak` or `Game.npak` next to the executable, mounts it via `VirtualFileSystem::MountPak`, and starts `RuntimeLayer`. |
| [`RuntimeLayer.h`](file:///E:/dev/noxiouse/NoxRuntime/src/RuntimeLayer.h) & [`RuntimeLayer.cpp`](file:///E:/dev/noxiouse/NoxRuntime/src/RuntimeLayer.cpp) | Loads the `.nproj` project (from VFS or disk), initializes `RuntimeAssetManager` (or `EditorAssetManager`), applies saved `ProjectRendererSettings`, loads `StartScene` (preferring binary `.noxbin` inside `.npak`), calls `m_ActiveScene->OnRuntimeStart()`, and drives the game loop and window swapchain resize/fullscreen shortcuts (`F11` / `Alt+Enter`). |

---

## 3. `Facerun` — Sample Game Project (`Facerun/` — 18 Files)

| File | Plain-English Summary & Gameplay Role |
| :--- | :--- |
| [`Facerun.nproj`](file:///E:/dev/noxiouse/Facerun/Facerun.nproj) | Sample project file (`UnitsPerMeter: 100.0`, `StartScene: Scenes/SampleScene.nox`). |
| [`Assets/AssetRegistry.nxr`](file:///E:/dev/noxiouse/Facerun/Assets/AssetRegistry.nxr) | YAML asset registry mapping UUIDs to cooked assets in `Facerun/Assets/`. |
| [`Assets/Scripts/Facerun.csproj`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/Facerun.csproj) | .NET 9 C# project referencing `Nox.ScriptCore.csproj`. |
| [`Assets/Scripts/config/config-win.bat`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/config/config-win.bat) | Legacy Premake5 project generator helper script. |
| [`Assets/Scripts/CharacterMovement.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/CharacterMovement.cs) | Third-person character controller driving `CharacterControllerComponent` (`SetMoveVelocity`, `Jump`), smooth yaw rotation toward movement direction, and `AnimatorComponent` blend parameters (`Speed`, `Grounded`, `Jump` montage, and `OnAnimationEvent` footstep handling). |
| [`Assets/Scripts/CameraFollow.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/CameraFollow.cs) | Third-person orbit camera (`OnLateUpdate`) with mouse-look (`Input.MouseDelta`), scroll-wheel zoom, and exponential spring smoothing. |
| [`Assets/Scripts/WASDMovement.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/WASDMovement.cs) | Physics-based movement script driving `RigidBody3DComponent.LinearVelocity` and spacebar jump impulses, plus `MorphDemo` and `TimelineTrigger` keys. |
| [`Assets/Scripts/Shooter.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/Shooter.cs) | Spawns projectile prefabs (`Scene.Instantiate`) on LMB and fires 3D physics raycasts (`Physics.Raycast`) with `AddImpulse` on RMB. |
| [`Assets/Scripts/Projectile.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/Projectile.cs) | Applies launch velocity to a spawned projectile rigid body and self-destructs (`Scene.Destroy(Self)`) after `Lifetime` seconds. |
| [`Assets/Scripts/VoxelTerrain.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/VoxelTerrain.cs) | Multi-threaded procedural voxel terrain generator: builds 16×16×16 chunk meshes in parallel on background `.NET` threads (`Jobs.Run<MeshBuilder>`) and uploads them to `Entity.SetMesh` & `Entity.SetMeshCollider`, with LMB/RMB raycast block digging and building! |
| [`Assets/Scripts/FlyCamera.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/FlyCamera.cs) | Free-flying WASD/QE + RMB mouse-look camera for flying across large worlds. |
| [`Assets/Scripts/LevelStreamingTest.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/LevelStreamingTest.cs) & [`LevelStreamingTestBox.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/LevelStreamingTestBox.cs) | Demonstrates manual (`F6`) and volume-triggered additive level streaming (`Scene.LoadLevel` / `UnloadLevel`) with rotating indicator cubes. |
| [`Assets/Scripts/WorldPartitionTest.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/WorldPartitionTest.cs) & [`StreamingWorldTest.cs`](file:///E:/dev/noxiouse/Facerun/Assets/Scripts/StreamingWorldTest.cs) | Automated stress-test scripts for the 1 km² World Partition city: flies waypoints or teleports across the map (`F8`), queries `Scene.IsWorldStreamingCompleted`, and logs cell activation latency and RAM/VRAM stats (`F7` / `F9`). |
| [`Assets/Scenes/SampleScene.nox`](file:///E:/dev/noxiouse/Facerun/Assets/Scenes/SampleScene.nox), [`StreamingSubLevel.nox`](file:///E:/dev/noxiouse/Facerun/Assets/Scenes/StreamingSubLevel.nox), [`StreamingTestWorld.nox`](file:///E:/dev/noxiouse/Facerun/Assets/Scenes/StreamingTestWorld.nox), [`WorldPartitionDemo.nox`](file:///E:/dev/noxiouse/Facerun/Assets/Scenes/WorldPartitionDemo.nox) | Sample YAML scene files demonstrating PBR/RT lighting, additive sub-levels, and partitioned worlds. |

---

## 4. Build Scripts (`scripts/`) & Architecture Design Documents (`docs/`)

### `scripts/` (2 Files)
| File | Plain-English Purpose |
| :--- | :--- |
| [`scripts/compile_shaders.bat`](file:///E:/dev/noxiouse/scripts/compile_shaders.bat) | Batch helper that compiles `.slang` shaders offline using `slangc.exe`. |
| [`scripts/run_tracy.bat`](file:///E:/dev/noxiouse/scripts/run_tracy.bat) | Downloads/launches the **Tracy v0.14.1** GUI profiler (`tracy-profiler.exe`) to connect to `NoxEditor` when built with `-DNOX_ENABLE_TRACY=ON`. |

### `docs/` (12 Files)
| File | Plain-English Summary |
| :--- | :--- |
| [`docs/Engine_Architecture_Plan_2026.md`](file:///E:/dev/noxiouse/docs/Engine_Architecture_Plan_2026.md) | Master 2026 engine roadmap covering Profiling, Multithreading, Asset Streaming, World Partition, HLOD, Jolt Physics, Animation, C# Scripting, and Packaging. |
| [`docs/Status_Audit_2026-10.md`](file:///E:/dev/noxiouse/docs/Status_Audit_2026-10.md) | Comprehensive October 2026 audit verifying the completion status of every subsystem in `Engine_Architecture_Plan_2026.md`. |
| [`docs/Backlog_2026.md`](file:///E:/dev/noxiouse/docs/Backlog_2026.md) | Remaining optional polish items and future roadmap ideas. |
| [`docs/Animation_Graph_Architecture_Plan_2026.md`](file:///E:/dev/noxiouse/docs/Animation_Graph_Architecture_Plan_2026.md) | Architecture specification for the generic `NodeGraph` runtime and the 11-node `AnimationGraph` system. |
| [`docs/Jolt_Physics_Integration_Plan_2026.md`](file:///E:/dev/noxiouse/docs/Jolt_Physics_Integration_Plan_2026.md) | Design document for the double-precision Jolt 3D physics integration, compound colliders, 6-DOF joints, and `CharacterVirtual`. |
| [`docs/ReSTIR_Integration_Plan_2026.md`](file:///E:/dev/noxiouse/docs/ReSTIR_Integration_Plan_2026.md) | Design document for integrating NVIDIA RTXDI (`ReSTIR DI`, `ReGIR`, `ReSTIR GI`, and `ReSTIR PT` with Hybrid Shift Mapping). |
| [`docs/PathTracer_Architecture_Audit_2026-04.md`](file:///E:/dev/noxiouse/docs/PathTracer_Architecture_Audit_2026-04.md) | Audit and upgrade plan for the hardware Ray Tracing Path Tracer (`PathTracer/*`) and RTXPT BSDF integration. |
| [`docs/OpenPBR_Material_Plan_2026.md`](file:///E:/dev/noxiouse/docs/OpenPBR_Material_Plan_2026.md) | Specification for the glTF 2.0 / OpenPBR multi-lobe material model (Clearcoat, Sheen, Specular, Iridescence, Anisotropy, Volume, Dispersion). |
| [`docs/Virtual_Geometry_Streaming_Plan_2026.md`](file:///E:/dev/noxiouse/docs/Virtual_Geometry_Streaming_Plan_2026.md) | Design specification for the Nanite-style Cluster LOD DAG cooker, 128 KB page format, GPU feedback pass, and 64 KB Sparse Buffer residency streamer. |
| [`docs/Sky_Atmosphere_Cloud_Plan_2026.md`](file:///E:/dev/noxiouse/docs/Sky_Atmosphere_Cloud_Plan_2026.md) | Design specification for the Hillaire 2020 physical sky atmosphere, volumetric clouds, horizon band, cloud shadow map, and 3D aerial perspective froxels. |
| [`docs/World_Units_Convention_Plan_2026.md`](file:///E:/dev/noxiouse/docs/World_Units_Convention_Plan_2026.md) | Specification for the `1 unit = 1 cm` (`UnitsPerMeter = 100.0`) world-unit convention and physics meter boundary conversions. |
| [`docs/Perf_Audit_Scene_Rendering_2026-04.md`](file:///E:/dev/noxiouse/docs/Perf_Audit_Scene_Rendering_2026-04.md) | CPU/GPU scene synchronization and rendering performance audit. |

---

## Levels Panel, Hierarchy & Profiler (November 2026)

- **Window > Levels** (Unreal's Levels window): the persistent level and its sub-levels as rows -- the **Current** circle (where
  new entities go), **Visible**, **Locked**, **Always Loaded / Streamed** -- with **Add Existing Level**, **New Level** (a name; the
  file is made next to the open scene), **Remove** (asks to save first) and **Move Selection to Current**. Read-only in Play.
- **Scene Hierarchy:** a sub-level's root shows `[LevelName]`; a locked level's entities are dimmed and cannot be selected;
  right-click > **Move to Level**. **Shift-click** selects the rows between the two on screen (folders first, collapsed folders and
  rows left out), as Windows Explorer and Unreal's Outliner do.
- **Profiler:** Save / Load captures, the Spikes list and its threshold, the memory graph and the GPU Transfer track (see Core,
  Multithreading, Project & Profiling).
