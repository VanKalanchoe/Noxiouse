# Project Context: Noxiouse (NoxCore / NoxEditor / NoxRuntime)

> **Workspace:** `E:/dev/noxiouse`
> **Language standard:** C++20 (`CMAKE_CXX_STANDARD 20` in the root `CMakeLists.txt`), CMake 3.31+, C# on .NET 9 for game scripts
> **Graphics:** Vulkan 1.4 (Vulkan SDK 1.4.350+) through NRI, Slang shaders compiled to SPIR-V 1.6
> **Main libraries:** SDL3, EnTT, GLM, Dear ImGui + ImGuizmo + imgui-node-editor, Taskflow, Jolt Physics (3D), Box2D (2D),
> Coral (.NET hosting), meshoptimizer, VMA, NVIDIA Streamline (DLSS / DLSS-RR), NRD, RTXDI, Tracy, msdf-atlas-gen
> **Last checked against the code:** 2026-10-20

---

## 1. Overview

**Noxiouse** is a 3D game engine and editor written from scratch in C++20 on Vulkan 1.4. It has four parts:

| Target | Kind | What it is |
|---|---|---|
| `NoxCore` | static library | The engine: NRI (graphics layer), renderer and render graph, GPU scene, ECS scene, assets, physics, animation, scripting host, job system, profiling. |
| `NoxEditor` | executable | The editor (`EditorLayer`, panels, undo history, script hot reload, File > Package Game). |
| `NoxRuntime` | executable | The standalone player: the engine without the editor, the scene drawn straight to the window. Built with `-DNOX_BUILD_RUNTIME=ON`. |
| `NoxScriptCore` | C# project (`net9.0`) | `Nox.ScriptCore.dll`, the C# API game scripts use. |

The sample project is `Facerun/` (`Facerun.nproj`, C# scripts in `Facerun/Assets/Scripts`). The world unit is a project
setting, `WorldUnitsPerMeter` (`Project/Project.h`), default **100: one unit is one centimeter**, like Unreal
(`Core/WorldUnits.h`, `docs/Units_And_World_Tools_Plan_2026.md`).

Rendering is hybrid: a GPU-driven visibility-buffer rasterizer (task/mesh shaders, cluster LOD, GPU instance and Hi-Z
occlusion culling) feeds deferred lighting, with ray-traced shadows, reflections, ambient occlusion and several global
illumination methods on top, plus a reference path tracer. Every frame is built as a render graph.

---

## 2. Directory map

| Path | Content |
|---|---|
| `NoxCore/src/NRI/` | The engine's graphics interface: `Device`, `CommandBuffer`, `Buffer`, `Texture`, `Pipeline`, `DescriptorHeap`, `AccelerationStructure`, `ShaderBindingTable`, `QueryPool`, `TimelineSemaphore`, `GpuProfiler`, `Swapchain`, `SlangCompiler`. No Vulkan types. |
| `NoxCore/src/NRI/Vulkan/` | The only Vulkan backend (`DeviceVK`, `CommandBufferVK`, `DescriptorHeapVK`, `PipelineVK`, `AccelerationStructureVK`, `MemoryAllocatorVK`, ...). Streamline, NRD and VMA are used only here. |
| `NoxCore/src/NoxCore/RenderGraph/` | The render graph: `RenderGraph.h/.cpp` (passes, resources, histories, compile, parallel recording), `RGResourcePool` (transient memory, aliasing), `RGBlackboard`, `RGValidation`, `RGFrameReport`. |
| `NoxCore/src/NoxCore/Renderer/` | `Renderer.cpp/.h` (frame setup, pipelines, settings, readbacks), `Passes/*.cpp` (the passes by area: `RasterPasses`, `LightingPasses`, `RTPasses`, `ReSTIRPasses`, `PostPasses`, `UpscalePasses`, `EditorPasses`, `ExtraViewPasses`), `GpuScene` (persistent instance / transform / material / mesh tables), `GeometryArena`, `UploadManager`, `MemoryBudget`, `Renderer2D`, `Font`, cameras. |
| `NoxCore/src/NoxCore/Scene/` | EnTT scene (`Scene`, `Entity`, `Components.h`), hierarchy (`SceneGraph`), YAML scenes (`SceneSerializer`), prefabs (`Prefab.h`, `PrefabInstance`), model instances, particles, `EntityCommandBuffer`. |
| `NoxCore/src/NoxCore/Asset/` | Asset managers (`EditorAssetManager`, `RuntimeAssetManager` = the same pipeline read-only), importers for glTF / textures / materials / skeletons / clips / prefabs / scenes / timelines / node graphs, `TextureStreamer`. |
| `NoxCore/src/NoxCore/Physics/` | `Physics3DScene` interface with the Jolt backend in `Physics/Jolt/` (bodies, character controller, `NoxJoltJobSystem` on the engine's workers); `Physics2D.h` helpers for Box2D (the 2D world is stepped in `Scene.cpp`). |
| `NoxCore/src/NoxCore/Animation/` | Skeletons, poses, `Animator`, the animation graph runtime (`AnimationGraphInstance`, `AnimationGraphNodes`), timelines. |
| `NoxCore/src/NoxCore/NodeGraph/` | The generic node-graph core (types, compiler, evaluation) the animation graph is built on. |
| `NoxCore/src/NoxCore/Scripting/` | `ScriptEngine` over `IScriptBackend`; `DotNet/DotNetBackend` hosts .NET through Coral. |
| `NoxCore/src/NoxCore/Tasks/` | `JobSystem` (Taskflow executors), `SystemGraph` (ECS systems with declared reads / writes), `FrameArena`, `WorkerLocal`, `TaskObserver`. |
| `NoxCore/src/NoxCore/Core/` | `Application` (SDL3 app callbacks, the frame loop), `Window`, `Input`, `Log`, `UUID`, `WorldUnits`, `VirtualFileSystem` (loose files and `.npak` packs). |
| `NoxCore/src/NoxCore/Profiling/` | `Profiler`, Nox Stats overlay (F3), stats reports, optional Tracy backend. |
| `NoxCore/src/NoxCore/Project/` | `.nproj` projects and the project's renderer settings. |
| `NoxCore/vendors/` | Git submodules (taskflow, imgui, ImGuizmo, tinygltf, meshoptimizer, msdf-atlas-gen, JoltPhysics, tracy, OffsetAllocator, bc7enc_rdo, VulkanMemoryAllocator-Hpp) and folders the setup script downloads (slang, Streamline, NRD, RTXDI, filewatch, vcpkg). |
| `NoxEditor/src/` | `EditorLayer`, `EditorHistory` (undo), `ScriptBuilder` (C# build + hot reload), `GamePackager` (File > Package Game), `Panels/` (scene hierarchy, content browser, material editor, node-graph editor, animation clip, timeline, render graph panel, asset previews, thumbnails). |
| `NoxEditor/assets/shaders/` | All Slang shaders and `shaderIO.h` (the structs shared by C++ and Slang). Subfolders: `PathTracer/` (the ray tracing pipeline path tracer), `RTXDI/{DI,GI,PT,Presampling}/` (ReSTIR passes). |
| `NoxRuntime/src/` | `Main.cpp`, `RuntimeLayer` (load the project, play its start scene). |
| `NoxScriptCore/Source/` | The C# API (`Entity`, components, `Input`, `Physics`, `Scene`, `Jobs`, `Mesh`, `WorldUnits`, ...). |
| `Facerun/` | Sample project (`Facerun.nproj`, `Assets/`). |
| `scripts/` | Windows setup: `install_windows_dependencies.bat`, `setup_windows_build.bat`, Tracy helpers. |
| `docs/` | Plans and status documents (see `README.md`). |

---

## 3. Frame flow

One frame, from `Application::Run` (`Core/Application.cpp`, driven by SDL3's `SDL_AppIterate`):

1. **Layers update.** `EditorLayer` or `RuntimeLayer` runs the active scene: `Scene::OnUpdateRuntime` (Play / player),
   `OnUpdateEditor` or `OnUpdateSimulation`.
2. **Scripts (main thread).** C# `OnFixedUpdate` at 60 Hz (at most 5 steps per frame), then `OnUpdate`
   (`ScriptEngine` -> `DotNetBackend` -> Coral).
3. **Update systems (job system).** `SystemGraph` runs Physics 2D (Box2D), Physics 3D (Jolt), Animation, Timelines,
   Transform Propagation and Particles on the `JobSystem` workers. Order comes from each system's declared
   `ComponentAccess`; systems that do not conflict run in parallel. Structural ECS changes are recorded into
   `EntityCommandBuffer`s and applied at the sync point after the graph.
4. **Late scripts.** Animation notifies and timeline events go to scripts, then `OnLateUpdate`; transforms are propagated
   again if a late update moved something.
5. **GPU scene sync.** `Scene::SyncGpuScene` hands changed mesh instances, transforms, bones and materials to the
   renderer's `GpuScene` (meshes are not submitted every frame).
6. **Submit.** `BeginScene` -> submit systems in parallel (lights, 2D, particles) -> `EndScene`.
7. **ImGui** (editor only): the panels are built.
8. **`Renderer::drawFrame`.** Waits for the frame slot and acquires the swapchain image, runs deferred releases and the
   upload manager, reads back last time's results (picking, raycasts, cull stats, BLAS compacted sizes, mip feedback),
   then `recordFrame`:
   - **Build:** the `add*Pass` functions add this frame's passes in a fixed order (`Renderer.cpp`, `recordFrame`):
     procedural sky, cloud shadows -> GPU scene update, compute skinning, animated BLAS update, BLAS build (and
     compaction), TLAS build -> instance culling, visibility, Hi-Z build, late culling, late visibility -> G-buffer ->
     SSAO / RTAO -> NRD guides, RT shadows, DDGI, screen-probe GI, RT reflections, light presampling, ReSTIR GI, ReSTIR DI,
     ReSTIR PT -> previous-frame copy, path tracer -> deferred lighting -> forward 3D, particles / OIT -> DLSS or TAA ->
     post process (exposure, tonemapping) -> 2D overlay, selection outline -> editor extra views -> present (with ImGui in the
     editor) -> readback passes. A feature that is off adds no passes.
   - **Compile:** the graph drops passes nobody needs, allocates and aliases transient textures and buffers, and derives
     synchronization2 barriers per resource (`RGSynchronization::Precise`, the default).
   - **Execute:** passes record into command buffers, in parallel on the job system workers by default.
   - Submit and present.
9. **Frame sync point:** `JobSystem::ResetFrameArenas`.

There are 89 `AddPass` call sites in `Renderer/Renderer.cpp` and `Renderer/Passes/*.cpp` (about 80 distinct pass names,
counting the per-view and per-mip names).

---

## 4. Rendering features (current)

- **Geometry:** meshlets through task + mesh shaders, cluster LOD DAG (meshoptimizer `clusterlod`) with the cut chosen on the
  GPU, GPU instance culling per view with indirect-count draws, two-phase Hi-Z occlusion culling, compute skinning.
- **Raster path:** visibility buffer (`VisibilityBuffer.slang`) -> G-buffer material resolve (`GBufferMaterial.slang`) ->
  deferred lighting (`DeferredLighting.slang`, `PBRLighting.slang`). Forward passes draw unlit and transparent surfaces,
  the sky and particles; weighted blended OIT and refraction are options.
- **Lights:** directional, point, spot, rect and disc lights; direct lighting is a plain loop or ReSTIR DI (RTXDI).
- **Ray tracing:** BLAS per unique mesh (compacted after build), per-instance BLAS refit for skinned meshes, TLAS per frame.
  RT shadows (`ShadowMask.slang`) with NRD SIGMA, RT reflections (`Reflection.slang`) with NRD REBLUR / RELAX, RTAO
  (`RTAO.slang`) or SSAO.
- **Diffuse GI** (`Renderer::setDiffuseGIMode`): 0 off (IBL), 1 DDGI probes, 2 ReSTIR GI, 3 screen-space probes.
- **Path tracer:** a ray tracing pipeline in `NoxEditor/assets/shaders/PathTracer/` (`PathTracerRayGen`, `PathTracerHit`,
  `PathTracerAnyHit`, `PathTracerMiss`, RTXPT-style BSDF), optional ReSTIR PT and RTXDI light sampling, progressive
  accumulation, NRD or DLSS-RR denoising.
- **Sky:** cubemap environment with IBL, or a procedural atmosphere (`SkyAtmosphere.slang`) with cloud shadows, aerial
  perspective and height fog.
- **Upscaling / AA:** DLSS and DLSS Ray Reconstruction through Streamline, or TAA.
- **Post:** auto exposure and 10 tonemappers (`Tonemapping.slang`); the default is **Hable UC2** (`m_tonemapMode = 8`).
- **Debug views:** 23 (0-22) in the editor's "PBR Debug View" combo (`EditorLayer.cpp`): material channels, lighting
  terms, world position, entity IDs, depth, RT shadow mask, RT reflections, motion vectors, indirect GI, DDGI probes, path
  tracer raw / progressive, texture streaming mips, cluster LOD level, ambient occlusion.
- **Quality presets:** Low / Medium / High / Custom, plus per-project renderer settings (`ProjectRendererSettings.h`).
- **Editor extra views:** up to 7 extra raster views (4 viewports, thumbnails, material and animation previews,
  `Passes/ExtraViewPasses.cpp`).
- **Streaming:** texture mip streaming with GPU feedback (`TextureStreamer`), memory budgets (`MemoryBudget`), background
  asset loads and a transfer-queue upload manager.

---

## 5. Engine systems (current)

- **Job system:** `Tasks/JobSystem` owns a Taskflow executor for frame work and one for IO. The scene's update and submit
  graphs, asset loading, texture streaming, Jolt's jobs and render graph recording all run on it.
- **Physics:** Jolt for 3D (rigid bodies, colliders, `CharacterController3DComponent` on `JPH::CharacterVirtual`, raycasts),
  Box2D for 2D. Jolt works in meters; `Physics3DScene` converts at the boundary from the project's world units.
- **Scripting:** C# through Coral on .NET 9 (`Scripting/DotNet`). The editor builds a project's scripts and hot-reloads
  them (`ScriptBuilder`). Status and API: `docs/Scripting_Architecture_and_Status.md`.
- **Animation:** skeletal clips, animation graphs (`.nanimgraph`: blend spaces, layered blend, additive, state machines,
  slots) on the generic node-graph core, timelines (`.ntimeline`), animation notifies.
- **Prefabs:** `.nprefab` assets with instances, overrides, nested prefabs and variants (`docs/Prefab_Architecture_Plan_2026.md`).
- **Assets:** cooked files next to their sources (`.nmesh` skeletal mesh, `.nsmesh` static mesh, `.ntex`, `.nmat`, `.nanim`,
  `.nskel`, `.nprefab`, `.nanimgraph`, `.ntimeline`, scenes `.nox`), a YAML registry (`AssetRegistry.nxr`), hot reload through a
  file watcher.
- **Runtime and packaging:** `NoxRuntime` plays a project's start scene. File > Package Game (`NoxEditor/src/GamePackager.cpp`)
  writes a folder with the renamed runtime, engine assets, precompiled shaders, the game's script assembly and
  `Game/Content.npak`; the game reads assets through `Core/VirtualFileSystem` (a loose file wins over a pack entry).
  `-DNOX_DIST=ON` builds the shipping runtime. Details: `docs/Runtime_And_Packaging_Plan_2026-10.md`.
- **Profiling:** Nox Stats overlay (F3, CPU / GPU scopes, memory), stats reports, render graph panel, optional Tracy
  (`-DNOX_ENABLE_TRACY=ON`).

---

## 6. Design rules (architectural contracts)

1. **NRI zero-leak boundary.** Never include `<vulkan/vulkan.h>`, `<vulkan/vulkan.hpp>` or `vk_mem_alloc.h` outside
   `NoxCore/src/NRI/Vulkan/`. The rest of the engine talks only to `NRI::Device`, `NRI::CommandBuffer`, and so on.
   Third-party SDKs (Streamline, NRD, RTXDI, Taskflow, Tracy) stay behind the module that owns them.
2. **Bindless through descriptor heaps.** Resources are registered once in the global resource and sampler heaps of
   `VK_EXT_descriptor_heap` and passed to shaders as heap slots; buffers are passed as device addresses (buffer device
   address) in push constants. No descriptor sets, layouts or pools in engine code. (Inside `DeviceVK` only: the Dear ImGui
   Vulkan backend's own descriptor pool, and a dummy descriptor set bound as a workaround for Streamline.)
3. **No image layouts.** `VK_KHR_unified_image_layouts`: every image stays in `General`; only synchronization2 memory barriers
   are recorded, and the render graph derives them.
4. **Slang integration.** Shaders are Slang, compiled to SPIR-V by `NRI/SlangCompiler.cpp` (scalar layout, SPIR-V 1.6),
   cached on disk and hot-reloaded. GPU structs shared with C++ go in `NoxEditor/assets/shaders/shaderIO.h` and must have the
   same layout on both sides.
5. **Reverse-Z, infinite far plane.** Near is depth 1.0, infinity is 0.0; depth tests use `GREATER` / `GREATER_OR_EQUAL`.
6. **Render graph passes.** New GPU work is a render graph pass: declare every resource access in the setup callback; the
   execute callback only records (it may run on any worker thread).
7. **ECS data driven.** Scene state lives in EnTT (`Scene.cpp`, `Components.h`). Systems declare reads and writes; they do not
   create or destroy entities or components directly but record into an `EntityCommandBuffer`.

---

## 7. History: the visibility-buffer migration (completed)

The renderer moved from forward rendering to a visibility buffer + G-buffer deferred design in steps 0-7, all completed:
`VK_KHR_unified_image_layouts` (no layout transitions), visibility raster target, vertex pulling and barycentrics,
G-buffer material resolve, deferred PBR lighting with IBL, forward unlit / skybox pass, HDR -> LDR post-processing, and the
2D overlay with outlines and mouse picking. Since then the frame has grown well past that record (render graph, GPU scene,
GPU culling, ray tracing, GI, upscaling); section 3 above is the current frame. Two values from the migration record no
longer hold: the debug views are now 23 (it listed 13), and the default tonemapper is Hable UC2 (it was Khronos PBR
Neutral, which is still mode 4).
