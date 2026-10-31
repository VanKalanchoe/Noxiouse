# NoxEngine: Scalable Engine Architecture Plan (2026)

> **Document Version:** 0.1 — Draft for refinement (September 2026)
> **Scope:** Threading & job system, render graph, GPU-driven rendering, GPU memory & residency, asset
> pipeline, async loading, texture/geometry/world streaming, ray tracing at scale, profiling.
> **Target API:** Vulkan 1.4 through NRI — full extension/feature/shader baseline in §3 (Renderer Technology Baseline)
> **Companion documents:**
> - `docs/RayTracing_Architecture_Plan_2026.md` — lighting, GI, denoising, RTXDI/NRD/DLSS (unchanged by this plan)
> - `architecture.md` — the NRI, render graph and job system as built (rewritten 2026-10-20; its §7 summarizes the
>   earlier v2/v3 proposals). Where an older proposal differs, this document wins (see §5.4.9).
> **Status 2026-10-20:** phases 0-5 and 6a built; 6b-1 (cluster DAG + GPU cut) built, 6b page streaming paused, the RT
> fallback LOD reverted; 6c and 7 not started. **2026-10-24:** 6c-0 level streaming and 6c-1 world partition (grid cells, streaming,
> editor) built (§5.12a, §5.13.1); async compute's first user (§5.4.4). D9 decided and built (§7). From §9: skinned BLAS, BLAS compaction and extra
> views are built. §3b is the snapshot from before this plan.
> **Industry references:** Unreal Engine 5 (Tasks System, RDG, GPU Scene, Nanite, Virtual Textures,
> Texture Streaming, World Partition, IoStore/Zen loader), Frostbite (FrameGraph), id Tech (job system).

---

## Table of Contents

1. [Goals & Non-Goals](#1-goals--non-goals)
2. [Collaboration Contract](#2-collaboration-contract)
3. [Renderer Technology Baseline](#3-renderer-technology-baseline)
3b. [Current State Snapshot](#3b-current-state-snapshot-september-2026)
4. [Target Architecture Overview](#4-target-architecture-overview)
5. [Subsystems](#5-subsystems)
   - 5.1 Profiling & Instrumentation
   - 5.2 Threading Model & Task System (Taskflow)
   - 5.3 Frame Pipeline (Main Thread + Frame Graph)
   - 5.4 Render Graph
   - 5.5 GPU Scene
   - 5.6 GPU-Driven Culling & Draw Generation
   - 5.7 Geometry Pipeline & LOD
   - 5.8 GPU Memory & Buffer System
   - 5.9 Residency & Streaming Manager
   - 5.10 Asset Pipeline & Cooking
   - 5.11 Async IO & Loading
   - 5.12 Texture Streaming
   - 5.13 World Streaming
   - 5.14 Ray Tracing at Scale
   - 5.15 Asset Lifetime (Garbage Collection)
   - 5.16 Performance Practices
6. [Roadmap](#6-roadmap)
7. [Open Decisions](#7-open-decisions)
8. [References](#8-references)
9. [Deferred Work](#9-deferred-work)

---

## 1. Goals & Non-Goals

### Goals
- **Hybrid tracer that stays fast on large scenes.** Raster visibility buffer + RT lighting (see RT plan)
  must scale from sample assets to Bistro-class scenes and beyond without frame-time cliffs.
- **Worlds larger than VRAM.** Geometry, textures, ray tracing data and world content stream in and out
  under explicit memory budgets.
- **The editor never freezes.** Importing, loading, unloading and hot-reloading happen asynchronously;
  the viewport keeps rendering with placeholders while content arrives.
- **Every core system uses all CPU cores.** One engine-wide job system (Taskflow) schedules gameplay,
  rendering preparation, IO, decoding and GPU uploads.
- **Measured, not guessed.** Every phase ships with profiling instrumentation and numeric exit criteria.

### Non-Goals (for this plan)
- Multiple graphics APIs. NRI keeps the abstraction boundary, but only the Vulkan backend is in scope.
- Networking, audio, scripting runtime, physics beyond the existing Box2D integration. (Since then, outside this plan:
  Jolt 3D physics and C# scripting through Coral; see `PROJECT_CONTEXT.md`.)
- Replacing the lighting/GI architecture — this plan provides the scene, memory and scheduling
  foundations that architecture runs on.

---

## 2. Collaboration Contract

Inherits §1 of `docs/RayTracing_Architecture_Plan_2026.md` (execution policy, class layout, NRI zero-leak
boundary, infinite reverse-Z) plus:

1. **The user builds and runs.** Changes are verified by code inspection, standalone shader compiles
   (`slangc`) and scratch-pad experiments; engine builds and runtime testing are done by the user.
2. **Investigate before fixing.** Root causes are established from code, data or measurements before a
   fix is proposed. Timing claims are backed by logs or benchmarks.
3. **Phase gates.** A phase starts only when the previous phase's exit criteria (§6) are met and confirmed.
4. **Planning before architecture changes.** Phases touching threading, the render graph or memory
   layout are designed in plan mode before code is written.
5. **Prototype freedom — no compatibility code.** The engine is in prototyping/development, not production:
   no backwards-compatibility shims, format versioning, automatic re-cook/migration or legacy code paths for
   cooked data, the asset registry or scene files. When a format changes, the user deletes cooked files /
   the registry and re-imports manually.
6. **Baseline first.** Every design in this plan assumes the technology baseline in §3. Adding, removing
   or making optional any extension/feature updates §3 in the same change.
7. **Class layout.** `public:` functions first, `private:` functions in the middle, a separate
   `private:` section at the bottom for all member variables. Every edit keeps this order.
8. **Nothing leaks.** Vulkan types and headers (including `vk_mem_alloc.h`) stay inside
   `NoxCore/src/NRI/Vulkan`; engine code sees only NRI types. Third-party SDKs (RTXDI, NRD, Streamline,
   Taskflow, Tracy) stay behind their owning module — forward-declare in headers, include in `.cpp`.
9. **Performance is measured in `Release`.** Timings, frame times and load times count only from `Release`
   (`/O2 /Ob2`, validation layers off via `NDEBUG`), no debugger attached, VSync off, same scene + camera,
   shader cache warm, several runs (first cold load reported separately). `RelWithDebInfo` (`/O2 /Ob1`:
   less inlining, plus debug info) is for Tracy captures and debugging optimized code, not for the recorded
   numbers. Debug builds are for correctness and validation only. Every recorded measurement states its build
   configuration.

---

## 3. Renderer Technology Baseline

This is what the renderer is built on. It removes whole categories of engine code that older Vulkan
renderers carry (descriptor set management, image layout tracking, per-state pipeline permutations,
vertex buffer bindings), and every subsystem in §5 is designed around it.
Source of truth: `NoxCore/src/NRI/Vulkan/DeviceVK.cpp` (`requiredDeviceExtension`, feature chain) and
`NoxCore/src/NRI/SlangCompiler.cpp`.

### 3.1 API version & device selection
- Instance and device target **Vulkan 1.4** (`vk::ApiVersion14`); ImGui backend initialized with 1.4.
- Device selection requires a 1.3+ device with graphics, all required extensions below, and the
  required features (dynamic rendering, synchronization2, extended dynamic state, mesh shaders,
  acceleration structures, ray query, ray tracing pipeline, compute derivative group quads).
- Memory: **Vulkan Memory Allocator** (VMA, dynamic Vulkan functions) behind NRI.
- **Queues: one combined graphics + compute + present queue** (single queue family, one queue). Async
  compute and dedicated transfer queues are not used yet (Open Decision D6).
  **Status 2026-10-20:** a transfer queue for uploads exists since Phase 5a (D6, §5.11.6): a dedicated queue family
  when the GPU has one, else the graphics queue (`DeviceVK`). Async compute is still not used.

### 3.2 Device extensions (all required unless noted)
| Extension | What we use it for | What it simplifies / architectural consequence |
|---|---|---|
| `VK_KHR_unified_image_layouts` | Every image stays in `General` for its whole lifetime | **No image layout tracking or layout transitions anywhere.** The render graph tracks access, not layouts (§5.4.3). Copies/blits/storage/sampling all use `General`. |
| `VK_EXT_descriptor_heap` (+ `VK_KHR_maintenance5`, `VK_KHR_shader_untyped_pointers`, `VK_KHR_shader_non_semantic_info`) | Global bindless resource and sampler heaps; shaders index textures/samplers by heap slot (`Texture2D.Handle(uint2(imageHeapIndexOffset + index, 0))`) | **No descriptor sets, layouts, pools or per-pass binding code.** A resource is "bound" by registering it once and passing its slot. Transient render-graph resources need stable slots (§5.4.2); freed slots must not be reused while referenced (deferred release, texture-slot cache eviction). |
| `VK_KHR_buffer_device_address` (core 1.2 feature enabled) | Buffers referenced by 64-bit GPU address; Slang reads them as typed pointers (`InstanceData*`, `UniformBufferObject*`) passed via push constants | **No storage-buffer bindings.** GPU scene and geometry are plain pointers + offsets; unified geometry buffers need only one address per stream (§5.8.2). |
| `VK_EXT_shader_object` (+ `VK_EXT_extended_dynamic_state3`, `VK_EXT_vertex_input_dynamic_state`) — **optional** | Shaders bound as independent stage objects when supported; otherwise classic pipelines are built from the same shader descriptions | **No pipeline permutation explosion.** Blend, cull, depth, polygon mode, write masks, etc. are dynamic state set per draw. Pipeline caching/precompilation (§5.16) matters only for the fallback path. |
| `VK_EXT_mesh_shader` (task + mesh) | Meshlet rendering: task-shader culling, mesh-shader emission; vertex pulling through BDA | **No vertex/index buffer bindings or vertex input layouts** for scene geometry. GPU-driven culling (§5.6) extends the existing task-shader stage. |
| `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations` | BLAS per mesh, TLAS per frame; inline ray queries from fragment/compute passes (shadows, reflections, DDGI, RTXDI, path tracer) | Ray tracing without shader binding tables for most passes; BLAS/TLAS lifetime is ordinary resource management (§5.14). |
| `VK_KHR_ray_tracing_pipeline` | Enabled/required (available for SBT-based tracing) | Current passes use ray queries; keep available for future pipeline-based tracing. |
| `VK_KHR_compute_shader_derivatives` | Implicit derivatives in compute (group quads/linear) | Required by NRD REBLUR/RELAX. |
| `VK_EXT_memory_budget` — **optional** | Real per-heap usage/budget from the OS; VMA created with `eExtMemoryBudget`, budget refreshed through `vmaSetCurrentFrameIndex` in `Device::beginFrame` | Nox Stats VRAM (§5.1.4) and memory budgets (§5.8.3). Without it VMA estimates usage/budget itself (`Device::isMemoryBudgetSupported()`). |
| Timestamp queries (core) | `NRI::GpuProfiler`: one `eTimestamp` query pool per frame-in-flight slot, `vkCmdWriteTimestamp2`, `vkCmdResetQueryPool` at frame begin, results read without waiting when the slot is recorded again | GPU timings (§5.1.3); disabled when the queue's `timestampValidBits == 0`. No host query reset feature needed. |
| `VK_KHR_swapchain` | Presentation | — |
| `VK_KHR_push_descriptor`, `VK_NVX_binary_import`, `VK_NVX_image_view_handle` | Required by NVIDIA Streamline / NGX (DLSS, DLSS-RR) | Vendor requirement; not used by engine rendering code. |

Instance extensions: `VK_EXT_debug_utils` (debug builds), `VK_KHR_get_physical_device_properties2`,
`VK_KHR_external_memory_capabilities`, `VK_KHR_external_semaphore_capabilities` (Streamline).

### 3.3 Core features enabled
| Version | Features |
|---|---|
| 1.0 | `multiDrawIndirect`, `samplerAnisotropy`, `shaderInt16` (RTXDI PT), `shaderInt64`, `sampleRateShading`, `geometryShader`, `tessellationShader`, `wideLines` |
| 1.1 | `shaderDrawParameters` |
| 1.2 | `bufferDeviceAddress`, `timelineSemaphore` (Streamline), `scalarBlockLayout`, `storageBuffer8BitAccess`, `shaderInt8`, `shaderFloat16` (RTXDI PT), descriptor indexing (non-uniform indexing, update-after-bind, partially bound, variable count, runtime arrays) |
| 1.3 | `synchronization2`, `dynamicRendering`, `shaderDemoteToHelperInvocation`, `privateData` (Streamline) |
| Extension features | `shaderObject` (if supported), `extendedDynamicState` + `extendedDynamicState3` (depth clamp, polygon mode, samples, sample mask, alpha-to-coverage/one, logic op, color blend enable/equation, write mask), `vertexInputDynamicState`, `descriptorHeap`, `shaderUntypedPointers`, `maintenance5`, `taskShader` + `meshShader`, `unifiedImageLayouts`, `accelerationStructure`, `rayQuery`, `rayTracingPipeline`, `computeDerivativeGroupQuads/Linear` |

Consequences:
- **synchronization2 memory barriers** are the only barriers the engine records (acceleration structure
  build/read, transfer → build). This is what the render graph derives (§5.4.3).
- **dynamicRendering** — no render pass/framebuffer objects; render graph passes begin/end rendering
  directly with attachment descriptions.

### 3.4 Shaders: Slang
| Setting | Value | Consequence |
|---|---|---|
| Target | SPIR-V 1.6, emitted directly (`EmitSpirvDirectly`), entry point names preserved | One shader file can hold task/mesh/fragment/compute entry points |
| Layout | `GLSLForceScalarLayout` (scalar/C-like struct layout), column-major matrices | **One shared header (`shaderIO.h`) defines GPU structs for both C++ and Slang with identical layout** — verified this session: `InstanceData` 312 bytes / `InstanceLUT` 168 bytes, identical offsets on both sides. No manual padding structs. |
| Capabilities | `spvDescriptorHeapEXT`, `spvRayQueryKHR` | Bindless handles and ray queries available in every shader |
| Resources in shaders | Typed pointers from BDAs, heap handles for textures/samplers, push constants carry pointers + slots | Passes need only a push-constant struct, no binding declarations |
| Tooling | Runtime compilation with SPIR-V binary cache, hot reload through the file watcher, RTXDI SDK include paths | Shader iteration without restarts |

### 3.5 Planned baseline additions
Not enabled yet; each is added to §3.1–3.3 when implemented.

| Addition | Needed by | Notes |
|---|---|---|
| Additional queues (transfer, async compute) | §5.4.4, §5.11.4 | Open Decision D6 |

### 3.6 Rules for new renderer code
1. New GPU structs go in `shaderIO.h` and must be layout-identical in C++ and Slang (scalar layout).
2. Resources are passed as heap slots or buffer addresses — never as descriptor bindings.
3. Never add image layout transitions; add access declarations/memory barriers only where
   synchronization2 requires them.
4. Pipeline-state variations use dynamic state, not new pipelines.
5. Code must also work on the classic-pipeline fallback when shader objects are unavailable.

---

## 3b. Current State Snapshot (September 2026)

> **Pre-plan snapshot.** This section records the engine as it was *before* this plan's phases, as the baseline they were
> measured against. Most limits in §3b.2 have been removed since (task system, render graph, GPU scene and culling, unified
> geometry, async loading, texture streaming, profiler, BLAS compaction). For the current state see the phase tables in §5,
> the roadmap in §6 and `PROJECT_CONTEXT.md`.

### 3b.1 What exists and works
| Area | State |
|---|---|
| NRI | Vulkan 1.4 backend, VMA, descriptor heap (bindless), buffer device addresses, unified image layouts |
| Raster | Task/mesh-shader meshlet pipeline, visibility buffer → G-buffer resolve → deferred lighting, forward transparent/unlit passes, post/tonemap, 2D overlays |
| Ray tracing | Per-submesh BLAS, TLAS rebuilt from render queues, ray queries in fragment/compute passes, RTXDI ReSTIR DI/GI/PT, DDGI, NRD, DLSS/RR, reference path tracer |
| Scene | EnTT ECS, UUIDs, relationship hierarchy + dirty world-transform propagation, glTF node/skinned animation |
| Assets | Editor asset registry (YAML), cooked `.nmesh/.nsmesh/.ntex/.nmat/.nanim/.nskel`, xxHash change detection, file watcher hot-reload, Slang hot-reload with SPIR-V cache |
| Lifetime | Mark-and-sweep unloading of unreferenced assets with frames-in-flight deferred GPU release (added this session) |
| Uploads | Batched mesh uploads (shared staging + shared BLAS scratch, budgeted submits) (added this session) |

### 3b.2 Structural limits (why this plan exists)
| Limit | Evidence | Addressed in |
|---|---|---|
| **Single-threaded main loop.** Game update, render preparation, asset IO/decode and GPU uploads all run on one thread. Taskflow is vendored but unused. | Every load stalls the editor | §5.2, §5.11 |
| **CPU-driven draw submission.** CPU rebuilds per-instance data every frame, sorts into queues, builds indirect commands, re-uploads everything. | `InstanceData` is 312 bytes/instance (full material packed in), rebuilt each frame | §5.5, §5.6 |
| **No instance-level GPU culling** (meshlet cone/bounds culling only). | — | §5.6 |
| **Paged geometry buffers** with page-table indirection in shaders; pages only return memory when 100% empty. | Freed memory stays allocated while any mesh shares the page | §5.8 |
| **No memory budget / residency.** Everything referenced is fully resident. | Bistro: 4.5 GB VRAM in use, 6318 device-local allocations (Phase 0 baseline, §3b.3) | §5.8, §5.9 |
| **Synchronous asset loading and uploads.** | Bistro, measured: mesh load 1.6 s; 216 textures × ~20.7 ms ≈ 4.5 s spread over the following frames | §5.11 |
| **Uncompressed textures from PNG/JPG sources** (cooked to RGBA8). DDS sources stay BC-compressed. | ~4× VRAM for glTF sample content | §5.10 |
| **BLAS without compaction, no RT LOD.** | Per-submesh BLAS + dedicated index buffers | §5.14 |
| **Monolithic frame recording.** `Renderer::recordCommandBuffer` is thousands of lines of manually ordered passes with manual barriers. | — | §5.4 |
| **No profiler.** | Timings needed ad-hoc log instrumentation | §5.1 |

### 3b.3 Measurements recorded this session (baseline for later comparison)
> Build configuration of these numbers was not recorded — they show relative improvements only.
> The Phase 0 baseline is re-measured in an optimized build (§2 rule 9).

| Measurement | Before | After |
|---|---|---|
| Full asset-tree rescan per mesh load (286 cooked files) | walk ~50 ms + `std::filesystem::relative` ~95 ms + linear registry compares | model-folder scan, hash set, lexical paths (~10 ms) |
| Bistro mesh GPU upload (2909 submeshes) | 4839 ms | 1129 ms |
| Bistro `ImportMeshTextures` | 2132 ms | 25 ms |
| Bistro `ImportMeshMaterials` | 361 ms | 55 ms |
| Bistro texture source hashing | 1979 ms | 20 ms |
| Bistro texture loads (remaining) | — | 216 × ~20.7 ms, main thread |

Bistro reference data: 6006 nodes, 2909 submeshes, 254 materials, 343 textures (DDS, BC), 8 animation clips.

#### Phase 0 baseline — Bistro, Release, 2026-09-15

Captured with Nox Stats reports (Ctrl+F3). Conditions: Release without debugger, VSync off, editor Play mode with the
imported Bistro glTF camera, output 1816×963 (editor viewport), sample counts at their defaults, stats reset after
the load settled, 120-frame window. One capture per configuration (run-to-run noise not yet measured). These reports
predate the per-pipeline settings block, so the NRD denoiser choices are only visible as the passes that ran.

| Configuration | Render res | FPS | Frame (wall) avg ms | CPU avg ms | GPU avg ms |
|---|---|---|---|---|---|
| **A** Raster (RT off, DLSS off) | 1816×963 | 228 | 4.38 | 2.83 | 4.37 |
| **B** Hybrid native: RT shadows + SIGMA, RT reflections + NRD, ReSTIR GI + NRD, ReSTIR DI + NRD, DLSS off | 1816×963 | 16 | 61.17 | 5.84 | 61.16 |
| **C** Hybrid (as B) + DLSS Quality + Ray Reconstruction | 1211×642 | 31 | 31.91 | 4.72 | 31.87 |

GPU passes, avg ms (nested passes are included in their parent):

| Pass | A | B | C |
|---|---|---|---|
| Visibility | 1.37 | 1.37 | 1.37 |
| G-Buffer | 0.46 | 0.63 | 0.24 |
| Deferred Lighting | 2.10 | 0.25 | 0.12 |
| RT Shadows / NRD Shadows | — | 1.82 / 0.26 | 1.04 / 0.14 |
| RT Reflections / NRD Reflections | — | 7.19 / 1.88 | 3.41 / — (RR) |
| ReSTIR GI (incl. NRD GI) | — | 42.86 (1.77) | 18.56 (RR) |
| ReSTIR DI (incl. NRD DI) | — | 4.41 (1.33) | 1.47 (RR) |
| DLSS | — | — | 5.09 |
| TLAS Build | 0.00 | 0.04 | 0.06 |
| Forward 3D + blit + post + ImGui/present | 0.29 | 0.31 | 0.29 |

CPU scopes, avg ms:

| Scope | A | B | C |
|---|---|---|---|
| Submit Renderables (per-entity mesh/light submission, 2909 instances) | 1.98 | 3.50 | 2.49 |
| Transform Propagation | 0.26 | 0.55 | 0.53 |
| ImGui Build | 0.30 | 0.45 | 0.43 |
| TLAS Prepare | 0.00 | 0.42 | 0.41 |
| Record Commands | 0.08 | 0.60 | 0.55 |
| Instance + Indirect Upload | 0.08 | 0.08 | 0.08 |

Memory (all configurations): VRAM 4.45–4.51 GB used of a 7.25 GB budget in 6318 device-local allocations; host-visible
heap 214–236 MB; process working set ~0.93 GB, private bytes ~5.43 GB. Counters: 2909 instances, 2909 indirect draws,
3 transparent draws, 97 lights, 2909 BLAS.

Load: Bistro GPU upload of 2909 submeshes (geometry + BLAS) 897.9 ms.

Observations for later phases:
- **A is GPU-bound at ~4.4 ms; the CPU frame is dominated by `Submit Renderables` (~2 ms for 2909 instances).** Phases
  1–3 (tasks, persistent GPU scene, instancing) target this directly.
- **ReSTIR GI is 70 % of frame B (42.9 ms) and still 58 % of frame C.** It scales with pixel count (C renders 44 % of
  B's pixels → 18.6 ms), so it is per-pixel tracing/resampling cost, not a fixed overhead. Largest single GPU target;
  a renderer-feature optimization outside the architecture phases.
- **Visibility stays at 1.37 ms at 44 % of the pixels (C)** — geometry-bound (2909 instances of meshlets), not
  resolution-bound. Phase 3 GPU instance culling / Hi-Z target.
- **The same scene costs more CPU with ray tracing on** (Submit Renderables 1.98 → 3.50 ms, Record Commands 0.08 →
  0.60 ms, TLAS Prepare 0.42 ms). To confirm with repeated captures before attributing.
- **6318 device-local allocations** (per-BLAS / per-texture) and **2909 BLAS for 551 unique meshes** — Phase 3 instancing
  and §5.14 BLAS compaction.
- **Private bytes ~5.4 GB vs ~0.9 GB working set** — committed CPU memory to be explained (retained CPU copies of mesh or
  texture data, driver allocations) in the memory phases (§5.8, §5.9).

#### Phase 1 result — Bistro, Release, 2026-09-15

Same conditions as the Phase 0 baseline except output 1816×916 (editor layout); 15 workers + 2 IO workers
(16 logical cores). Reports now include the renderer settings: B = RT shadows + SIGMA, reflections + RELAX,
ReSTIR GI (2 spatial samples, history 10) + RELAX, ReSTIR DI (8 local / 1 infinite samples, 1 spatial, history 20)
+ RELAX; C = same with DLSS Quality + Ray Reconstruction (GI/DI denoisers off).

| Configuration | CPU frame avg ms (Phase 0 → Phase 1) | FPS (Phase 0 → Phase 1) |
|---|---|---|
| **A** Raster | 2.83 → 1.56 → 1.37 (refcount fix) → **1.14** (level split) | 228 → 234 → 236 → 240 (GPU-bound) |
| **B** Hybrid native | 5.84 → **3.07** | 16 → 16 (GPU-bound) |
| **C** Hybrid + DLSS Quality + RR | 4.72 → **3.09** | 31 → 33 (GPU-bound) |

| CPU work, config A, avg ms | Phase 0 (serial) | Phase 1 wall (main thread) | Phase 1 summed CPU time |
|---|---|---|---|
| Mesh submission (Submit Renderables → Scene Submit Systems / Submit Mesh Chunk) | 1.98 | 0.53 → **0.28** after the refcount fix | 5.86 → **2.19** after the refcount fix |
| Transform propagation | 0.26 | 0.37 → **0.17** after the level split (task wall; top levels 0.05) | 0.28 → 0.39 subtrees |

Load: Bistro GPU upload 852.2 ms (unchanged work, run-to-run variation vs 897.9 ms).

Observations:
- **Exit criterion met for frame time:** CPU frame −45 % (A), −47 % (B), −35 % (C); work visible on worker threads.
- **Parallel efficiency in mesh submission — found and fixed (confirmed by measurement):** first capture showed
  5.86 ms summed CPU time for work that took 1.98 ms serially (≈3× inflation). Cause: every `AssetManager` static
  call copied `Ref<Project>` and `std::shared_ptr<AssetManagerBase>`, and each submesh copied `Ref<Mesh>` /
  `Ref<Material>` — atomic reference-count writes on the same few counters from 15 threads (Bistro: one mesh asset,
  shared materials). Fix: `Project::GetActiveAssetManager()` (no copies) and `AssetManager::FindLoadedAsset<T>`
  (raw pointer). Re-capture: summed 2.19 ms (≈ serial cost, i.e. near-linear scaling), main-thread wait 0.28 ms.
  Rule for task code: no shared `Ref`/`shared_ptr` copies in per-item loops.
- **Transform propagation — lopsided hierarchy, fixed:** splitting at the roots' children gave one subtree almost
  all of Bistro's ~6000 nodes (0.37 ms, slower than serial 0.26 ms). Now the top levels are updated serially until
  there are `workers × 16` subtrees, then those run in parallel: 0.17 ms wall (top levels 0.05 ms).
- **Config A CPU frame after both fixes: 1.14 ms (−60 % vs the Phase 0 baseline).**
- Dragging in a model loads everything during import (`ImportAsset`) before entities exist, so the one-frame
  "missing asset" path only applies to assets that were unloaded; asynchronous appearance is Phase 5.

---

## 4. Target Architecture Overview

```
┌─ Main thread: window · input · ImGui · submit/present · frame sync point ─────┐
└───────────────────────────────────────┬───────────────────────────────────────┘
                                        │ runs the frame graph (Taskflow)
┌──────────────────────────── Game Update (tasks) ─────────────────────────────┐
│ Scene (EnTT) · Scripts · Physics · Animation · World Partition streaming      │
└───────────────┬───────────────────────────────────────────────┬───────────────┘
                │ scene change events                           │ asset requests
                ▼                                               ▼
┌───────────────────────────────┐              ┌────────────────────────────────┐
│ Render Prepare (tasks)        │              │ Asset System (async tasks)     │
│  GPU Scene updates            │◄── ready ────│  Request API · State machine   │
│  View setup · Render Graph    │   notices    │  Dependency graph              │
│  build/compile/record         │              │  Cooked containers · IO        │
└───────┬───────────────┬───────┘              └──────────┬─────────────────────┘
        │               │                                 │ decoded payloads
        ▼               ▼                                 ▼
┌──────────────┐ ┌──────────────────────┐   ┌──────────────────────────────────┐
│ Render Graph │ │ GPU Scene             │   │ Upload Manager                   │
│ passes, pool │ │ instances, materials, │   │ transfer queue, staging ring,    │
│ barriers,    │ │ transforms, lights    │   │ per-frame budget, fences         │
│ parallel rec │ └──────────┬───────────┘   └──────────────┬───────────────────┘
└──────┬───────┘            │                              │
       │                    ▼                              ▼
       │       ┌─────────────────────────────────────────────────────────────┐
       │       │ GPU Memory: VMA pools · unified geometry buffers (TLSF)     │
       │       │ Residency Manager: budgets, LRU eviction, streaming pools   │
       │       └─────────────────────────────────────────────────────────────┘
       ▼
┌────────────────────────────────────────────────────────────────────────────────┐
│ NRI (Vulkan 1.4) — graphics / compute / transfer queues                        │
└────────────────────────────────────────────────────────────────────────────────┘
          ▲ everything above schedules on one Taskflow executor (§5.2)
```

---

## 5. Subsystems

### 5.1 Profiling & Instrumentation

**Why first:** every later phase has numeric exit criteria; they need a trustworthy measurement tool.
Nothing exists today (no timestamp queries, no memory budget queries, no timers besides ad-hoc logs).

#### 5.1.1 Design: one instrumentation layer, two consumers
Engine code is instrumented **once** through Nox profiling macros. The layer forwards to:

| Consumer | Purpose | Scope |
|---|---|---|
| **Tracy** (optional, compile-time switch) | Deep analysis: full timelines, per-thread zones, locks, allocations, captures | Everything the macros emit |
| **Nox Stats** (in-engine, always available in development builds) | Live "what is slow / how much memory" overlay in the editor/game, like Minecraft's F3 or UE's `stat unit` | **Phase 0 scope:** CPU and GPU times (min / max / avg) per named scope, RAM and VRAM usage. Flame graphs, frame-time graphs and history views are later extensions of the same data. |

```
 NOX_PROFILE_FRAME()          NOX_PROFILE_SCOPE("Name")          NOX_PROFILE_GPU_SCOPE(cmd, "Pass")
          │                              │                                    │
          ▼                              ▼                                    ▼
   ┌───────────────────────── Nox Instrumentation Layer ──────────────────────────┐
   │ scope registry (static name → id) · per-thread CPU event buffers             │
   │ GPU timestamp queries (NRI, one pool per frame-in-flight)                     │
   │ memory sampler (RAM: process counters · VRAM: memory budget + VMA stats)     │
   └───────────────┬───────────────────────────────────────────────┬──────────────┘
                   ▼                                               ▼
        Tracy (zones, GPU zones via its                 Nox Stats aggregator
        manual GPU-zone API, plots)                     (rolling window min/max/avg) → ImGui overlay
```

Rules:
1. **No direct Tracy calls in engine code** — only Nox macros. Tracy can be removed or swapped without
   touching instrumented code.
2. **GPU timings come from one source:** timestamp queries owned by NRI. Results are forwarded to Tracy
   through its manual GPU-zone API and to Nox Stats — no second query set for Tracy's own Vulkan context.
3. **Zero cost when disabled:** macros compile out in shipping builds; Nox Stats can be compiled out
   independently of Tracy.
4. **The render graph instruments itself:** every pass automatically gets a CPU scope (record) and a GPU
   scope (execute) named after the pass (§5.4); job system tasks get CPU scopes by task name (§5.2).

#### 5.1.2 CPU timing
- `NOX_PROFILE_SCOPE(name)` records begin/end on a thread-local buffer (no locks in the hot path);
  names are static strings registered once to an integer id.
- End of frame: the aggregator drains thread buffers, sums durations per scope id per frame
  (a scope entered several times in a frame counts once, summed), then updates the rolling window.
- Top-level scopes defined up front (so the overlay is useful immediately): `Frame`, `Game Update`,
  `ECS Systems`, `Animation`, `Transform Propagation`, `Asset Streaming`, `Extract`, `Render Prepare`,
  `Render Graph Build/Compile/Record`, `Submit`, `Present Wait`, `ImGui`.

#### 5.1.3 GPU timing
- NRI gains a timestamp query API: `beginGpuScope(name)` / `endGpuScope()` on a command buffer, writing
  `vkCmdWriteTimestamp2` pairs into a query pool per frame-in-flight slot.
- Results are read **when that frame's fence has signaled** (never blocking), converted with
  `timestampPeriod`, and attributed to the frame they were recorded in.
- Supported by the single current queue; with additional queues (D6) each queue gets its own pools and
  results are merged per frame.
- Initial GPU scopes: each current major pass (visibility, G-buffer, lighting, RT shadows/reflections,
  DDGI, RTXDI DI/GI/PT, NRD, DLSS, forward, post, 2D/editor, ImGui) and TLAS/BLAS builds.

#### 5.1.4 Memory
| Metric | Source |
|---|---|
| **VRAM used / budget** per memory heap | `VK_EXT_memory_budget` (`heapUsage`, `heapBudget`) through `vmaGetHeapBudgets` — optional extension (§3.2) |
| **VRAM by engine category** (geometry, textures, RT, render-graph transient, staging, other) | VMA allocation statistics per pool/category, allocations tagged by the owning subsystem |
| **RAM (process)** | OS process counters (Windows: working set, private bytes via `GetProcessMemoryInfo`) |
| **RAM by engine category** (later) | tagged allocators / Tracy memory events |

Sampled a few times per second (not every frame), cached, shown in the overlay and plotted in Tracy.

#### 5.1.5 Aggregation
- Rolling window per metric (default: last 120 frames or last 2 seconds), plus "since reset".
- Per CPU/GPU scope: **min, max, avg**, last value; frame total; FPS derived from `Frame`.
- Reset on demand (hotkey/button), on scene switch, and on resize/denoiser toggles (GPU timings change
  meaning).
- Frame index attached to every sample so CPU and GPU data for the same frame line up even though GPU
  results arrive `MAX_FRAMES_IN_FLIGHT` frames later.

#### 5.1.6 Nox Stats overlay (Phase 0 UI)
- Toggle with a hotkey (F3-style) in editor viewport and game.
- Sections: frame summary (FPS, frame ms min/max/avg), CPU scopes table, GPU passes table, memory
  (RAM process, VRAM used/budget per heap and per category).
- Sortable by avg/max; scopes indented by nesting depth.
- Later (same data, no new instrumentation): frame-time graph, last-frame flame graph, spike capture
  ("freeze the frame that exceeded N ms"), per-thread view.

#### 5.1.7 Counters (added as subsystems arrive)
Instances visible/culled, draw calls, BLAS count/size, streaming requests/evictions, upload bytes per
frame, asset states per type — registered through `NOX_PROFILE_COUNTER(name, value)`, shown in Nox
Stats and Tracy plots.

The `[AssetLoad]` log lines added this session stay until equivalent scopes exist.

#### 5.1.8 Implementation (Phase 0, September 2026)
| Part | Where | Notes |
|---|---|---|
| Macros + aggregation | `NoxCore/src/NoxCore/Profiling/Profiler.{h,cpp}` | `NOX_PROFILE_SCOPE/FUNCTION`, `NOX_PROFILE_GPU_SCOPE` (RAII) and `NOX_PROFILE_GPU_BEGIN/END` (explicit pairs for the monolithic frame recorder), `NOX_PROFILE_GPU_FRAME_BEGIN/END`, `NOX_PROFILE_COUNTER`, `NOX_PROFILE_FRAME`. Static call-site registration (max 4096 scopes, fixed tables so worker threads can read them), thread-local CPU stacks + per-thread completed-scope buffers drained in `EndFrame`, 120-frame windows with last/min/max/avg and peak since reset; scope tree from each scope's most recent parent. |
| Tracy backend | `Profiling/TracyBackend.{h,cpp}` (only file including Tracy) | Tracy **v0.14.1** submodule, official CMake target `Tracy::TracyClient` (`TRACY_ENABLE`, `TRACY_ON_DEMAND`), linked PRIVATE. CPU zones via static source locations; GPU zones via the C `_serial` GPU API in TracyVulkan's order (context once, zone begin/end at record, times at readback), connection state sampled once per frame. |
| GPU timestamps | `NRI/GpuProfiler.h`, `NRI/Vulkan/GpuProfilerVK.{h,cpp}` | 256 queries per frame-in-flight slot, a valid begin always reserves its end query, debug-utils labels per scope when the extension is enabled. Scope names/nesting stay in the engine profiler. |
| Memory | `NRI::Device::getMemoryStats` (VMA `vmaGetHeapBudgets`), `Utils/PlatformUtils` (process working set / private bytes) | Sampled every 250 ms by the renderer. Per-category VRAM arrives with the memory system (§5.8, Phase 4). |
| Overlay | `Profiling/StatsOverlay.{h,cpp}`, `Profiling/StatsOverlayLayer.{h,cpp}` (pushed by `Application` for every app) | Drawn with the existing Renderer2D `DrawQuad`/`DrawString` in the normal 2D overlay pass: quads and glyphs are placed on a plane just in front of the camera, built by unprojecting the output image's corner pixels (`Renderer::getViewProjection/getOutputSize`), so the overlay sits at the top-left, pixel-exact, in the editor viewport and in shipped games. No extra pipeline, pass or shader. F3 toggles, Shift+F3 resets. Basic view (default): FPS, CPU frame time (frame minus `NOX_PROFILE_WAIT_SCOPE` time: GPU fence + present) and GPU frame time with last/avg/min/max, RAM process/installed (`SDL_GetSystemRAM`), VRAM usage/budget. Detailed view (per-scope tables, heaps, counters) is kept but off (`StatsOverlay::SetDetailed`); per-pass analysis uses Tracy for now. Like all 2D overlay content it writes entity ID -1, so picking under the overlay hits nothing. |
| Switches | `NOX_PROFILE_STATS` (ON), `NOX_ENABLE_TRACY` (OFF) CMake options, `NOX_BUILD_CONFIG` | Both off: every macro compiles to nothing. |

Differences from the design above: GPU samples are not matched to CPU frame indices in the stats windows (GPU windows are kept
separately; the frame timeline below matches them); stats reset on scene new/open and render-resolution changes (not on every
denoiser toggle).

#### 5.1.9 Frame timeline (built 2026-10-23)
The editor's **Window > Profiler**, Unreal Insights' timing view inside the editor, from the same instrumentation (no new macros):
- **Capture** (`Profiler::SetTimelineCapture`, on only while the window is open, visible and recording): each frame's CPU scopes with
  their start / end (steady clock), nesting depth and thread lane, and its GPU scopes, for the last 300 frames
  (`Profiler::GetTimelineFrame`, at most 65536 CPU scopes a frame). A GPU frame record carries the CPU frame number that recorded it,
  so its timestamps, read back a few frames later, land on that frame.
- **GPU on the CPU clock:** `NRI::GpuProfiler::calibrate` samples a device timestamp and the host clock together
  (`VK_KHR_calibrated_timestamps`, else the EXT original; the host domain std::chrono::steady_clock reads: QueryPerformanceCounter on
  Windows, CLOCK_MONOTONIC on Linux), again every second; GPU ticks become host nanoseconds through that pair, as Unreal Insights and
  Tracy calibrate. A device without it gets the GPU track drawn from its frame's start (durations right, offset unknown; the window
  says so).
- **Window:** frame-time graph of the kept frames (frame time bars against 60 / 30 fps lines, GPU frame time marks; click picks a frame,
  Follow keeps the newest whose GPU times came back); timeline with a ruler from the picked frame's start, frame boundaries, a GPU
  track and one lane per thread (main thread, task workers by name), wheel zoom around the mouse, drag to pan, click marks a scope,
  double-click zooms to it, tooltips with duration, offset, thread and source location; the picked frame's scopes summed per scope and
  track (calls, total, max, % of the frame), sortable and filterable.
- Not yet: saving / loading a capture, spike trigger ("keep the frame over N ms"), memory over time, async compute / transfer queue
  tracks.
Process memory stays in `Utils/PlatformUtils.cpp` because SDL3 has no per-process memory API.

### 5.2 Threading Model & Task System (Taskflow)

#### 5.2.1 Principles (Decision D1: no render thread)
- **No long-lived subsystem threads** — no render thread, physics thread or audio thread. Dedicated
  per-subsystem threads don't scale with core count, idle while waiting on each other, and force
  synchronization at every handoff. With Vulkan, command recording is cheap and the GPU is already
  asynchronous; GPU-driven rendering (§5.6) shrinks render-side CPU work further.
- **The main thread + one task graph.** The main thread owns the window, input and the GPU queue
  (submit/present) and orchestrates the frame. All other CPU work runs as tasks in Taskflow DAGs on a
  shared worker pool, so it scales with however many cores the machine has.
- **Per-worker *state*, not per-subsystem *threads*.** Some state must still be worker-local
  (allocators, Vulkan command pools, profiling buffers) — it belongs to whichever worker runs a task,
  not to a subsystem (§5.2.4).
- **No `std::thread` in engine code** outside `Nox::JobSystem`.

#### 5.2.2 Threads
| Thread(s) | Owns | Never does |
|---|---|---|
| **Main** | SDL window/events/input, OS-bound calls (dialogs, clipboard), ImGui frame, running the frame graph, **Vulkan queue submit & present** (one queue, externally synchronized), frame-end sync point | Heavy CPU work — that is always a task |
| **Taskflow workers** (one `tf::Executor`, default logical cores − 1, tunable) | Every task: ECS systems, animation, transforms, streaming decisions, render prepare, render-graph compile, command recording, culling setup, asset decode, cooking, BLAS batching | Blocking IO; waiting on other tasks except through graph edges / Taskflow cooperative waits |
| **IO executor** (documented exception: separate small `tf::Executor`, 1–2 workers, owned by `JobSystem`) | Only IO completion/blocking reads that can't use non-blocking IO (IoRing/overlapped IO preferred, §5.11.3) | CPU-heavy decode (hands payloads to the main executor) |

#### 5.2.3 Task graphs
- **Frame graph:** a `tf::Taskflow` describing one frame (§5.3), built once and re-run every frame;
  rebuilt only when systems or render-graph structure change.
- **Dynamic work:** render-graph pass recording and per-view culling setup as subflows; streaming and
  asset loading as async tasks outside the frame graph whose results are consumed at the next frame's
  start.
- **Visualization:** `tf::Taskflow::dump()` exports the frame graph (and loading graphs) as GraphViz
  DOT — exposed as an editor command ("Dump task graph") for inspection in GraphViz.
- **Automatic profiling:** a Taskflow observer (`tf::ObserverInterface`) emits a profiling scope per
  task using its name (§5.1), so every task shows up in Nox Stats and Tracy without manual macros.
- **Engine API** (`Nox::JobSystem`) wraps Taskflow for most code: `ParallelFor`, `Async` (futures,
  cancellation tokens), named graph building. Subsystems never create their own executors.
- **Priority** (frame-critical > streaming > background cooking) is enforced by `JobSystem`
  (admission limits, per-frame budgets, deferral of background work while frame work is pending) rather
  than relying on scheduler-level priorities.

#### 5.2.4 Worker-local state
| State | Why worker-local |
|---|---|
| Frame arena allocator (reset each frame) | No heap churn, no allocator contention |
| NRI command pool(s) per worker | Vulkan command pools are not thread-safe; parallel recording needs one pool per recording thread |
| Profiling event buffer | Lock-free CPU scope recording (§5.1.2) |
| Scratch buffers (culling, sorting, decoding) | Avoid contention and false sharing |

#### 5.2.5 Ownership & synchronization rules
1. **ECS access:** each system task declares the component sets it reads/writes; the frame graph orders
   conflicting systems and runs non-conflicting ones in parallel.
2. **Structural ECS changes** (create/destroy entities, add/remove components) from tasks are recorded
   into per-worker command buffers and applied on the main thread at the frame sync point. State that
   systems toggle every frame is data, not structure: `DirtyTransformComponent` is present on every entity
   and marking/clearing writes its flag.
3. **Vulkan:** queue submit and present only on the main thread; command recording on any worker using
   that worker's pool; NRI resource creation is thread-safe through an allocation lock; GPU object
   release goes through the deferred release queue (§5.8.4) processed at the frame sync point.
4. **Blocking** only on the main thread, only at defined points: frame graph completion, present,
   frame-in-flight backpressure.
5. **Cross-task data** flows through graph edges, per-frame snapshot structures, or lock-free queues —
   no shared mutable state without a declared owner.
6. **Assets from tasks:** `AssetManager::FindLoadedAsset` only (never imports; raw pointer, no reference-count
   writes, valid while the main thread waits for the graph). Handles that are not loaded
   are reported (`WorkerLocal` lists) and loaded with `GetAsset` on the main thread at the sync point; they
   are used from the next frame.

#### 5.2.6 Implementation (Phase 1, September 2026)
| Piece | Where | Notes |
|---|---|---|
| `JobSystem` | `NoxCore/src/NoxCore/Tasks/JobSystem.{h,cpp}` | Owned by `Application` (first member: created first, destroyed last). Main executor: `SDL_GetNumLogicalCPUCores() - 1` workers; IO executor: 2 workers. `ParallelFor` (deterministic chunk layout, cooperative wait on workers via `tf::TaskGroup::corun`), `Async` / `AsyncIO` → `TaskFuture` + `CancellationToken`. Taskflow headers only in `Tasks/*.cpp` and the internal `TaskObserver.h`. |
| Worker threads | `tf::WorkerInterface::scheduler_prologue` | Assigns the `WorkerLocal` thread slot (CPU workers, IO workers, main thread last) and names the thread for Tracy ("Nox Worker N", "Nox IO N"). |
| Profiling | `Tasks/TaskObserver.{h,cpp}` | `tf::ObserverInterface` on both executors: `on_entry`/`on_exit` open/close a profiler scope named after the task (thread-local name → scope id cache, `Profiler::RegisterNamedScope`). Covers graph tasks, parallel-for chunks, async and subflow tasks. Taskflow's TFProf (`TF_ENABLE_PROFILER`) still works alongside. |
| `WorkerLocal<T>` | `Tasks/WorkerLocal.h` | One cache-line-aligned `T` per thread slot. |
| `FrameArena` | `Tasks/FrameArena.{h,cpp}` | Linear allocator per thread slot, reset by `Application` at the frame sync point after `drawFrame`. Frame graph tasks only (async work outlives the frame). |
| `SystemGraph` + `ComponentAccess` | `Tasks/SystemGraph.{h,cpp}` | D11 decided: systems declare `Read<...>()` / `Write<...>()` (EnTT `type_hash`; tag types for non-component resources). A system runs after every earlier-registered system it conflicts with; others run in parallel. `RunAfter` for non-data ordering; `DumpDot` via `tf::Taskflow::dump`. |
| `EntityCommandBuffer` | `Scene/EntityCommandBuffer.{h,cpp}` | Create/destroy entity, add/replace/remove component recorded into the recording thread's frame arena; applied by the Scene on the main thread in slot order. |
| Priorities (built 2026-10-23) | `JobPriority` in `JobSystem.h` | Taskflow has no priorities, and a worker waiting inside a frame task (`corun`) runs any task it finds, a multi-second cook included, so a cap on the shared pool would not protect the frame. As Unreal does with its background workers: **High** work (Jolt physics jobs) runs on the frame workers at once; **Normal** (scene reading, asset reads and imports) and **Background** (texture / mesh cooking, split-mesh cooking) are queued by JobSystem and run on a second executor of as many **background workers** as frame workers at **low OS thread priority** (`SDL_SetCurrentThreadPriority`), Normal first, Background on at most three quarters of them so streaming keeps room. A `ParallelFor` inside queued work keeps its deterministic chunks but runs them on the background workers its priority may still take (none: the calling thread runs them all). The IO executor takes queued reads by priority too: High reads (texture mip streaming) start at once, Normal reads wait for a free IO worker, Background reads use one. Every `Async` / `AsyncIO` names its priority. `SceneLoader` reads through `Async(Normal)` instead of a thread of its own, sharing only the parse result with the job (a level dropped mid-read no longer waits for it). Not built: per-frame time budgets for applying results on the main thread beyond the existing upload / publish budgets, cancelling queued work by owner. |
| Worker NRI command pools | Phase 2 | Designed with render-graph partition recording (submit order, per-buffer GPU timestamps, barrier state across partitions); they will live in `WorkerLocal`. |

### 5.3 Frame Pipeline (Main Thread + Frame Graph)

```
Main thread : Poll SDL events/input → apply completed async results (loads, readbacks)
              → run FRAME GRAPH (below, on workers) → ImGui draw data → Submit → Present
              → Sync point: apply structural ECS changes, process deferred releases

Frame graph : ┌ Game Update ─────────────────────────────────────────────────────────┐
              │ ECS systems (parallel by access sets) → Transform propagation         │
              │ (parallel per hierarchy depth) → Animation (parallel per animator)    │
              └──────────────┬───────────────────────────────────────────────────────┘
                             ▼
              ┌ Streaming decisions (requests to the async lane) ────────────────────┐
              └──────────────┬───────────────────────────────────────────────────────┘
                             ▼
              ┌ Render Prepare ──────────────────────────────────────────────────────┐
              │ GPU scene deltas → views (camera/shadows/probes) → build RG           │
              │ → compile RG → record passes (parallel subflow, worker command pools) │
              └──────────────────────────────────────────────────────────────────────┘

Async lane  : asset IO → decode → upload staging (tasks outside the frame graph, budgeted)
```

- **Render Prepare reads ECS state after Game Update finishes** (graph edge), so no snapshot copy is
  needed in the default model.
- **Frame pipelining (option, off by default):** Render Prepare of frame N runs concurrently with Game
  Update of frame N+1 using an extract snapshot (changed transforms, materials, lights, views only).
  This is the one benefit a render thread would give; in a task graph it is a graph shape, not a thread.
  Enable only if profiling (§5.1) shows Game Update + Render Prepare exceeding the frame budget.
- **ImGui** runs on the main thread (it touches editor/ECS state and is not thread-safe); its draw
  data is recorded by an overlay pass in the render graph.
- **Editor readbacks** (entity picking, box select) are requests tagged with a frame ID, completed on
  the main thread at frame start once that frame's fence has signaled (replaces the current "previous
  slot" readback, which can read an in-flight buffer).
- **Phase 1 shape (September 2026).** Render Prepare is still `Renderer::drawFrame` until the render graph
  (Phase 2), so the scene runs two `SystemGraph`s from its `OnUpdate*`:
  - **Scene Update:** Physics 2D → Animation → Transform Propagation (roots serial, root-child subtrees
    in a `ParallelFor`), ordered by declared access; then the scene sync point (command buffers, missing
    assets). **Animation is parallel per animator (2026-10-23):** animators are grouped by the node table they
    write into (one imported model's joint and node entities; an animator without one is its own group), a group
    updates in registry order inside one task and groups run in a `ParallelFor`, so no two tasks write the same
    entity; the component pools the animators look up are created before the tasks (EnTT creates a missing pool
    on lookup). The skinned meshes' joint matrices (`GetBoneTransforms`) are built in a `ParallelFor` too and
    handed to the renderer in order.
  - main thread: primary camera → `Renderer::BeginScene` (may recreate render-resolution resources).
  - **Scene Submit:** Submit Meshes (`ParallelFor` over mesh entities; each chunk fills its own
    `MeshSubmissionChunk`, merged in chunk order by `Renderer::EndMeshSubmission`, so instance order equals
    the serial loop) ‖ Submit Lights ‖ Submit 2D; then the scene sync point.
  - Application frame sync point after `drawFrame`: frame arenas reset.
  - Pick results are copied right after `acquireNextImage` waited for the slot's previous submission
    (`Renderer::readPickResult`); `getPickedEntityID(s)` read that CPU copy.
  - Phase 2 composes the Render Prepare graph after the scene graphs.

### 5.4 Render Graph

#### 5.4.1 Scope: what the render graph is (and isn't)
The render graph **schedules GPU work and manages frame-lifetime GPU resources**. It is rebuilt and
compiled every frame from pass declarations. It does **not** own or prepare persistent data.

| Render graph does | Other systems do (and plug into the graph, §5.4.8) |
|---|---|
| Order passes, cull unused passes | **GPU Scene manager** (§5.5): ECS changes → instance/material/transform/light deltas |
| Allocate/pool/alias transient resources, manage history resources | **Upload manager** (§5.11.4): staging, budgets, transfer queue, completion |
| Derive and insert synchronization (barriers, present transition) | **Residency / streaming** (§5.9): what is resident, eviction |
| Begin/end rendering, attachments, clears, viewport/scissor | **GPU-driven culling** (§5.6): culling logic, buffers, draw buckets |
| Partition, record (in parallel), submit command lists | **Asset system** (§5.11): loading, decoding |
| Per-pass profiling, debug labels, visualization, validation | **Shader/pipeline management**: compilation, hot reload |
| Readbacks as graph outputs | **Material system**: material table contents |

Rationale: the graph lives for one frame; data preparation spans many frames and must not be recreated
per frame. Separating them keeps passes small and data systems testable without rendering.

#### 5.4.2 Responsibilities
1. **Pass declaration, ordering and culling.** Passes declare resources with access (§5.4.4), their
   queue, and side effects (present, readback). Compile performs a topological sort and culls passes
   whose outputs have no consumer. This replaces most manual feature branching in
   `recordCommandBuffer` (RT features, denoisers, debug views): disabled features simply do not
   contribute passes, or their outputs go unconsumed. Feature arbitration rules from the RT plan
   decide which passes are added.
2. **Resource lifetimes** (§5.4.3): transient allocation with pooling and aliasing, render-resolution vs
   display-resolution sizing, resize handling, history resources with rotation and reset, stable
   bindless slots for graph resources.
3. **Synchronization** (§5.4.5): derive where barriers are required; strategy switchable and profiled.
4. **Pass setup boilerplate:** `beginRendering`/`endRendering`, attachment load/store ops, clear values,
   viewport and scissor (including reverse-Z conventions) derived from declared targets — passes only
   bind shaders, set dynamic state and draw/dispatch.
5. **Recording and submission** (§5.4.6): command-list partitions recorded as tasks on the job system
   with worker-local command pools, submitted in graph order on the main thread; queue assignment once
   more queues exist (D6).
6. **Instrumentation and debugging** (§5.4.7): automatic CPU scope (record) and GPU scope (execute) per
   pass (§5.1), `VK_EXT_debug_utils` labels, graph visualizer, texture inspection of any graph resource,
   validation mode, readbacks (entity picking, box select, screenshots) as graph outputs completed by
   frame ID.

#### 5.4.3 Resources
| Kind | Examples | Lifetime handling |
|---|---|---|
| **Transient** | G-buffer targets, NRD/RTXDI intermediates, DLSS inputs, Hi-Z pyramid, bloom chains | Described by a desc (size relative to render/display resolution, format, usage); allocated from a pool keyed by desc; memory aliased between passes with non-overlapping lifetimes; returned at frame end |
| **History** | DLSS/TAA history, NRD history, RTXDI reservoirs, previous depth/normals/albedo, DDGI atlases, path tracer accumulation | Declared once with a history length; rotated by the graph each frame; reset on resize, denoiser/feature toggles, or explicit reset (replaces today's manual `m_resetNRD`/`m_resetDLSS` flags) |
| **External** | Swapchain, TLAS, GPU scene buffers, unified geometry buffers, bindless material textures, staging buffers | Owned by other systems; imported per frame with their access declared; the graph never frees them |

Bindless: graph-owned textures expose descriptor-heap slots for use in push constants. Slots of pooled
transients are stable per pool entry so descriptors are not rewritten every frame. Freed slots return
through the deferred release queue (§5.8.4).

#### 5.4.4 What the graph tracks, given the baseline (§3)
With `VK_KHR_unified_image_layouts` all images live in `General`; with `VK_EXT_descriptor_heap` + buffer
device addresses there are no descriptor sets to update per pass; with shader objects and dynamic state
there are no pass-specific pipelines; with dynamic rendering there are no render pass/framebuffer objects.
The graph therefore tracks **access and lifetime**, not state:
- **Access declarations:** `Sampled`, `Storage` (read/write), `ColorTarget`, `DepthTarget` (read/write),
  `ASBuild`, `ASRead`, `TransferSrc`, `TransferDst`, `Present`, `Readback`.
- Used for: ordering, culling, validation, aliasing safety, barrier derivation.
- The only layout transition in the engine is the **swapchain image for present**, emitted by the
  graph's final present pass.

The old `NRI::ResourceState` layout model in `architecture.md` v3 is replaced by this access model.

**Async compute (built 2026-10-23, D6).** `RGPassFlags::AsyncCompute` marks a compute pass that may run on the device's
compute-only queue (`NRI::QueueType::Compute`, a family with compute but not graphics: its own front end, so its work runs beside
the graphics queue's; `Device::hasAsyncComputeQueue`). With `RenderGraph::SetAsyncCompute` and Precise synchronization:
- **Blocks, fork and join** (`RenderGraph::PlanQueues`, after culling): consecutive executed async passes form a block. It starts
  after its **fork**, the last earlier graphics pass it conflicts with (one of them writes what the other accesses) -- at least the
  frame's first graphics pass, never before the previous block's fork; the first later graphics pass it conflicts with is its
  **join** and waits for it (none: the frame's last graphics submission does, so the frame fence covers the block). A block with
  nothing graphics before it or after its fork stays on the graphics queue, and the planning starts over.
- **Lifetimes:** what a block accesses lives from its fork to its join, so no transient of the graphics passes running beside it
  takes its memory (the pool aliases by lifetime).
- **Barriers:** on the compute queue a barrier names only that queue's stages and accesses (compute, transfer, AS build, indirect,
  ray tracing); what the graphics queue did before is ordered by the timeline wait at the fork. Graphics barriers are unchanged.
- **Submissions** (`RGSubmission`, `Execute`): the graphics segments between fork / join points and one per block, each its own
  command buffers (a compute queue's from a pool of that family, set up with the descriptor heaps only). A fork segment signals the
  graph's graphics timeline semaphore, the block waits for that value and signals the compute timeline, the join segment waits for
  it; every submission also waits for the uploads. The last submission is graphics and presents. vkQueueSubmit2's semaphore scopes
  reach earlier / later submissions on the same queue, which orders the next frame after this one's compute work.
- **Resources:** with a compute-only family every buffer and image is created `VK_SHARING_MODE_CONCURRENT` over the queue
  families (no ownership transfers). Reported free on NVIDIA; on AMD concurrent images can lose compression -- ownership transfers
  would be the alternative there.
- **Profiling:** a compute command buffer's GPU scopes go on the timeline's "GPU Async Compute" track (Window > Profiler), timed
  where that family has timestamps, not sent to Tracy (one GPU context there).
- **First user: the screen-probe GI** (its five passes are compute dispatches since this change, writing their targets as storage
  images; the pass order declares DDGI and the GI right after the G-buffer so that SSAO, RTAO, the occlusion blur, the NRD guides
  and the RT shadows run beside it, and the RT reflections or the lighting join). Toggle in the renderer settings, on by default
  where the queue exists.
- **Fixed with it (2026-10-24):** Precise synchronization tracked state per graph resource, so two transients the pool aliases onto
  one texture (lifetimes that do not overlap) got no barrier between the first one's last access and the second one's first write
  (SYNC-HAZARD-WRITE-AFTER-WRITE before "NRD Shadows" once the new pass order changed which textures alias). The state is tracked per
  physical resource now; the test has the case (it failed before the fix).
- **Tested** without a GPU: a render graph test against generated NRI mocks (fork / join placement, submission order and timeline
  values, compute barriers free of graphics stages, no aliasing beside a block, async off, no compute queue), under AddressSanitizer
  and UBSan. Not yet measured on a GPU.

#### 5.4.5 Synchronization strategy
Today `NRI::CommandBuffer::executionBarrier()` records a **blanket** synchronization2 memory barrier
(all stages, all access) through `pipelineBarrier2`, placed by hand 34 times in `recordCommandBuffer`;
acceleration structures use dedicated barriers (`TransferToBuild`, `BuildToBuild`, `BuildToShaderRead`).

The graph derives **where** synchronization is needed from access transitions between consecutive
passes (a resource written, then read or written by a later pass; AS build → read; transfer → build;
later: cross-queue handoff). **How** it is emitted is a switchable strategy:

| Strategy | Emission | Trade-off |
|---|---|---|
| **Blanket** (matches today) | One blanket barrier at each boundary where any access transition occurs (or once per command-list partition) | Minimal CPU work and code; may over-synchronize and constrain driver scheduling |
| **Precise** | Barriers scoped to the transitioning resources/access types only | More barrier data per boundary; potentially fewer implicit waits in the driver |

**Profiling experiment (Phase 2 exit task):** run reference scenes (Sponza, Bistro) with both strategies
and compare CPU record time, GPU frame time and frame pacing using §5.1. **Result (September 2026, Bistro A/B/C):** equal GPU and CPU cost; D13 decided for **Precise** as the default because it is validated by synchronization validation and gives the driver exact information (§5.4.11).

#### 5.4.6 Recording, submission & queues
- Passes are grouped into **command-list partitions** (contiguous passes on one queue).
- Each partition is recorded as a task on the job system (§5.2) using the worker's own NRI command pool;
  small partitions may be merged to avoid task overhead.
- Submission happens on the main thread in graph order (single queue today).
- Once additional queues exist (D6): compute-queue candidates are GPU culling, Hi-Z build, NRD, RTXDI
  resampling, DDGI updates, BLAS builds/compaction; transfer-queue for the upload manager. Cross-queue
  handoff becomes an access transition the graph synchronizes.

#### 5.4.7 Tooling
- **Graph visualizer** (editor panel): passes in order, resources and lifetimes, aliasing, culled passes,
  per-pass CPU record time and GPU time; optional GraphViz DOT export like the task graph (§5.2.3).
- **Texture inspection:** view any graph resource of the last frame (generalizes today's debug view modes
  0–19 into "inspect any intermediate").
- **Validation mode:** read-before-write, undeclared access, use of a resource outside its lifetime,
  bindless slot misuse, missing present.
- **Debug labels:** every pass recorded inside a `VK_EXT_debug_utils` label for RenderDoc/Nsight captures.

#### 5.4.8 Integration with data systems
Data systems plug in without the graph owning their data:
1. **Register external resources** each frame (e.g. GPU scene buffers, TLAS, geometry buffers) with the
   access their passes need.
2. **Contribute passes** that perform their GPU work, so ordering is automatic:
   ```
   Scene Upload (GPU Scene) → Upload Flush (Upload Manager) → BLAS Builds → TLAS Build
     → Hi-Z Build → GPU Culling (Culling) → Visibility → G-Buffer → Lighting / RT passes → ...
   ```
3. **Consume outputs** asynchronously where needed (streaming feedback buffers, readbacks) through the
   graph's readback mechanism.

#### 5.4.9 Relationship to `architecture.md`
Kept: RenderGraph-as-compiler concept, Taskflow-based execution, transient aliasing.
Changed: resource-state/layout model → access model (§5.4.4); `RenderJob` semaphores → queue partitions
submitted on the main thread; RAII render-pass scopes → optional sugar, since pass setup is derived by
the graph; data preparation explicitly outside the graph (§5.4.1).

#### 5.4.10 Migration
Port `recordCommandBuffer` section by section into passes while keeping images identical (validated with
existing debug view modes and texture inspection). Order: visibility → G-buffer → lighting → forward →
post → 2D/editor → RT passes (shadows/reflections) → RTXDI/NRD/DLSS → path tracer. The 34 manual
`executionBarrier()` calls disappear as their sections are ported.

#### 5.4.11 Implementation (Phase 2, September 2026)
Decisions: **D2 = typed handles + typed blackboard**; pass code split per feature under `Renderer/Passes/`; worker NRI
command pools built in this phase (§5.4.6). Delivered in steps, each verified by identical images before the next:

| Step | Content | Status |
|---|---|---|
| 2a | `NoxCore/src/NoxCore/RenderGraph/` core: `RGTexture`/`RGBuffer` handles, `RGBlackboard`, `RGBuilder` access + color/depth/swapchain targets, `RenderGraph::Compile` (culling back to front: side effects = `NeverCull`, swapchain, writes to imported resources; synchronization front to back: a pass synchronizes after itself when a later executed pass accesses what it wrote), `Execute` (begin/end rendering from declared targets, flipped-Y viewport, swapchain present transitions, CPU+GPU profiler scope per pass and lazily opened groups, Blanket barrier). Whole frame ported: `Renderer::recordFrame` = `prepareFrameGraph` (imports every renderer texture/buffer, resolves one-shot resets and history slots) + feature `add*` functions (`Passes/{Raster,RT,ReSTIR,Lighting,Upscale,Post,Editor}Passes.cpp`) + compile + execute + one final uniform upload. Intra-pass barriers became separate passes (light PDF mips, presampling). No `executionBarrier()` call outside the graph. | **Verified 2026-09-16:** images unchanged; Release Bistro CPU frame A 1.08 / B 2.34 / C 2.23 ms, GPU frame A 4.15 / B 59.18 / C 30.29 ms (≈ Phase 1); graph build 0.01–0.07 ms, compile < 0.01 ms. Per-pass GPU timing shows ReSTIR GI Initial = 37.5 of 41.2 ms (B) — the initial candidate trace is the GI cost. |
| 2b | `RGResourcePool`: transients keyed by (resolved size, format, usage, mips), allocated at compile in first-use order and aliased across non-overlapping pass ranges, bindless slots kept for the entry's life (per-mip storage slots for Storage usage), released 120 frames after last use through a 4-frame delayed queue. `RGSize` render/output/absolute resolution. History resources `GetHistoryTexture/GetHistoryBuffer(key, desc, count)` recreated on description change, `ResetHistory(key)`/`ResetAllHistory` + `WasHistoryReset` also drive NRD and DLSS (external history). Renderer-owned render targets removed (~40 members, the graph-owned `create*Resources`, `m_resetNRD`/`m_resetDLSS`/`m_ddgiFirstFrame`); only the editor scene image, environment, neighbor offsets, TLAS and picker staging stay imported. Histories: DDGI atlases ×2, path tracer accumulation ×2, previous depth/normal/albedo/material, NRD view Z + normal/roughness guides, ReSTIR GI (1) / DI (3) / PT (3) reservoirs. Features set per-frame flags when their passes are added; consumers declare and bind only produced outputs; bindless slots go into the uniforms after compile (`resolveFrameUniforms`). Resize releases all graph resources (GPU idle) and resets every history. Empty scenes get a G-buffer clear pass. | **Verified 2026-09-16:** images unchanged, resize/DLSS/feature toggles OK. Release Bistro (viewport 1816×874 vs 918 in 2a; B/C with ReGIR off): VRAM heap A 4510 → 3614 MB, B 4510 → 3909 MB, C 4320 → 4101 MB (features that don't run no longer hold targets); CPU frame A 1.15 / B 2.32 / C 2.30 ms, GPU frame A 4.12 / B 58.24 / C 29.66 ms (≈ 2a); graph build 0.01–0.07 ms, compile ≤ 0.01 ms. |
| 2c | Parallel recording (cross-checked against https://docs.vulkan.org/tutorial/latest/17_Multithreading.html). `RenderGraph::Execute(frameSlot)` splits executed passes into consecutive chunks, one command buffer each, returned in submission order; `Device::submitCommandBuffers` submits them in one queue submission. Chunk planning uses each pass's smoothed measured record time: every chunk count up to min(8, workers) is estimated as wall time (chunks sharing an exclusive key add up) + 0.05 ms per extra chunk, the cheapest wins, one chunk records inline. `RGBuilder::RecordExclusive(key)` ("NRD", "Streamline", "ImGui") → `JobSystem::RunTasks` with one `tf::Semaphore` per key (no worker blocks). One `CommandAllocator` per (chunk, frame slot), reset whole (`CommandBufferReset::WithAllocator`) after the slot's fence. `SetCommandBufferSetup` records descriptor heaps + baseline dynamic state at every command buffer start. Execute callbacks only record: NRD result selection, ReSTIR PT camera-moved, TLAS build/update, pick readback bookkeeping, meshlet queue offsets (per-pass `MeshletDrawCursor`) moved to setup; DLSS failure blits its input to the output. GPU profiling: `GpuScopeContext` per command buffer, atomic query pairs, query reset + frame timestamps in the first/last buffer, group scopes reopened per chunk and summed. Editor toggle "Parallel Recording", report header + "Command Buffers" counter. | **Verified 2026-09-17:** images unchanged, validation clean. Release Bistro, Parallel recording ON / OFF — Execute Frame Graph (record) A 0.08 / 0.08 ms (1 buffer), B 0.53 / 0.50 ms (1 buffer: NRD passes spread over the frame serialize; a first naive split into 2 cost 0.60 ms, fixed by key-aware planning), C 0.37 / 0.48 ms (2 buffers: DLSS and NRD record concurrently). CPU frame A 1.10 / 1.11, B 2.33 / 2.33, C 2.06 / 2.11 ms. |
| 2d | Editor **Render Graph** panel (Window > Render Graph; `RGFrameReport` filled only while open): passes in frame order under groups (culled, command buffer, CPU record ms, GPU ms, Raster/Present/Sync/exclusive keys, per-pass accesses), resources (kind, size/format/mips, memory, lifetime bar, GPU object id for aliasing), history resets, pool memory, DOT export (`WriteRenderGraphDot`). **Texture inspection:** `RenderGraph::FindTexture/InspectTexture` records an inspector right after the last executed writer of any graph texture (else last reader, else frame end); `TextureInspect.slang` converts float (exposure, channels), signed ids, visibility pairs and reverse-Z depth into an RGBA8 history texture shown in the panel, raw texel value via a 1x1 probe readback. **Validation** (Debug default, panel toggle; each message logged once): undeclared access in execute callbacks (`RGPassContext` checks the pass's declarations), stale handles (handles carry a frame id), transient read before any write, writes to `RGImportAccess::ReadOnly` imports, no presenting pass; unused transient writes listed. Found and fixed: Forward 3D loaded an unwritten HDR scene in path tracer mode. **Debug labels** per pass/group from the graph (`CommandBuffer::beginDebugLabel`), `VK_EXT_debug_utils` enabled whenever available, labels removed from the GPU profiler. | **Verified 2026-09-17:** panel, inspection and validation work; DOT export of hybrid + DLSS RR: 31 passes, none culled, 2 command buffers split before DLSS. |
| 2e | `RGSynchronization::{Blanket, Precise}` (panel toggle, report header). Precise: every declaration carries an NRI `ResourceState` (access + stage bits: shader read/write at task/mesh/fragment or compute, color/depth attachment, transfer, AS build/read); Compile walks executed passes and, where the previous or new access writes, records per-resource `ImageMemoryBarrier2`/`BufferMemoryBarrier2` (layouts stay General) at the start of the consuming pass via `CommandBuffer::resourceBarriers`. Synchronization validation (vkconfig) clean with Precise except NGX-internal messages; it found two pre-existing issues, fixed: the ImGui/present pass loaded a swapchain image just transitioned from Undefined (now cleared), and `TransferToBuild` lacked shader-read for BLAS/TLAS build inputs. | **Verified 2026-09-17:** images identical. Release Bistro, Blanket / Precise — GPU frame A 4.13 / 4.16, B 58.41 / 58.15, C 29.66 / 29.32 ms; per-pass GPU times equal within noise; CPU Record Commands A 0.10 / 0.11, B 0.58 / 0.60, C 0.46 / 0.42 ms. **D13 decided: Precise** (default). |

### 5.5 GPU Scene

#### 5.5.1 Persistent buffers (replace per-frame `InstanceData` rebuild)
| Buffer | Content | Update |
|---|---|---|
| `InstanceBuffer` | mesh/LOD group id, material id, transform index, bounds (local sphere/AABB), flags (cast shadow, RT visible, double-sided, alpha mode, skinned, entity id), last-visible frame | slot alloc, dirty upload |
| `TransformBuffer` | world matrix, previous world matrix (motion vectors), normal matrix (or derived in shader) | dirty upload |
| `MaterialBuffer` | material parameters + bindless texture slots, shared by instances (today duplicated into every instance) | on material change |
| `MeshTable` | per mesh/LOD: geometry ranges in unified buffers, meshlet ranges, BLAS id, bounds — **one entry per unique mesh, shared by all instances** | on load/unload |
| `LightBuffer` | lights (already exists; becomes dirty-updated) | dirty upload |

Target instance record size: **≤ 64–96 bytes** (vs 312 today).

#### 5.5.2 Update path
ECS change events (component add/remove/modify, transform dirty) → Render Prepare tasks collect
deltas → write into a mapped staging ring (ReBAR mapped where available) → compute/copy scatter into the
persistent buffers. Full rebuild only on scene switch.

#### 5.5.3 Skinned meshes
GPU skinning in compute into per-instance deformed vertex ranges (feeds raster and BLAS refit),
replacing per-frame skinning in the mesh shader for RT correctness.

Stage 1 is implemented pending capture verification: `ComputeSkinning.slang` writes a per-frame-ring local-space
position/normal cache for opaque and masked skinned instances. Visibility and G-buffer material resolve share that
cache, and the previous ring slot supplies skeletal motion vectors. Stage 2 remains the per-instance animated BLAS
update/refit that makes ray queries see the deformed pose.

**Status 2026-10-20: Stage 2 is built.** Each ray-traced skinned instance has its own BLAS (`AnimatedBLAS` in
`Renderer.h`), rebuilt or refit from the compute-skinned vertices in the "Animated BLAS Update" pass
(`Renderer::addAnimatedBLASPass`, `Renderer/Passes/RasterPasses.cpp`), right after "Compute Skinning" and before the TLAS
build.

#### 5.5.4 Implementation (Phase 3, September 2026)
Decisions: culling per view (N-view infrastructure; a second fully rendered viewport later with per-view graph passes),
per-object motion vectors (previous world matrix), mesh-shader skinning kept (compute skinning §5.5.3 later), meshlet
culling off unless captures prove it faster.

| Step | Content | Status |
|---|---|---|
| 3a | Mesh instancing: `MeshImporter` cooks each glTF mesh once; nodes share its submesh range (geometry + BLAS once per unique mesh, TLAS places each instance). | **Verified 2026-09-17:** Bistro 551 submeshes/BLAS for 2909 instances, identical images, Debug upload 536 ms (was 850–1360). Release A: VRAM 3641 → 3127 MB with 6283 → 1532 allocations, private bytes 4561 → 3948 MB, GPU frame 4.13 → 3.95 ms, Visibility 1.24 → 0.86 ms (packed geometry). Also fixed: the material panel edited metallic-roughness factors on specular-glossiness materials (no visible effect); it now shows the material's workflow fields. |
| 3b | GPU scene (`Renderer/GpuScene`): persistent slot tables for instances, transforms (world/normal/previous world), shared materials, meshes and per-instance ray tracing records, CPU mirrors with dirty slots scattered from per-frame-slot staging in the first graph pass ("GPU Scene Update"). Change tracking: EnTT signals on Mesh/Material/Animator components, transform propagation lists moved mesh entities, editor edits patch components / notify material changes, scene switch re-registers. Draw buckets kept incrementally (CPU still builds the per-frame draw list); TLAS from a compact bucket-ordered list, rebuilt when ray traced instances change (NVIDIA: rebuild the TLAS rather than refit). Raster shaders read the tables (visibility stores the instance slot), RT shaders one `GpuRayTracingInstance` record; per-object motion vectors from the previous world matrix. | **Verified 2026-09-17:** identical images, moving/animated objects keep correct shadows and velocity, edits/Play/Stop work. Release A: CPU frame 1.09 → 0.76 ms (Scene Submit Systems 0.25 → 0.01 ms, GPU scene sync ≤ 0.03 ms for 2909 instances), GPU 3.90 ms. B: GPU 56.7 → 56.0 ms, C: 29.3 → 29.0 ms; TLAS rebuild 0.24–0.5 ms per frame while Bistro animates (refit was 0.03 ms). Finding: ray query candidate/hit lookups reading instance → material → mesh → transform from separate tables made ReSTIR GI Initial 35 → 46 ms (same data, same TLAS, same code shape measured); one colocated record per instance restored it. |
| 3c | GPU instance culling per view: the draw list (instance slots in bucket order) is uploaded only when buckets change, a compute pass (`InstanceCulling.slang`, four dispatches) tests world bounds against the view frustum and compacts the visible entries per bucket into visible instance slots + indirect commands + draw counts without atomics (visibility bits, visible counts per 256-entry block, block offsets and bucket counts, then the writes in entry order so transparent sorting survives); the raster passes draw per bucket with `drawMeshTasksIndirectCount`. Frozen culling view (Render Graph panel; its F key was dropped 2026-10-24 -- it fired while typing and with Frame Selected -- and the viewport says when it is frozen), skinned instances are never culled. Task shader is lean by default (no bounds fetch); optional per-meshlet frustum test. Vulkan pipeline statistics per raster pass (triangles, fragments, task/mesh invocations) behind a panel toggle. | **Verified 2026-09-17:** identical images, freeze shows what was culled. Release A (1816x887, Bistro, 2909 instances): instance culling off Visibility 1.17 ms / GPU 4.14 ms; on 0.67 ms / 3.92 ms with 2320 visible and 0.12 ms culling; looking away 0 visible, GPU 0.77 ms (250 -> 1000 FPS). Meshlet frustum culling on top: Visibility 0.60 ms / GPU 3.84 ms (kept as an option, default off). CPU draw list build gone (Scene Submit ~0.01 ms). Ray traced passes are unaffected by design: the TLAS holds the whole scene. Cone culling dropped: meshoptimizer's cone test removed visible meshlets here (cause not found), so only the frustum test remains. Two bugs found and fixed while testing: the four culling dispatches ran without barriers between them (flicker while moving), and the indirect commands/counts were barriered as shader reads instead of indirect reads. |
| 3d | Two-phase Hi-Z occlusion culling per view: a depth pyramid (`HiZBuild.slang`, level 0 is half the render size, every level keeps the farthest depth of its 2x2 for reverse-Z, odd sizes fold the leftover row/column in) is built from the visibility depth into a history texture. Phase 1 tests each instance's previous transform against the previous frame's pyramid with that frame's view, and sends what it rejects to a candidate list instead of dropping it; after the pyramid is rebuilt, phase 2 (`lateMain`) re-tests the candidates with their current transform and view and writes the commands for a `Visibility Late` pass, so a wrongly rejected instance still draws in the same frame. Pyramid inspectable per mip in the Render Graph panel (`Depth curve` shows reverse-Z values), counters for candidates and phase 2 draws, Draws/s and Triangles/s. | **Verified 2026-09-17:** identical images (checked against a diagnostic that drew every candidate unconditionally, since removed). Bookkeeping is exact: phase 1 visible + candidates equals the unculled visible set (705 + 1615 = 2320; 288 + 139 = 427). Release A (1816x887, Bistro, 2909 instances), occlusion off -> on: fixed camera spot Visibility 0.67 -> 0.49 ms, GPU 3.95 -> 3.81 ms, 252 -> 262 FPS; viewpoint with much hidden geometry Visibility 0.20 -> 0.13 ms, GPU 3.24 -> 2.99 ms, 308 -> 334 FPS. Cost: Hi-Z Build 0.04 ms + phase 2 culling 0.01 ms (the 0.33 ms measured for Hi-Z Build with 2320 instances drawn is the depth flush landing in the first pass that reads depth). Three bugs found while testing: shader binaries were cached under a hash of the shader's own file only, so a layout change in `shaderIO.h` left every shader that includes it running a stale binary (crash on load; the key now covers the includes, `SlangCompiler::sourceHash`); the pyramid lookup scaled the uv by the level size instead of shifting the level 0 index, which misses the object's rectangle on the coarse levels; and it converted NDC to uv without the y flip although the scene is rasterized through a negative-height viewport, so the pyramid was read vertically mirrored and geometry whose mirrored region held nearer surfaces was culled every frame. |

### 5.6 GPU-Driven Culling & Draw Generation

References to consult when this phase starts: Khronos Vulkan tutorial, GPU-driven pipelines (https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/07_GPU_Driven_Pipelines/01_introduction.html — indirect dispatch, multi-draw indirect, GPU-side command generation).

#### 5.6.1 Per-view pipeline (compute)
1. **Instance culling:** frustum, distance/screen-size, Hi-Z occlusion against the previous frame's
   depth pyramid (reprojected), per-view flags (shadow casters, RT-only).
2. **LOD selection** (§5.7): screen-space error → LOD/cluster level.
3. **Compaction:** visible instances written into per-bucket instance-index lists.
4. **Draw commands:** `drawMeshTasksIndirectCount` per bucket
   (opaque, masked, double-sided variants — mirroring today's queues without CPU packing).
5. **Meshlet culling** stays in task shaders (existing cone/bounds culling) on the already-reduced set.

#### 5.6.2 Views
Camera, shadow cascades/local light shadows (where raster shadows are used), DDGI/probe capture,
editor secondary views. All views share instance data; only culling parameters differ.

#### 5.6.3 Transparent geometry
Remains sorted: GPU culling produces the visible transparent set, a CPU or GPU sort orders it,
drawn in the forward pass (behavior unchanged from today's doubleSided-aware transparent queues).

#### 5.6.4 Hi-Z
Depth pyramid built each frame in compute from the visibility pass depth; previous pyramid used for
occlusion with conservative reprojection to avoid disocclusion popping.

### 5.7 Geometry Pipeline & LOD

- **Cook:** meshoptimizer meshlets (as today) + **cluster hierarchy** (Nanite-like DAG): group
  meshlets, simplify with locked group borders, repeat until a small root. Each cluster stores bounds,
  parent error, and simplification error.
- **Runtime:** select the cut through the hierarchy on GPU by screen-space error; seamless transitions
  without per-object discrete LOD pops.
- **Paging:** clusters are packed into fixed-size streaming pages (§5.9); root pages always resident.
- **Fallback path:** meshes that are too small or non-manifold use discrete LODs from `meshopt_simplify`.
- **Ray tracing geometry** uses a coarser, resident fallback LOD per mesh (§5.14). **Tried in 6b-2 and reverted**, see
  §5.7.1 and D8.

#### 5.7.1 Implementation (Phase 6b, September 2026)
| Step | What was built | Result |
|---|---|---|
| 6b-1 | Cluster LOD DAG cooked with meshoptimizer's own builder (`demo/clusterlod.h`, `clodBuild`, implemented in `MeshImporter.cpp`), set up as its `nanite.cpp` demo: 64-vertex / 64-triangle clusters, attribute-aware (normal weights 0.5), every shading seam protected in permissive mode (normals, uv0, uv1 -- the header asks for it; unprotected hard edges came out smoothed), **no sloppy fallback** (it ignores orientation: flipped triangles on flat streets cost no error and were back face culled as holes). Skinned meshes keep one level. Per cluster in `MeshletBounds`: own and parent group bounds + error, LOD level, triangle count. The task shader draws the cut (clusterlod's rule, `nanite.cpp`'s `boundsError` ported) before frustum culling, bounds to world by the instance's largest scale, with the culling view's (frozen) camera. BLAS and "Visible Triangles" use the original clusters only. Cluster stats buffer (drawn clusters / triangles of the visibility passes, the mip feedback pattern), LOD error slider and Full Detail toggle (Render Graph panel), debug view 21 (LOD level). Also: a reimport now only refreshes loaded assets (a model copied in and imported at once was cooked a second time on the main thread, and its 263 .nmat auto-reimported). | **Verified 2026-09-18:** Full Detail identical to before; at 1 px no visible change, LOD level rises with distance. Bistro Release, 1816x948: 2.32M -> 1.40M triangles drawn (-40 %), Visibility 0.49 -> 0.39 ms, GPU frame 3.93 -> 3.82 ms; Geometry used 154 -> 172 MB (LOD levels +18 MB). |
| 6b-2 | *Reverted.* RT fallback per static mesh (the DAG cut at a fixed object-space error, own vertices, the BLAS built from it) plus ray origin offsets where rays leave the raster surface (first the error bound at the pixel's distance, then the drawn cluster's error through the G-buffer). | Pure path tracing sees the BLAS as primary visibility: 5 mm dropped thin detail (holes on the bike and street); the offsets cost contact shadows in every mode. Every fix needed another offset: the mismatch between traced and rasterized geometry is the approach's own problem (UE's Nanite fallback meshes have it too). Reverted to the 6b-1 state: ray tracing uses the original clusters. |

Page streaming (6b-3/6b-4) was paused while ray tracing kept the original geometry resident; it is built since 2026-10-29 with the
BLAS taken from the resident pages instead (§5.7.2).

#### 5.7.2 Geometry streaming (6b-3/6b-4, October 2026)
Nanite's page streaming, with ray tracing following the pages rather than a fallback mesh.

**Cook (GS-1, `MeshImporter.cpp` writePages).** clodBuild's groups go into pages of about 128 KB, whole groups only (a cut never
splits a group). Terminal groups (nothing coarser replaces them: the top of the DAG, and groups whose simplification got stuck)
come first as **root pages**, always resident; the other groups follow coarsest first. A page's **dependencies** are the pages
holding the clusters its groups were simplified into; they always come earlier. Each page has its own vertex block (its clusters
index it through `MeshletDraw::globalVertexOffset`, so the mesh shaders did not change); vertices shared across pages are
copied, ~2.2x the source vertices over all levels, measured on 0.17M and 1.1M triangle test meshes. Per page: its conservative
LOD bounds (a sphere around its groups' simplified bounds, their largest error). Per cluster in `MeshletBounds`: its page and
`refinedPage`, the page of the group it was simplified from. Cooked format v3 (`NSM3`/`NMS3`, `MeshSerializer.h`): cluster records,
page table, dependencies and mesh bounds come first; page data sits in an aligned payload section, one contiguous range per page,
read alone with `VFS::ReadRange` from a loose file or a pack. v2 files still load (no pages, always resident). A v2 cook whose source is
present is cooked again, except one mesh of a split import (Do Not Combine): cooking it alone parses the whole file, which for
Bistro's 551 meshes meant 551 parses, so it loads as it is until the model is reimported.

**Exact cut with residency.** The task shader draws cluster *c* when its page is resident, its parent error projects above the
threshold, and either its own error is below it or its `refinedPage` is not resident. The streamer keeps residency
**dependency-closed**: a page becomes drawable only after its dependencies, and is evicted only when no resident or loading page
depends on it. With that, and error and bounds monotonic along the DAG, every group's "refine" decision is monotonic, so the
surface is drawn exactly once. This is checked offline (scratchpad `gstest`) for 200 random closed residency sets, as the leaf cut
and as the raster cut from random cameras. Full Detail draws the finest resident clusters.

**Runtime (GS-2, `GeometryStreamer`).**
- Loads read the cluster records and root pages (`ReadCookedMesh(..., streamPages)`). The rest is fetched on demand.
- Feedback pass (`GeometryStreamingFeedback.slang`):
  - Runs over every draw-list instance, for the main view and each recorded editor view (viewports, previews). Instances are not
    frustum culled: ray tracing sees behind the camera, so detail streams by distance.
  - A page is wanted once its error passes `GeometryPrefetchFactor` (0.5) × the threshold. It is then resident, and in the BLAS,
    before the raster cut refines into it.
  - Missing pages are requested every frame they are wanted. Resident ones report every 16 frames, which keeps the LRU current.
  - Each page is reported once per frame (`requestFrame`).
- Requests are read back, most wanted first.
- Pool:
  - Size: the Geometry budget share minus the geometry that does not stream, or an override.
  - When full, it evicts pages no page depends on: first those not wanted for 64 frames (least recently wanted first), then wanted
    ones that are clearly less important (by priority, with a 1.25 margin against swapping). A pool smaller than what the view wants
    therefore holds the most important pages, as Nanite's does. Lowering the pool takes effect live, at up to 512 evictions a
    frame.
- Page load:
  1. A read job writes from the file straight into staging.
  2. Copies on the transfer queue go into fresh stream ranges.
  3. The page is **activated by the frame** (`Streaming Update`, after the GPU scene update): its clusters' draw records are
     patched and its resident flag set. Every pass of a frame then sees one residency. A transfer-queue flag flip could tear
     mid-frame or land before its data.
- Evictions clear the flag the same way.

**Ray tracing (D8 resolved for streamed meshes).** A streamed mesh's BLAS holds its **leaf cut**, the finest resident clusters.
After its residency changes (at most every 10 frames), `PrepareBlasBuild` takes the cut when the build is recorded:
- `ClusterIndices.slang` writes the cut's triangles as absolute vertex-stream indices into a new RT index range.
- The build goes into a new structure of the right size; the hit shaders' index range switches in the same frame as the TLAS
  (both the next).
- Evicted pages stay allocated until that BLAS is in use (the hit shaders read their vertices until then).

The leaf cut is never coarser than what the raster draws (the raster cannot draw finer than resident; prefetch covers the rebuild
delay), so `raster_surface_bias` stays the only ray offset. 6b-2's fixed-error fallback is not repeated: near the camera the BLAS
is full detail, far away it is what that distance needs.

**Preserve Area (R9): tried and reverted (2026-10-30).** Simplifying thin, open geometry loses area. A leaf ends as one triangle,
then disappears. On a synthetic bush, level 5 keeps 49.6% of the area and level 10 1.5%.

The attempt was UE5 Nanite's option: after clodBuild, each simplification's open borders were pushed outward until the area of the
group it came from was restored. Positions shared with other groups were never moved, so offline cuts stayed exact and crack free.

Two versions were tested by the user on Bistro with the pool override at 32 MB:
1. **Full restore.** The removed leaves' area went to the survivors: few leaves were left, each up to about 8x a leaf's length.
2. **Growth limit plus error.** A vertex moved at most its leaf's radius, and the push counted as LOD error. The view looked the same,
   but now flashed constantly. The likely cause is that the raised foliage priorities made foliage and other pages evict each other
   in the starved pool; this was not confirmed.

Neither version helped in that test, and the benefit at a budget-sized pool (override 0, where coarse levels are only drawn far away)
was never shown, so the code was reverted. Foliage density at a distance would need a different approach (Nanite's later voxel
foliage, or impostors).

**Not yet:** vertex compression (positions relative to the cluster, octahedral normals: Nanite's pages are ~4x smaller), streaming
skinned geometry (one level, not paged), per-view request priorities. Editor: Geometry Streaming section (enable for meshes loaded
next, stats, pool override for forcing pressure). The stats report's counters say whether streaming is engaged: Streamed Geometry
Meshes, Pages (resident / total / loading), MB, Root MB, Page Requests, Pages Evicted. Debug view 23 colors clusters by their page:
gray for a mesh that does not stream, blue for a root page, green for a streamed page (brighter = finer), cyan for original geometry.
To test: open a scene with streamed meshes, switch to view 23, and fly in. Near surfaces turn green/cyan as their pages arrive. Then
set the pool override low (32 MB): Pages Evicted climbs, and far geometry goes back to blue/coarser.

### 5.8 GPU Memory & Buffer System

This section answers three questions: **which buffers exist**, **who decides how much memory they
get**, and **what happens every frame vs. what stays until it is no longer needed**. The base version
(§5.8.6) is deliberately small; streaming, eviction and defragmentation build on it later without
changing its shape.

#### 5.8.1 Lifetime classes: the rule that decides every buffer
Every piece of GPU data belongs to exactly one class. The class decides the buffer strategy — not the
system that owns the data.

| Class | Lives as long as… | Examples | Storage | Changes when |
|---|---|---|---|---|
| **Asset** | the asset is loaded (later: resident, §5.9) | vertices, meshlet data, RT indices, BLAS, textures | **one large device-local buffer per stream** + range sub-allocator (§5.8.2); textures and BLAS as VMA objects in category pools | load → allocate range · unload/evict → free range |
| **Scene** | the entity/material/light exists | instance records, transforms (current + previous), material table, lights | **large persistent buffers with fixed-size slots** (slot index = GPU id used by shaders) | only **changed slots** are uploaded (§5.5.2) |
| **Frame** | one frame | camera/frame constants, TLAS instance list, visible-instance lists, indirect draw commands, readback requests | buffers **created once and reused**, one copy per frame-in-flight; rewritten every frame | contents every frame; capacity only grows (rarely) at a frame boundary |
| **Render-graph transient** | one frame (logically) | G-buffer, lighting targets, intermediates | render-graph pool; memory aliased between passes (§5.4.3) | acquired/returned each frame, physical memory kept by the pool |
| **History** | the feature is enabled and resolution unchanged | NRD/DLSS/RTXDI history, reservoirs, previous depth/Hi-Z | owned by the feature, registered as graph history resources | recreated on resize/feature toggle |
| **Transfer** | until its copy is submitted | staging data for uploads | **one persistent-mapped host-visible staging ring** | filled and flushed every frame under a byte budget |

**Rule: nothing is allocated or freed in the steady-state frame.** Allocations happen on load, unload,
resize, feature toggle, or when a reused buffer must grow — never as part of drawing a frame.

**Large vs. small buffers.** A few large buffers with sub-allocation are the default for buffer data:
one Vulkan object, one buffer device address per stream (no page tables), trivial GPU-driven indexing,
cheap allocation (a range, not a Vulkan call). Separate objects remain correct when:
- the **memory type differs** (host-visible staging/readback vs. device-local),
- the object **must be its own Vulkan object** — images (textures, render targets) and acceleration
  structures. VMA already places these into large memory blocks internally, so "many textures" does not
  mean "many device allocations",
- it is a **rare, very large one-off** that should return to the driver on its own (e.g. a history
  buffer at 4K).

#### 5.8.2 Unified geometry buffers (replaces `PagedBufferAllocator`)
| Stream (today's pages → unified buffer) | Element | Notes |
|---|---|---|
| `m_vertexPages` → `GeometryVertices` | `shaderio::Vertex` | usage: storage + BDA + AS build input + transfer src/dst |
| `m_meshletDrawPages` → `GeometryMeshletDraws` | `shaderio::MeshletDraw` | |
| `m_meshletBoundsPages` → `GeometryMeshletBounds` | `shaderio::MeshletBounds` | |
| `m_meshletVertPages` → `GeometryMeshletVertices` | `uint32_t` | |
| `m_meshletTriPages` → `GeometryMeshletTriangles` | `uint8_t` | |
| per-submesh `MeshBLAS::indexBuffer` → `GeometryRTIndices` | `uint32_t` | removes one buffer per submesh (2909 for Bistro) |
| (Phase 6b) `GeometryClusters` | cluster hierarchy data | §5.7 |

- **Sub-allocation:** a TLSF range allocator per stream hands out `{offset, size}` with O(1)
  allocate/free and low fragmentation. It only manages numbers — the GPU buffer never changes on
  alloc/free. Candidates: VMA virtual blocks (`vmaCreateVirtualBlock`, TLSF, already vendored — must
  then be wrapped inside NRI, since `vk_mem_alloc.h` pulls in Vulkan headers) or a standalone offset
  allocator. Chosen during Phase 4 design.
- **Shaders:** one base address per stream in the frame constants; everything else (mesh table,
  instance LUT, RT hit shading) stores **offsets, never absolute addresses**. That makes growth safe:
  only the base address changes.
- **Capacity & growth (base):** start at a moderate size (e.g. 256 MB vertices, smaller for the other
  streams), double when an allocation does not fit, up to the geometry category budget (§5.8.3).
  Growth = create larger buffer → GPU copy of the used ranges → swap base address → old buffer through
  deferred release. Happens only while loading, never in a steady-state frame. BLAS are unaffected
  (built acceleration structures do not reference their input buffers).
- **Sparse binding (D5, built 2026-10-30):** each stream reserves its element space as address space (a sparse buffer, up to
  the device's largest buffer and a sixteenth of its sparse address space) and memory is bound to it in pages (64 KB on desktop
  GPUs) as live ranges touch them:
  - `GeometryArena` counts the live ranges touching each page. A page whose count leaves 0 is committed when the range is
    allocated. A page whose count returns to 0 is released when the range is freed, and ranges are freed only once frames are done
    with them.
  - `Device::updateSparseBuffer` allocates each page's memory from VMA (it shows in the heap statistics) and binds it with
    `vkQueueBindSparse` on the transfer queue (or the graphics queue when the copy engine cannot bind sparse memory).
  - Every binding waits for the previous one and signals the next value of a timeline. The upload submissions and every frame
    submission wait for its newest value, so copies and GPU writes into new pages come after their memory. A released page's
    memory is freed once its unbind completed.
  - The buffer never moves and nothing is copied. The grow-copy, its full GPU wait when graphics-written streams grow, and the
    doubled capacity are gone. The Geometry category's committed bytes are now the committed pages.
  - Devices without `sparseBinding` + `sparseResidencyBuffer` (or without a queue that binds sparse memory) keep the grow-copy path.
  - Offline test against a stub device: random allocate/free sequences on four element sizes, with the bound pages checked to be
    exactly the pages the live ranges touch. No page is committed or released twice, and nothing is left bound at the end.
  - **Measured on Bistro (2026-10-30, RTX laptop GPU: sparse address space 1 TB, 64 GB reserved per stream at most):** Geometry
    went from 621 MB committed / 328 MB used to 319 MB committed / 317 MB used. That leaves 2 MB of page rounding, and saves
    302 MB of VRAM. The ~5000 page allocations are suballocated from VMA's blocks (6126 allocations in all, against 1073 before).
    Still to confirm: the path tracer, geometry streaming under a small pool, back-to-back scene loads, and a Debug run with
    validation.
  - The geometry streaming pool for cluster pages (§5.7.2) is a byte budget inside these streams, not a separate fixed buffer.
- **Freeing:** ranges return to the allocator after frames-in-flight (§5.8.4). Unlike pages, freed
  memory is immediately reusable by the next load.
- **Vertex compression (R7, built 2026-10-30, Nanite-style packing):** the vertex stream holds `shaderio::PackedVertex`, 36 bytes
  instead of 88.
  - **Format:** the position stays a float3 at offset 0 (BLAS builds read it directly with the packed stride), and the UVs stay
    floats (tiling UVs run far past 0..1). The normal is octahedral 16:16 unorm. The tangent is octahedral 15:15 with the bitangent
    sign in bit 31.
  - **Bone influences:** they left the vertex. A skinned mesh's go to a skin stream (`shaderio::SkinVertex`, 16 bytes: 16-bit bone
    indices, 16-bit unorm weights renormalized to sum to 1). It mirrors the vertex stream's element ranges
    (`GeometryArena::Reserve`, sparse pages), so a vertex's influences are at its own index. Static geometry carries none of the
    32 bytes per vertex it used to.
  - **Shaders** read through `unpackVertex` / `loadMeshVertex` (bone influences for the instances they skin) and
    `LoadRayTracingVertex`. Most hit shaders read only `.uv0`, which kept its name and type in the packed vertex.
  - **CPU and cook:** the CPU keeps working on `shaderio::Vertex` (import, cluster DAG, HLOD bake), packing at the boundaries:
    the cook, where cooked format version 4 stores packed vertices and the page payloads are read into staging as they are, and the
    upload. Version 3 cooks still load: their inline vertices are read as before, and their streamed pages are packed by the read
    job (`GeometryStreamer`), so no re-import is needed. A re-import writes version 4, smaller on disk and per page read.
  - **Offline test:**
    - Precision over 200,000 directions: 0.034° at most for the normal and tangent, 1.5e-5 for bone weights.
    - Version 4 round trip: positions and UVs exact.
    - A version 3 file written by the previous commit's code reads back byte-identical, and its streamed page packs exactly as a
      version 4 page.
    - The cooked test mesh is 46% smaller.
  - **Measured on Bistro (2026-10-30):**
    - Geometry went from 319 MB committed / 317 MB used to 161 / 159 MB (-49%), with the same image.
    - The streamed pages' resident share went from 243 to 110 MB; the root pages from 44 to 19 MB.
    - A re-import as version 4 shrank the project folder from 4.21 to 4.03 GB: the 180 MB saved are the meshes, and the textures
      are most of the rest.
- **Vertex indexing (2026-10-30):** the cook now starts as meshoptimizer's pipeline does, with `meshopt_generateVertexRemap` (in
  `indexVertices`). It welds vertices identical in every byte (exporters split them per face, primitive or attribute set) and
  drops vertices no triangle uses, before the cluster LOD (`buildClusterLod`) or the one-level clusters (`buildMeshlets`).
  - For the cluster LOD, a duplicate vertex is also a border the simplifier must keep: welded, it simplifies across it.
  - The one-level path also gets the vertex cache and vertex fetch orders (`meshopt_optimizeVertexCache`,
    `meshopt_optimizeVertexFetch`), so a cluster's vertices lie close in memory.
  - Until then only the per-cluster steps ran (`meshopt_buildMeshlets`, `meshopt_optimizeMeshlet`, bounds, tangents). The log
    reports each cook's welded count.
- **Vertex colors (I1, built 2026-10-30 and tested by the user, UE's `FColorVertexBuffer`):** glTF COLOR_0 (float, normalized byte or short; RGB or RGBA)
  multiplies the base color, alpha included, as the glTF spec says.
  - **Where they live:** an optional color stream, `m_colorStream`, one packed RGBA8 per vertex: RGB sRGB-encoded (dark colors
    0.0007 apart rather than 0.002), A linear. Like the skin stream, it mirrors the vertex stream's element indices, and only a mesh
    with colors reserves (and with sparse buffers, commits) its range.
  - **The cook:** `shaderio::Vertex::color` (white by default) and `MeshData::VertexColors`. Combining submeshes keeps colors if any
    part has them. The simplifier gets the color as attributes 19-22: weighted 0.5 like the normal, and color seams protected.
  - **Cooked format v5 (`NSM5` / `NMS5`):**
    - Each mesh gets a flags word.
    - One-level meshes store their colors after the bone influences.
    - Pages carry a color block after the vertex block, marked by `MeshPage::Colors`.
    - Version 4 cooks still read, without colors. Versions 2 and 3 are no longer read: they stored the old 88-byte vertex. With
      their source present they are cooked again; without it the importer logs that the mesh must be imported again.
  - **Runtime:** the upload stages the colors. `GeometryStreamer` reserves the color range of each page it loads and copies its color
    block there. `GpuMesh::flags` (`GpuMeshVertexColors`) tells raster shaders (`InstanceData::vertexColors`) that the mesh has
    colors; for ray tracing, `GpuRayTracingInstance::colorBufferAddress` is 0 when it has none.
  - **Shaders:** `loadMeshVertexColor`, `RayTracingVertexColor`, `RayHitAlphaTested` and `RayHitAlpha` (`shaderIO.h`) apply the
    color in every base color evaluation and every alpha test:
    - raster: G-buffer resolve, the forward PBR, unlit and transparent passes, and the visibility, selection and stable-depth alpha
      tests;
    - ray traced: hit shading for reflections and GI, the screen-probe and DDGI rays, the path tracer, and ReSTIR DI/GI/PT;
    - shadow and AO rays: their alpha tests.
  - **HLOD bakes** multiply the colors into the baked base color.
  - **Tested offline (scratchpad `gstest/colortest`):**
    - Packing error is at most 0.0043.
    - v5 round-trips colors on paged and one-level meshes, read whole and streamed.
    - A page read alone, as the streamer reads it, holds the right color block.
    - A red/blue seam is crossed by none of 102,263 triangles over all LOD levels.
    - A v4 file still reads; a v3 file is refused.
- **Morph targets (I1, M1 built 2026-10-31 and tested by the user; UE's morph targets / Blender's shape keys):** glTF primitive targets (POSITION and NORMAL
  deltas; TANGENT deltas are left out, the tangent follows the base shape).
  - **Import:** names come from the mesh's `extras.targetNames` and default weights from its `weights`. Sparse accessors, which
    exporters commonly use for targets, are now read as glTF specifies (`ReadAccessorFloats`).
  - **The cook:** `MeshData::Morphs` holds dense per-vertex deltas that follow their vertices through every step:
    - welding (`meshopt_generateVertexRemapMulti` with the delta streams, so vertices with different deltas stay apart);
    - vertex fetch order (`meshopt_optimizeVertexFetchRemap`);
    - tangent-seam duplicates (`GenerateMissingTangents` reports their origins);
    - import scale;
    - combining: a morphed submesh stays its own group, its deltas transformed like its vertices.
  - A mesh with morph targets is one level, like skinned geometry: no cluster LOD, since a simplified surface would move wrongly.
  - **Cooked format v6 (`NSM6` / `NMS6`):** after a one-level mesh's triangles, each target's name, default weight and sparse
    deltas (vertex, position, normal). v5 and v4 cooks still read, without targets.
  - **GPU:**
    - A `GpuMorphRange` per vertex in `m_morphRangeStream` (mirroring the vertex stream, reserved only by morphed meshes) points
      at that vertex's run of `GpuMorphDelta`s (target, position, normal) in `m_morphDeltaStream`.
    - `ComputeSkinning.slang` adds the weighted deltas before the bones (Unreal's order), so morph-only meshes go through it too
      (no bones: identity).
    - The deformed vertex cache then feeds every raster pass, ray tracing and motion vectors. Forward buckets now join the compute
      pass, and `Material_PBR_Mesh.slang` reads the cache too, so transparent and unlit surfaces deform as well.
  - **Weights:**
    - `MeshComponent::MorphWeights` holds the entity's weights by name; a target it does not name keeps its mesh's default.
    - They're handed over every frame (`Renderer::SetMeshInstancesMorphWeights`) into a per-frame buffer like the bone matrices; an
      instance nobody set gets its defaults.
    - The inspector's Mesh component has a Morph Targets section: one slider per target, plus Reset to Defaults.
    - They're saved with the scene. Entities that have them use the text path in cooked world-partition cells.
  - **Offline test (scratchpad `gstest/morphtest`):**
    - Sparse accessors read correctly, both over zeros and over dense data.
    - 9,600 unwelded vertices weld to the grid's 1,681, every delta still on its vertex.
    - Vertices that differ only in their deltas are not welded.
    - Tangent-seam duplicates report the right origins.
    - v6 round-trips the deltas exactly; a v5 file still reads.
    - The GPU's per-vertex runs equal the dense weighted sum for random weights.
  - **M2 (built 2026-10-31): animated weights.**
    - **Clips:** glTF `weights` channels become `AnimationSequence::MorphCurves`, a curve per target named like the mesh's targets
      (LINEAR, STEP; CUBICSPLINE keeps the values and drops the tangents; values may be normalized integers). `.nanim` gets a new
      magic, `NAN2`, with the curves; old `NANM` files read without them.
    - **Poses:** `AnimPose::Curves` (UE's animation curves) are sampled with the clip (`SampleClipPose`) and go through every blend:
      lerp and weighted by name (a target missing from a pose counts as 0); Layered Blend per Bone, where the blended pose's
      curves win; additive, as base + (additive − reference) × alpha. Graph nodes, blend spaces and montages carry them.
    - **Animator:** single-clip mode and node animation sample the played clip's curves (`Animator::GetMorphWeights`).
    - **Routing:** `Scene::UpdateAnimator` writes the weights to `MeshComponent::AnimatedMorphWeights` each frame. A node
      animation's go to the channel's node entity when that entity has a mesh; everything else (a skeletal mesh's face shapes)
      goes to the animator's own entity. A clip without curves clears the entity's animated weights.
    - **Sequencer:** a Morph Weight track property, with the target name in `TimelineTrack::Parameter`. The add-track menu lists
      a mesh's targets. Tracks write `SequencerMorphWeights`; preview snapshots and restores them.
    - **Script API (version 11):** `Nox.MeshComponent` with `SetMorphWeight`, `GetMorphWeight` (the value shown now) and
      `ClearMorphWeight(s)`.
    - **Precedence per target:** the mesh's default < animation < sequencer < set by hand or a script (UE's `SetMorphTarget` wins
      over the animation).
    - **Offline test (scratchpad `gstest/morphanimtest`):**
      - The importer's own `ParseAnimationsFromGltf`, on a hand-built glTF, makes a named curve per target, LINEAR as well as
        CUBICSPLINE (values, not tangents).
      - Sampling, the Animator's weights, and all four pose blends give the expected values.
      - NAN2 round-trips; NANM reads without curves.
  - **Clips of a static mesh imported per mesh (built 2026-10-31):** a Do Not Combine import without Import Into Level used to
    drop the file's clips, so a model like AnimatedMorphCube had nothing to play.
    - **Cooking:** each mesh now gets its own copies, `Meshes/<Mesh>_<Clip>.nanim`, the clip moved onto the mesh asset's nodes
      (`RetargetClipToMesh`):
      - **Node 0, the node a plain drag places:** the animation relative to the mesh node's rest pose, so it plays where it is put.
      - **The file-layout nodes:** world space, under the mesh's resting parents.
      - **Morph curves:** pointed at both nodes.
      - **Resampling:** channels are resampled at their key times (STEP stays STEP, everything else becomes LINEAR).
    - **Linking:** the cook writes `<Mesh>.nsmesh.clips`, the exact list of clips it wrote. `LinkImportedAssets` links those
      instead of matching names by prefix (a mesh named `Cube` would otherwise also take `Cube_Top`'s clips).
    - **Spawning:** `ModelInstance::addAnimators` now counts morph curves as a clip's targets. Clips with only weights (in any
      import) used to get no animator. The owner is the first node of the clip that was placed.
    - **Limitation:** an animated parent of the mesh node is held at its rest pose. Import the whole file, or Into Level, to keep
      the hierarchy's motion.
    - **Offline test (scratchpad `gstest/retargettest`):**
      - A door swinging under a rotated hinge: node 0 starts at identity and moves relative to its rest; the layout node matches
        the hinge's world times the door's local transform (error 1e-7).
      - The hinge's own channel is dropped.
      - A clip of weights only keeps its curves, on both nodes.
- **Material variants (I1, built 2026-10-31; glTF `KHR_materials_variants`, three.js / Babylon's `selectVariant`, a Variant
  Manager switch in Unreal):** one model, several named material sets (a shoe in three colors).
  - **Cook:**
    - The file's variant names (root extension) and each primitive's mappings become `MeshData::MaterialVariants` on the
      submesh: variant name, its index in the file, the material.
    - They travel with the submesh through the static/skeletal split, Do Not Combine copies and Import Scale.
    - Combining keeps a submesh with variants apart from one without (or with others), or a variant would recolor both.
    - Every variant material gets its `.nmat`, written once like the others.
  - **Material names:** a `.nmat` is named after its material, and exporters reuse names (all three of Khronos'
    MaterialsVariantsShoe materials are `phong1SG`). A material named like an earlier one of the file now gets its index
    (`phong1SG_1`; `GltfMaterialName`). Before, such materials silently shared one `.nmat` and could merge when combining.
  - **Format v7** (`NSM7` / `NMS7`): every mesh record ends with its variant mappings. Versions 4 to 6 still read, without
    variants (version from the magic's digit, `MagicVersion`).
  - **Asset:**
    - The mesh asset keeps the mappings per submesh (`GetMaterialVariants`).
    - `EditorAssetManager::ImportMeshMaterials` links each mapping's `.nmat` (`GetVariantMaterialAssets`), with the same lookup
      as the submesh materials.
    - `ImportMeshTextures` registers the textures of the variant materials too. Found in the editor: the shoe's beach and street
      drew untextured white-gray, because their base color images are used by no submesh's own material.
    - Packaging takes them like the other materials of the model (owned by name).
  - **Selecting** (`Scene/MaterialVariants`):
    - **What it writes:** a variant is the whole file's, so it covers the entity and everything below it (a model instance's
      root switches all its nodes). `Select` writes the `MaterialComponent` slots of those meshes: the variant's `.nmat`, or 0
      (the mesh's own) where the variant has none for that slot.
    - **What that buys:** it is saved, undone, kept as a model instance's node override, baked by HLOD and drawn like any other
      material override, with no new state.
    - **Reading it back:** `Current` works out which variant the slots show (or Default, or Custom when set by hand).
    - **Undo:** the history fingerprint also hashes material slots, so a variant chosen on a parent is one undo step.
  - **Editor:** a Material Variant combo at the top of the Properties panel for any entity with variants below it (Default,
    the variants, and Custom shown when the slots were set by hand).
  - **Script API (version 12):** `Entity.SetMaterialVariant(name or index)`, `ClearMaterialVariant()`, `MaterialVariantCount`,
    `MaterialVariantIndex` (-1 Default, -2 Custom).
  - **Offline test (scratchpad `gstest/variantstest`):**
    - The importer's own parsing on Khronos' MaterialsVariantsShoe: midnight / beach / street map to `phong1SG`,
      `phong1SG_1` and `phong1SG_2`, three different `.nmat` files.
    - Combining three submeshes, the middle one with a variant, gives two groups.
    - v7 round trip, full and streamed, paged and one-level, static and skeletal kinds.
    - A v6 cook reads without variants; v3 is refused.
- **glTF scenes (I1, 2026-10-31):** a file may hold several scenes. Like Unreal and Godot, Nox imports one: the file's default
  (`scene`), else the first. There is no scene picker, by decision: neither engine offers one.
  - **Before:** every node of the file was imported, so another scene's meshes, lights and cameras came along.
  - **Parse:** `NodesInScene` marks what the imported scene reaches. The other nodes keep their slot in the node table, so indices
    stay the file's (skins, clips and lights point at them), but they get no geometry (their meshes are not even read), their
    parent becomes `MeshNodeData::OutsideScene`, and their lights and cameras are dropped.
  - **Placement:** model instances and Import Into Level skip such nodes. The import dialog's size estimate already used the
    default scene; the mesh-kind check now does too.
  - **Offline test (scratchpad `gstest/scenestest`):** default scene 1, no default (first scene, whole subtree), an invalid
    default, and no scenes at all (every node).
- **glTF extras (I1, built 2026-10-31):** a node's extras (Blender's custom properties) become a `MetadataComponent` on its entity.
  Godot keeps them as node metadata, Unreal as user-defined attributes.
  - **Format:** key / value text, in the file's order. An object's members become one pair each: a string as it is, anything else
    as compact JSON (`Health` = `50`, `Tags` = `["a","b"]`). Extras that are not an object become one entry named `extras`.
  - **Where they go:**
    - Cooked into the node table (format v8). Per-mesh assets carry their node's extras.
    - Model instances, flat placement and Import Into Level put them on the node entities. A node without a mesh that carries extras
      or physics keeps the model instance's hierarchy (flat placement would drop it).
  - **Editing:** saved with the scene and editable in Properties (Metadata: key, value, add, remove).
  - **Scripts (API 13):** `Entity.GetMetadata(key)` (null when missing), `HasMetadata`, `GetMetadataFloat`, `SetMetadata`,
    `RemoveMetadata`. A value comes back as UTF-8 into a buffer the script side allocates, so no native string crosses back.
  - Mesh and material extras are not imported.
- **glTF physics (I1, built 2026-10-31; `KHR_physics_rigid_bodies` with `KHR_implicit_shapes`, both Release Candidate):** Godot
  imports physics bodies from glTF (OMI_physics_*, with an importer for the KHR extensions). Unreal does not import glTF physics.
  - **Mapping onto Nox's components** (`NodePhysics` in the node table, format v8):
    - A **motion** becomes a Dynamic `RigidBody3DComponent` (Kinematic with `isKinematic`, or with mass 0, the spec's infinite
      mass), with its mass and gravity factor.
    - A **collider** becomes a Box / Sphere / Capsule Collider 3D: box half extents, sphere radius, capsule radius and half height
      (the spec's height is between the cap centers). The physics material gives dynamic friction (else static) and restitution.
    - A **trigger** becomes a sensor.
    - Sizes go through Import Scale like the vertices.
  - **Bodies:**
    - A collider under a motion is part of that body. A collider with no motion above it gets a static body of its own (the spec's
      static colliders).
    - A trigger gets a static sensor body; one under a moving body is left out, since Nox's sensors are whole bodies.
    - Decided from the motions alone, so the order of the nodes does not matter.
  - **Compound bodies (new in `JoltPhysics3DScene::CreateBody`, Godot's collision shapes under a body, Unreal's attached shape
    components):**
    - Colliders on descendant entities without a body of their own are added to the body as a `StaticCompoundShape`, each at its
      position and rotation relative to the body and sized by its own world scale. The spec puts offset or rotated shapes on child
      nodes, and so does Blender's exporter.
    - The body's friction and restitution come from its own collider, else its first part's.
  - **Mass:** `RigidBody3DComponent::Mass` 0 now means "from the colliders' volume" (Jolt's density of water, Unreal's default
    1 g/cm³), which is what the spec asks when a file gives no mass. The Properties panel shows "from volume".
  - **Left out:** what Nox has no component for is left out and named once in the log (`glTF physics: no Nox equivalent, left
    out: ...`): cylinder and plane shapes, mesh and convex hull colliders, joints, collision filters, compound triggers, initial
    velocities, a set center of mass or inertia. Tapered capsules come in straight, with the larger radius.
  - **Per-mesh assets** carry their node's own body. Colliders on child nodes stay with the whole file (Import Into Level or the
    whole-file asset).
  - **Offline test (scratchpad `gstest/physicstest`):** the importer's own `FlattenExtras` and `ReadNodePhysics` on a hand-written
    file with every case:
    - Extras of each JSON kind.
    - A dynamic crate with a box and a child capsule.
    - A static ground with a sensor zone under it.
    - A kinematic body, and mass 0 read as kinematic.
    - Everything left out, named.
    - The v8 node table round trip; a v7 cook still reads.

    The compound body in Jolt is only compile-checked.
- **Every glTF physics feature (I1, 2026-10-31):** the rest of `KHR_physics_rigid_bodies`, with the engine components it needed
  (Unreal's and Godot's physics have them all).
  - **New colliders:**
    - **Cylinder Collider 3D:** with a bottom radius it becomes a cone or frustum (Jolt Cylinder / TaperedCylinderShape).
    - **Plane Collider 3D:** unbounded is Jolt's PlaneShape (a half-space, Godot's WorldBoundaryShape); with a half size it is a
      flat slab.
    - **Mesh Collider 3D:** a mesh asset's triangles, or with Convex their hull (Unreal's complex / simple collision).
      - It reads the cooked file's LOD-0 cluster triangles (welded, as HLOD does) once per physics session, cached per mesh.
      - A dynamic body always takes the hull, since Jolt gives triangles no mass.
      - "Use This Entity's Mesh" fills it from the entity's own Mesh component.
    - **Capsule Collider 3D:** gains Bottom Radius (Jolt TaperedCapsuleShape).
    - Cylinder and plane draw in the viewport like the others.
    - A collider added or removed during Play rebuilds the body it belongs to (its own, or the nearest one above).
  - **Physics Joint 3D** (Unreal's physics constraint, Godot's Generic6DOFJoint, Jolt SixDOFConstraint):
    - It joins the body at or above its entity to the one at or above Connected Entity (or the world). The frames are the two
      entities' transforms.
    - Per axis (move X/Y/Z, turn X/Y/Z): Free, Locked or Limited, with soft translation limits.
    - Drives: a position target with a spring, else a velocity, with an optional maximum force.
    - Collide With Each Other off keeps the pair apart.
    - Constraints are rebuilt before the next step whenever a body or joint changes. They are removed before a body goes, since
      Jolt holds pointers to their bodies.
  - **Collision Filter 3D:**
    - Fields: Systems, Collide With, Not Collide With (the spec's names).
    - It is enforced by a Jolt group filter: every body's group is its filter, its sub group the body. The same filter keeps
      jointed pairs apart.
    - The spec's rule, both sides agreeing. A Collide With list admits nothing else, a body in no system included; Not Collide With
      never excludes a body in no system.
  - **Import:**
    - Shapes: cylinders and cones, finite and unbounded planes, tapered capsules.
    - Mesh and convex colliders are matched to the asset's submeshes of that glTF mesh at cook time. Import Into Level uses the
      mesh's own asset.
    - Collision filters, and joints: limits per axis, translation scaled with the import, drives.
    - Format v9 holds them in the node table. Per-mesh assets drop joints, whose other end is another node of the file.
  - **Still left out (named in the log):**
    - Initial velocities, a set center of mass or inertia, compound triggers, triggers on moving bodies.
    - Simplified on the way in: limits on several axes at once are limited per axis; soft angular limits are hard;
      acceleration-mode drives are driven as force.
  - **Mesh / material extras:** not imported, by decision. Assets have no metadata a script or the editor reads, and node extras
    carry Blender's object properties.
  - **Offline tests:**
    - `gstest/physicstest`, extended: cylinder, finite plane, tapered capsule, convex mesh collider, a locked + limited joint with
      an angular drive, a collision filter; v9 round trip.
    - `jolt/filtertest` with real Jolt: the filter rules, jointed pairs kept apart, and a filtered box falling through a floor an
      unfiltered one rests on.
    - Joint constraints and mesh collider shapes in a running scene are compile-checked only.
- **Material lobes (M1, built 2026-10-31; glTF `KHR_materials_clearcoat`, `_sheen`, `_specular`, `_iridescence`, `_anisotropy`,
  `_dispersion`):** the lobes past the core metallic-roughness model, one slab as UE5 Substrate's: every lobe in the same material
  at once, each costing nothing where it is off. The math is each extension's spec, checked against the Khronos glTF Sample
  Renderer (`pbr.frag`, `brdf.glsl`, `iridescence.glsl`).
  - **Data:**
    - `MaterialData` has each extension's factors and textures (`MaterialTexture`: path and UV set); `ReadGltfMaterial` reads the
      members, a missing one keeps the spec default.
    - `.nmat` (magic `NMA2`) and a cooked mesh's material list share one writer, `MaterialSerializer::WriteFields`. The cooked
      list had dropped IOR, volume, diffuse transmission and depth bias before; it carries every field now.
    - **Format v10** (`NSM:` / `NMS:`, the magic's character is `'0' + version`), **v11** with the UV channels. Only the current
      version is read: an older cook is cooked again from its source, an older `.nmat` (now `NMA3`) is logged (delete it and the
      model's import writes it again).
    - `GpuMaterial` carries the lobes and `lobes`, the `MaterialLobe*` bits (`PackMaterial`: a lobe is on when its factor is;
      the specular bit also for an IOR other than 1.5). Ray tracing records link their material (`materialIndex`, `lobes`).
    - The lobe textures are imported with their color space (sheen and specular color sRGB, the coat normal BC5) and kept loaded
      with their materials; the G-buffer reports their mips to texture streaming.
  - **G-buffer** (`GBufferMaterial.slang`, `MaterialLobes.slang`'s `evaluate_material_lobes`):
    - The material target's blue holds the pixel's lobe bits and the unlit bit (`gbuffer_encode_flags`, `gbuffer_unlit`).
    - Two targets: `GBuffer Lobes` (RGBA32_UINT: coat normal octahedral 16:16; coat factor, coat roughness, sheen roughness,
      anisotropy strength; sheen color and the anisotropy direction; iridescence factor, IOR and thickness) and
      `GBuffer Specular` (RGBA8: the dielectric F0's square root and the specular weight). Readers load them only for pixels with
      lobe bits (`gbuffer_load_lobes`).
    - The anisotropy direction is an angle against an orthonormal basis of the stored normal (Duff et al. 2017), rebuilt exactly
      by the reader from the same half-float normal.
    - The coat darkens the emission by its Fresnel (the spec's coated emission).
  - **Shading** (`PBRLighting.slang`): `evaluateDirectLightLobes` and `evaluateRectLightLobes` for lights, `lobe_ambient` for the
    environment, GI and reflections, in the deferred lighting, the forward pass (transparent surfaces, the editor's extra views)
    and ray hits (`RayHitShading.slang`: what reflections and GI rays see).
    - Specular: F0 from the IOR tinted by the specular color (at most 1), times the weight; F90 the weight.
    - Iridescence: the thin film's Fresnel (Belcour & Barla) at the view angle, the metal's and dielectric's mixed by metallic, in
      place of the base Fresnel; below it the diffuse by its largest channel (the spec's `rgb_mix`). The split sum takes its F0
      (`schlick_to_f0`). A film of 0 nm is none (as the reference renderer).
    - Anisotropy: Burley's anisotropic GGX and its height-correlated visibility; the environment along the spec's bent normal.
    - Sheen: Conty & Kulla's Charlie distribution and visibility; what is below scaled by `1 - max(sheen color) x E`, E the
      sheen's directional albedo in the BRDF LUT's blue (`BRDFIntegration.slang`, now RGBA16F); the environment at the sheen's
      roughness as Filament does.
    - Clear coat: GGX with the coat's normal and roughness at IOR 1.5; everything below weighted by `1 - coat x F(N_c.V)`
      (`fresnel_coat`). Rect lights give the coat its highlight by the same representative point.
  - **Reflections:** one ray per pixel traces the *reflection layer* (`gbuffer_reflection_layer`, UE's clear coat rule): the coat's
    roughness blended over the base's by the coat factor, the normal likewise. The NRD and DLSS-RR guides describe the same layer;
    the RR specular albedo includes the coat's. The deferred lighting gives the traced radiance to each layer by its own
    roughness, so a smooth coat over a rough base reflects the traced scene.
  - **Dispersion:** refraction per color channel at the spec's IORs (`dispersion_iors`), in the screen-space and the traced
    refraction of the forward pass.
  - **Normal maps facing away:** a normal-mapped normal turned away from the camera is bent back to just facing it
    (`lobe_facing_normal`) for the coat and the reflection ray; the reflection pass returned nothing there and a coat (Fresnel 1
    at that angle) covered the pixel with black (ClearCoatTest's shared-normal row). Anisotropic surfaces' reflection rays sample
    the anisotropic GGX; ray hits build their tangent frame (coat normal map, anisotropy).
  - **Path tracer** (`PathTracer/StandardBSDF.slang`, `RTXPT/BxDF.hlsli`): RTXPT's specular lobe gained the anisotropic GGX (VNDF
    sampling, D, height-correlated G in the shading frame whose T is the stretch direction), F90 (the specular weight) and the
    thin film in place of its Schlick Fresnel; the isotropic path is unchanged. The sheen (Charlie, cosine-sampled) and the clear
    coat (GGX around its normal) are layers over RTXPT's BSDF: one-sample MIS over the layers by their shares, delta lobes of the
    base keep what the layers let through. Dispersion: at its first dispersive refraction a path takes one color channel (a third
    of the time each, its throughput that channel times 3) and refracts at that channel's IOR, as every later dispersive surface
    does for it. The pure lobe math is one module (`MaterialLobeMath.slang`) for the raster and the path tracer.
  - **Not yet / simplified:** ReSTIR DI's direct lighting mode shades the base lobes only (the user's decision: ignore for now);
    the specular weight does not change the transmission's Fresnel (the spec lets it); the path tracer's film takes the base's
    combined F0 (exact for metallic 0 or 1).
  - **Offline tests:**
    - `lobetest/lobetest`: the importer's `ReadGltfMaterial` on 8 Khronos sample assets (ClearCoatTest, ClearCoatCarPaint,
      SheenChair, SpecularTest, IridescenceLamp, AnisotropyBarnLamp, AnisotropyRotationTest, DispersionTest: 89 materials, every
      member against the files' JSON), `.nmat` and v10 round trips of every field, older files refused.
    - `slangcpu/lobetests`: the shader functions themselves compiled to C++ (`slangc -target cpp`). Charlie and anisotropic GGX
      distributions integrate to 1; the anisotropic visibility equals the isotropic one at equal roughness (2e-7); white furnace:
      the coat and anisotropy never gain energy over the base; the iridescence Fresnel equals the reference renderer's to 7e-8
      and an exact spectral thin film (Airy reflectance, CIE 1931 observer) to 0.011; the sheen LUT within 1e-4 of a
      2048 x 2048 integration; G-buffer encodings: octahedral 0.03 degrees, anisotropy direction 0.35 degrees, values 0.002.
    - `slangcpu/bsdftest`: the path tracer's `StandardBSDF` (RTXPT's lobes and the layers) compiled to C++, 400k samples per case
      (plain, clear coat, sheen, anisotropic metal, specular 0.5 with a film, coat with sheen, at N.V 0.9 and 0.3): the mean
      sample weight equals eval integrated over the sphere (within 0.003), the pdf's integral equals the share of samples taken
      (0.79 for the rough anisotropic lobe, whose rest goes below the horizon), no case above a white furnace of 1.
    - The shading in a running scene is compile-checked only.
- **Texture transforms (I2, built 2026-10-31; glTF `KHR_texture_transform`, Unreal's per-texture UVs):** a material has four UV
  channels (`MaterialData::UVChannels`): a mesh UV set and its transform (offset, rotation, scale: `uv' = T x R x S x uv`). Every
  texture reference's set and transform become a channel at import (textures sharing both share one; a fifth distinct one is
  logged and shares the nearest), the texture reading it (its Set). The shaders transform each texture's UV and gradients where
  they sample it (`instance_texture_uv`, `material_texture_uv`): G-buffer, forward, alpha tests, lobes, ray hits and the path
  tracer (its mip requests and footprints too); the hybrid passes that only read a base color (GI, ReSTIR) take the base color's
  channel from the ray tracing record. glTF's `normalTextureInfo.scale` (the coat's too) and `occlusionTextureInfo.strength` are
  read and applied. Khronos' SheenChair needs three channels (base color 7x with an offset, normal 2x, occlusion on UV set 1);
  `lobetest` checks those, ClearCoatCarPaint's 0.2 normal strength and 3x tiling, TextureTransformMultiTest, and the rows against
  the spec's matrix product.
- **Defragmentation (later, only if measured):** move live ranges and patch the mesh table's offsets.

#### 5.8.3 Budgets: who decides how much
Budgets are **top-down policy**, allocations are **bottom-up requests**. No system guesses sizes and no
system negotiates with another.

```
VK_EXT_memory_budget ── device-local heap budget (what the OS lets us use right now)
        │  minus safety margin
        ▼
Engine VRAM budget ── split by settings/quality preset into category budgets
        │
        ├─ Geometry  (unified buffers, later cluster pool)
        ├─ Textures  (VMA texture pool, later mip streaming)
        ├─ RayTracing (BLAS, TLAS, RT scratch)
        ├─ Scene     (instance/transform/material/light buffers)
        ├─ Transient (render-graph pool + history)
        └─ Headroom  (driver, swapchain, Streamline/DLSS, spikes)
```

1. **The GPU reports the total.** `VK_EXT_memory_budget` gives the device-local heap budget (on an 8 GB
   card, somewhat below 8 GB depending on the OS and other applications). Polled every few frames.
2. **Settings split it.** Category shares are configuration (Open Decision D10, set from Phase 0
   measurements), e.g. textures 40 %, geometry 20 %, RT 15 %, scene 5 %, transient 10 %, headroom 10 %.
3. **Consumers request exact sizes.** The loader knows every size from the cooked file header and asks
   its category: "geometry: 48 MB vertices, 6 MB meshlets", "textures: 22 MB image".
4. **The category accountant decides.** Each category tracks committed bytes (for unified buffers:
   their capacity, not just used ranges).

| Situation | Base behaviour (Phase 4) | Streaming behaviour (Phase 6) |
|---|---|---|
| Fits in category budget | allocate | allocate |
| Over category budget, total VRAM still free | allocate, category shown as over budget in Nox Stats + log warning | residency manager evicts lowest-priority/least-recently-used data of that category, then allocates |
| Real device out-of-memory | load fails cleanly with an error, asset stays unloaded | quality scaling: drop texture mips, coarser geometry cut, fewer RT instances |

The base is a **soft budget**: it measures and warns but never breaks editing. Enforcement arrives with
the residency manager (§5.9), which needs streamable content (mip tails, cluster pages) to have
something to evict.

#### 5.8.4 Deferred release
One `DeferredReleaseQueue` for every GPU object and range: buffers, images, acceleration structures,
geometry ranges, scene slots, bindless heap slots. Entries carry the frame index at release; the main
thread frees them at the frame sync point once all frames-in-flight that could reference them have
completed. Generalizes today's `m_deferredAssetReleases`, `processDeferredMeshFrees` and texture-slot
invalidation into one mechanism.

#### 5.8.5 What happens every frame vs. what stays
```
Frame N (main thread + frame graph, §5.3)
 ├─ Sync point        complete finished uploads/readbacks · free DeferredReleaseQueue entries that
 │                    are older than frames-in-flight · poll memory budget (every few frames)
 ├─ Game Update       ECS changes → dirty flags (transform, material, light, instance add/remove)
 ├─ Streaming         (Phase 5+) finished loads get ranges/slots · residency picks loads/evictions
 ├─ Render Prepare    upload dirty scene slots only · write frame constants · TLAS instance list
 │                    (all through the staging ring, byte-budgeted)
 ├─ GPU (per view)    culling writes visible lists + indirect commands into reused frame buffers ·
 │                    render-graph passes use pooled transients · history resources swapped
 └─ Present           transients returned to the pool; nothing allocated, nothing freed
```

| Stays until no longer needed | Rewritten every frame (never reallocated) |
|---|---|
| geometry ranges, BLAS, textures (asset lifetime) | frame constants, camera/view data |
| instance, transform, material, light slots (scene lifetime) | visible-instance lists, indirect draw commands |
| history resources (feature lifetime) | TLAS instance list, instance LUT contents |
| unified buffer capacity, render-graph pool memory, staging ring | render-graph transient *usage* (memory stays pooled) |

Walkthrough (Bistro): **load** → loader requests exact sizes → geometry ranges + texture images + BLAS
allocated, instance slots assigned, dirty slots uploaded once. **Following frames** → only moving
objects' transform slots and frame data are touched. **Delete** → entity slots and (after the asset GC
finds the mesh unreferenced) its ranges, BLAS and textures go into the release queue → freed after
frames-in-flight → the next load reuses the same ranges.

#### 5.8.6 Base implementation (Phase 4 scope)
In:
- Unified geometry buffers for all streams in §5.8.2 with a TLSF range allocator; page tables removed
  from shaders; offsets instead of absolute addresses in mesh table / instance LUT.
- Grow-and-copy growth at load boundaries.
- Category accounting (geometry, textures, RT, scene, transient, headroom) fed by
  `VK_EXT_memory_budget` + VMA statistics, shown in Nox Stats; soft budgets with warnings.
- Frame-class buffers created once per frame-in-flight with high-water-mark growth (today's
  `createInstanceBuffer`/`createIndirectBuffer` and TLAS instance/LUT buffers already follow this; the
  remaining per-frame `createBuffer` calls are removed).
- Single `DeferredReleaseQueue`.

Out (later phases): eviction and hard budgets (§5.9, Phase 6), sparse binding (D5), defragmentation,
fixed streaming pools (6b), BLAS sub-allocation from a shared AS buffer and compaction (§5.14).

#### 5.8.7 Implementation (Phase 4, September 2026)
| Step | What was built | Result |
|---|---|---|
| 4a | Unified geometry streams: `Renderer/GeometryArena` gives each stream one device buffer sub-allocated by element count with `OffsetAllocator` (vendored, TLSF-style, O(1)); the allocator hands offsets out of a fixed element space while the buffer covers what is in use and grows by copying itself, so an allocation's offset never changes and only the base address does. Vertices, meshlet draws, meshlet bounds, meshlet vertices and meshlet triangles moved over; `PagedBufferAllocator`/`FreeListAllocator` deleted. Page tables gone: the frame constants carry one base address per stream, `GpuMesh` keeps `drawsOffset`/`boundsOffset` (the other offsets are baked into the mesh's `MeshletDraw` records at upload), and the four raster shaders index the streams directly. Meshlet bounds got their own range instead of silently reusing the draw allocation. | **Verified 2026-09-17:** identical images. Bistro allocations 1545 -> 1512 (its ~38 pages became 5 buffers), 65 after deleting the scene (freed ranges return immediately, where a page only came back when it emptied completely). |
| 4b | `GeometryRTIndices`: the flat triangle list of every submesh moved into a sixth stream, replacing one `BufferUsage::Index` buffer per submesh. The BLAS build input is the stream base plus the range offset, `MeshBLAS` is down to storage + acceleration structure, and `GpuMesh` stores `verticesOffset`/`indicesOffset` from which `GpuScene::SetGeometryBases` derives each ray tracing record's addresses -- a stream that grows rewrites them through the existing dirty-slot upload, so the hit shaders keep reading one flat record with absolute addresses (3b measured that shape as the fast one). | **Verified 2026-09-17:** ray traced pipelines identical. Bistro allocations 1512 -> 962 (551 index buffers gone), 66 after deleting the scene. Release A GPU 3.81 -> 3.71 ms (Visibility 0.49 -> 0.48, G-Buffer 0.44 -> 0.43: one indirection less per draw), B 56.0 -> 56.7 ms, C 29.0 -> 28.7 ms. VRAM 3129 -> 3347 MB: capacity doubling plus the blocks the intermediate buffers of a growth chain leave behind. |
| 4c | Budgets and release: `Renderer/MemoryBudget` splits the device-local budget (`VK_EXT_memory_budget` through VMA) into geometry 20 / textures 40 / ray tracing 15 / scene 5 / transient 10 percent with the rest as headroom, each category reporting what it holds (streams their capacity and their live ranges, acceleration structures, GPU scene tables, render graph pool, and every image through `NRI::Device::getTextureBytes`, which counts `estimateTextureBytes` -- one estimate now shared with the render graph pool and aware of the BC formats). Soft budget: over budget is logged once per crossing and shown in the stats report, never enforced. One `DeferredReleaseQueue` (`std::variant` of buffer, asset reference and mesh handle) replaces the three queues for buffers, assets and geometry ranges. | **Verified 2026-09-18:** Bistro loaded -> deleted -> loaded again: geometry committed 259 MB throughout (used 154 -> 0 -> 154, so the reload reuses the freed ranges and no stream grows), textures 2143 -> 213 (renderer images) -> 2143 MB, ray tracing 118 -> 0 -> 118 MB, allocations 962 -> 66 -> 962, VRAM 3501 -> 1453 -> 3501 MB. The heap line now shows allocator blocks (2755 MB) next to process usage (3501 MB): about 750 MB is held outside VMA (driver, swapchain, Streamline/NGX). Frame-class buffers were already high-water (bone, light, draw list, TLAS instances double on demand), so nothing allocates in a steady-state frame. Found while testing: the merged queue's drain moved an entry out only on the pass *after* it reached zero, so the erase destroyed it under the queue lock and the Mesh destructor locked the same mutex again (MSVC breakpoint on delete). The entry is now moved out in the pass it reaches zero, and destructors always run outside the lock. Validation output was going to stderr only, so it now goes through the logger into Nox.log; warnings and errors are flushed at once. |

#### 5.8.7 Dependencies
- Scene-class buffers and dirty uploads are defined in §5.5 (Phase 3); Phase 4 moves their memory into
  the scene category.
- Upload path (staging ring, transfer queue) is §5.11.4; Phase 4 keeps today's upload batching.
- Asset GC (§5.15) decides *when* something is unused; this section decides *how* its memory returns.

### 5.9 Residency & Streaming Manager

- **Requests** come from GPU feedback (visible clusters, texture mip demand) and CPU sources
  (world partition, camera prediction, editor selection).
- **Priority** = f(screen size/error, distance, time since last use, category).
- **Eviction:** LRU within priority bands; never evict root pages / mip tails; hysteresis prevents
  thrashing; eviction deferred through the release queue.
- **Per-frame budgets:** max IO requests, max upload bytes, max BLAS builds.
- **Diagnostics:** residency heatmap view, thrash counter, pending request queue size.

### 5.10 Asset Pipeline & Cooking

#### 5.10.1 Cooked formats
| Asset | Cooked representation |
|---|---|
| Textures | BC7 (color), BC5 (normals), BC4 (single channel), BC6H (HDR); full mip chain; mip tail flag; header with streaming metadata. PNG/JPG sources compressed at cook (today: RGBA8). |
| Meshes | meshlets + cluster hierarchy + streaming pages + RT fallback LOD + bounds |
| Materials | parameter block + texture references by asset handle (not path strings) |
| Animation | compressed curves (later); skeleton |
| World | per-cell entity/component data (§5.13) |

#### 5.10.2 Derived data cache
Cook keyed by `hash(source content) + hash(cook settings) + cooker version`; cooking runs as
background tasks on the job system; editor shows cook progress; results shared between projects.

#### 5.10.3 Containers
Runtime builds package cooked assets into large, aligned, memory-mappable container files
(IoStore-like) with an index (handle → offset/size/compression). Editor reads loose cooked files.

#### 5.10.4 Registry
Replace path-string lookups in hot paths with handle-based references (materials → textures).
The registry becomes an indexed database (path ↔ handle maps, type index, dependency index),
eliminating the linear scans measured in §3b.3.

### 5.11 Async IO & Loading

#### 5.11.1 Request API
`AssetManager::Load<T>(handle, priority) -> AssetRef<T>` returns immediately; the reference exposes
state and a completion callback. Components observe state (render placeholder until `Ready`).

#### 5.11.2 States
```
Unloaded → Requested → Reading (IO) → Decoding (worker) → Uploading (upload manager) → Ready
                         ↘ Failed                                        ↘ Evicted → Unloaded
```
Dependencies (mesh → materials → textures) are task-graph edges; a mesh can become `Ready` with
textures still streaming (mip tails first).

#### 5.11.3 IO backend
- Windows: IoRing or overlapped IO on unbuffered handles for large reads; memory-mapped containers for
  runtime packages. DirectStorage (with GPU decompression) as a later option (Open Decision D7).
- Reads are aligned to container blocks; requests are coalesced per file.

#### 5.11.4 Upload manager
- Dedicated **transfer queue** (fallback: graphics queue) with its own command pools.
- **Staging ring buffer** (persistent mapped); copies batched per frame under a byte budget.
- Completion tracked with a **timeline semaphore** (the upload clock) polled on the main thread at frame start; the
  frame submission waits on the last signalled value — no `submitAndWait` on hot paths (single-time command buffers
  remain only for startup/tools).
- BLAS builds queued with per-frame primitive budgets (generalizing this session's upload batching).

#### 5.11.5 Editor integration
Drag-and-drop import/load becomes asynchronous: the entity hierarchy is created immediately with
placeholder bounds; geometry/textures appear when ready; progress shown in a status bar.

#### 5.11.6 Implementation (Phase 5, September 2026)
Baseline before 5a (Release, no debugger, Bistro dragged into an empty scene): the whole load runs in one frame of
3427 ms; 196 assets sum to 3285 ms, of which the mesh 377 ms (GPU upload + BLAS 238 ms) and 195 textures 2908 ms
(~15 ms each). Frame times once loaded: A GPU 3.96 / CPU 0.77 ms, B 56.12 / 1.62 ms, C 28.05 / 1.30 ms.

| Step | What was built | Result |
|---|---|---|
| 5a | NRI multi-queue: `DeviceVK` picks a dedicated transfer family (transfer, neither graphics nor compute; fallback the graphics queue), `NRI::QueueType`, `createCommandAllocator(resetMode, QueueType)`, `NRI::TimelineSemaphore` (`createTimelineSemaphore`, `getValue`, `wait`) and `Device::submit(queue, commandBuffers, waits, signals)` on `vkQueueSubmit2`; the frame submission also moved to `vkQueueSubmit2` and takes timeline waits next to its binary swapchain semaphores. `submitAndWait` waits on its own fence instead of idling the queue. `sharedAcrossQueues` on buffer and texture descs gives concurrent sharing (geometry streams and uploaded asset textures; images stay in `eGeneral`, so no ownership or layout handoff). `Renderer/UploadManager`: transfer command allocator, upload timeline, one persistently mapped 64 MB staging ring (oversized uploads get a retained buffer of their own); `ReserveStaging`, `CopyToBuffer`, `CopyToTexture` (`Texture2D::recordUpload`), `CopyBuffer`; `Flush` submits and signals the next timeline value, `Poll` returns completed ring space. It replaces the upload batch, the per-texture staging buffer and every upload `submitAndWait`; stream growth copies through it too. BLAS builds left the load: queued per mesh and built by a "BLAS Build" graph pass under 2M primitives per frame with one reusable scratch buffer; `GpuScene::SetMeshBlas` puts the mesh into the TLAS. Precise synchronization now tracks per resource the last write, the reads since it and the stages already made visible, and carries that state across frames by physical resource (the multi-reader depth hazards and cross-frame write-after-write that synchronization validation reported). | **Verified 2026-09-18:** synchronization validation clean (NGX aside), images identical, ray tracing of freshly loaded Bistro fills in, load -> delete -> reload fine. Release (log: transfer queue on a dedicated family): longest load frame 3427 -> 3070 ms, mesh GPU upload + BLAS 238 -> 39 ms (mesh asset 377 -> 162 ms), textures 2908 -> 2689 ms (14.0 each: file read and decode on the main thread dominate, 5b's work), all assets 3285 -> 2851 ms. Frame times unchanged: A GPU 3.82 / CPU 0.77, B 55.38 / 1.63, C 28.56 / 1.31 ms. Measurement note: a Vulkan Configurator layer override applies to Release builds too (first 5a run: recording 10-50x slower per pass, CPU B 6.6 ms); close it before measuring. |
| 5b | Background loading (`Asset/AssetLoader`, driven by `EditorAssetManager::Update`): `AssetState` (Unloaded / Loading / Ready / Failed) and `RequestAsset`, which starts a load once and never blocks; `GetAsset` stays for tools. The scene sync point and the material texture lookup request instead of loading, entities stay pending until their mesh is Ready, materials draw without a texture until it is loaded and are re-packed then, and failed loads are not retried per frame. Cooked textures: the .ntex header is read on an IO worker, the main thread creates the image and reserves staging, the texels are read from the file straight into staging, copied on the transfer queue and published once the copy completed. Meshes: .nmesh read and parsed on an IO worker, submeshes admitted per frame with ranges and staging on the main thread, written into staging by a job (patched draws, flat RT index list, bounds), copied and published in order. Materials, skeletons and animations import as jobs. All loads share a 64 MB per frame staging budget. The main thread reserves staging and workers only write it, so the ring needs no lock and no worker ever waits for ring space (a waiting worker could starve the frame graph). `UploadManager`: reservations stay open until their copies are recorded (`Commit`), frames wait only for the copies they read (stream growth, synchronous uploads) plus completions already published, every transfer command buffer starts with a barrier (copies of different submissions touch the same streams), ring 128 MB. `Renderer` uploads split into begin / write (any thread) / end / publish; `UploadTexture` and `UploadMeshGeometry` are built on them. DDS and KTX2 sources are cooked into .ntex like PNG/JPG (they keep their own mips and block compression; a Basis KTX2 is transcoded once at cook time). `MaterialComponent` holds overrides only (UE's OverrideMaterials: indexed by slot, zero keeps the mesh's material), so the drag-in no longer copies the mesh's list into every entity and scene loading no longer loads meshes to fill it. A sweep made while loads are in flight is repeated once they have all finished. | **Verified 2026-09-18:** validation clean (NGX aside), images identical, load -> delete -> reload fine, VRAM back to Textures 212 MB / BLAS 0 / 71 allocations after deleting Bistro. Release, Bistro dragged into an empty scene: longest frame 3427 (baseline) / 3070 (5a) -> 167 ms, which is the drag-in's synchronous mesh load (155 ms, 5c); everything visible about 1.5 s after the drop, streamed over normal frames. Opening the saved scene: the YAML read is one synchronous 0.93 s frame (§9 model instances, D12), the mesh then streams in 277 ms and all 597 assets within about 2 s. Frame times unchanged: A GPU 3.85 / CPU 0.78, B 55.75 / 1.72, C 28.51 / 1.39 ms. Left for 5c: one frame's 36 ms in Asset Manager Update (image creation for a frame's textures) and a 62 ms GPU wait (BLAS builds). Found while testing: deferred releases ran before the in-flight fence wait, so a release came one frame early; with streaming, freed stream buffers were still read by the GPU (device lost in Release only). They now run after the wait, and a grown stream's old buffer waits one frame more (the next frame reads it through the grow copy). Bistro's textures had no .ntex (only PNG/JPG were cooked), and materials packed while a texture loaded were not re-packed when it arrived synchronously. Bistro's scene file was 54 MB (551 material handles copied into each of 2909 entities): 3.9 MB now. |
| 5c | Model instances (§5.11.5, how UE's Packed Level Actors and Unity's model prefab instances keep scenes small): a dropped model is one root with `ModelInstanceComponent` (model, removed nodes, per-node overrides); `Scene/ModelInstance` spawns its node entities from the cooked node data once the model (with its skeleton and clips) is loaded -- hierarchy, meshes, lights, cameras, animators and clip groups, moved out of the editor into core. Node UUIDs derive from the instance UUID and the node index, so animator node tables and children attached to nodes resolve on every load. The scene saves the root, the removed nodes (deleting a node records it; "Restore Removed Nodes" respawns) and overrides computed against the model at save time (transform, name, material overrides), never the spawned nodes. Duplicating an instance spawns fresh nodes with the original's overrides; removing the component unpacks it. Not saved yet: other components added to a node, reparented nodes. Async drag-in: the root appears at once and follows the placement preview, the nodes spawn when the model is ready; the editor never loads a mesh synchronously. Cooking on workers: `MeshImporter::CookMesh` (glTF -> .nmesh, skeleton, clips, one .nmat per material) and `TextureImporter::CookTexture` run as jobs when a load finds no current cooked file (stb's thread-local vertical flip); imports only register and start the background load, and a model's textures and materials are registered (not loaded) when it is published. Status bar: loads in flight, bytes pending upload, BLAS builds pending. Per-frame budgets for what a large model otherwise does in one frame: 500 k BLAS triangles (was 2 M), 64 submesh publishes, 512 new GPU scene registrations (removals immediate); material files are checked once per distinct .nmat and a model folder is scanned only after a cook. Content browser thumbnails request their material and texture instead of loading them. | **Verified 2026-09-18:** drag-in, delete, content browser, validation clean (instance save / reopen with overrides and removed nodes, duplicate, Play/Stop and a first-time cook still to be checked). Release, Bistro dragged into an empty scene: longest frame 3427 (baseline) / 3070 (5a) / 167 (5b) -> 24 ms, everyday frame 1.75 ms, GPU peak 15 ms (was 42 ms with 2 M BLAS triangles per frame); mesh streamed in 228 ms, all 601 assets in about 2 s after the drop. Left: the model-ready frame's registration of its textures and materials and the skeleton/clip lookup (16 ms) and spawning plus the first registrations (9 ms). Bistro's scene file holds the instance root instead of 6007 entities. |

### 5.12 Texture Streaming

#### 5.12.1 Approach (Decision D3: mip streaming first, virtual texturing later)
- **Mip streaming (UE Texture Streaming model):** each texture has resident mip range
  `[firstResidentMip, lastMip]`; mip tail (e.g. ≤ 64²) always resident.
- **Demand:** GPU computes required mip per material/texture from screen-space UV derivatives
  (written to a small feedback buffer), combined with CPU distance heuristics for not-yet-visible
  content.
- **Update:** new mips uploaded into a new image or into a pre-allocated full image with per-mip
  memory commit; shader sampling clamped via a per-texture min-LOD value in the material buffer to
  avoid sampling non-resident mips (bindless-friendly, no descriptor churn). **Built (6a): the new image** (UE's
  texture reallocation) -- the image only ever holds the resident mips, so raster and ray traced shaders need no
  clamp; the new image gets a new bindless slot and materials re-pack onto it (§5.12.3).
- **Budget:** texture pool share of VRAM; over budget drops highest mips of lowest-priority textures.

#### 5.12.2 Streaming virtual texturing (SVT, chosen 2026-10-29)
UE5's Streaming Virtual Texturing: a virtual texture is resident by **tile**, not by whole mip, so a 16K texture of which the view
shows a corner holds only that corner's tiles at the mips the pixels ask for. It complements mip streaming (§5.12.3) and does not
replace it: textures stay mip-streamed unless marked virtual (Unreal's per-texture "Virtual Texture Streaming", plus an automatic
threshold for very large ones).

- **Sampling (SVT-1, built 2026-10-29).** Every material texture lookup goes through one entry point in `shaderIO.h`:
  `SampleMaterialTexture` / `...Bias` / `...Grad` / `...Level` and `MaterialTextureSize`, by the texture index the material holds.
  It covers raster, ray-traced hits, the path tracer and ReSTIR: 74 lookups in 25 shaders, converted mechanically and compiled.
  The behavior is unchanged: the index is still the image's bindless slot. A virtual texture's index will carry a flag and resolve
  through its page table here, so no material shader has to know which kind it samples.
- **Cook (SVT-2, built 2026-10-29).** A virtual texture's .ntex is an NTV1 file (`TextureImporter::SaveNTV`):
  - Every mip larger than a tile is cut into 128x128-texel tiles, each with a 4-texel border (UE's defaults: bilinear and
    anisotropic filtering never read outside the tile). Tiles go mip by mip, row by row, each one contiguous file range
    (`ReadVirtualTextureTile`, one `VFS::ReadRange`, like a geometry page). Blocks are copied, never re-encoded. The border and a
    mip's last tiles past its edge wrap around the texture (repeat addressing).
  - The mips no larger than a tile (the tail) are stored whole. At runtime they are an ordinary small texture, always resident: the
    fallback under every tile and the data for the coarsest mips.
  - Setting: `TextureSpecification::virtualTexture`, Auto / Off / On (Unreal's flag plus its Auto Virtual Texturing Size: Auto is
    virtual from 4096² texels). Stored in the registry as `VirtualTexture: On|Off`. A texture can be virtual only if it is one 2D
    layer with power-of-two sides and a fixed block size (BC or plain texels, not a KTX2 kept in its own format), larger than a tile, with mips down to a
    tile. A cooked file whose kind disagrees with its setting is cooked again, as a stale hash would be.
  - `ReadCookedTextureMips` still returns whole mips of an NTV1 file, put together from the tiles' insides. Mip streaming, the
    synchronous importer and the HLOD bake read virtual textures unchanged until SVT-3 streams their tiles.
  - Offline test: every tile of every mip, block by block, against the source with wrapping; every run of mips read back
    byte-identical; NTX1 reads as before. Sizes covered: 4K BC7, 8Kx2K BC1, thin, RGBA8, one tiled mip (non-power-of-two sizes are refused). The cost is the borders, 13% for
    a 4K texture, as in UE.
- **Runtime and virtual sampling (SVT-3 + SVT-4, built 2026-10-30; one step, since the tile feedback comes from the sampling helpers).**
  - **Loading.** A virtual texture loads only its tail (the mips no larger than a tile) as an ordinary image; its slot is the
    material texture index, with `shaderio::MaterialTextureVirtual` set (`Renderer::RegisterVirtualTexture`, from the asset manager).
    With "Stream Virtual Textures" off, textures cooked as virtual load mip streamed instead (their tiles put together into whole mips).
  - **Physical pools** (`VirtualTextureStreamer`): one image per format, a grid of 136x136-texel slots (48x48 by default: 41 MB of
    BC7, 20 MB of BC1/BC4; "Pool Tiles Per Side" in the editor). Its first transition is on the transfer queue, and its tiles are
    copied there too (`Texture2D::recordRegionUploads`).
  - **Page table**: one buffer for all virtual textures (`VirtualTileCapacity` entries, 4 MB), one uint per tile of each tiled mip,
    plus a `GpuVirtualTexture` descriptor per material texture slot.
    - Each entry holds the pool slot and mip of the finest resident tile at or above it, or "unmapped" (the tail). So a sample takes
      one lookup and never finds a hole.
    - The CPU keeps each table and re-uploads a texture's whole table when it changes. Loading a tile maps its subtree to it;
      evicting a leaf maps its subtree back to its parent.
    - Power-of-two sizes make the tile hierarchy exact.
    - The table changes by the frame's own commands, in the `Streaming Update` pass shared with geometry pages (§5.7.2).
  - **Residency is ancestor closed**, as geometry pages are dependency closed:
    - A tile starts loading only once its parent is loading or resident, and becomes visible only after its parent.
    - Only leaves are evicted (no child loading or resident).
    - An evicted tile's slot is reused only after MAX_FRAMES_IN_FLIGHT + 2 frames, once nothing samples it.
  - **Tile feedback** (`MipFeedback.slang`): uses the G-buffer resolve's and the transparent pass's mip feedback pixels, one per 8x8
    tile, rotating each frame.
    - It reports the tile at the finer of the two mips the sample blends. The path tracer reports its primary and mirror hits with
      their ray-cone LOD.
    - It sends at most one request per tile and frame (a per-tile stamp buffer). A missing tile asks every frame; a resident one asks
      once per 16 frames.
    - Requests go into a list after the mip feedback in the same buffer, cleared and read back with it.
  - **Streamer**: requests coarse mips first, along with each requested tile's missing ancestors.
    - It reads tiles from the cooked file straight into staging, then copies them into pool slots.
    - When a pool is full, it evicts tiles not wanted for 64 frames first (least recently wanted first), then wanted tiles finer
      than the request (Nanite-style priority replacement by mip).
  - **Sampling** (`shaderIO.h` helpers):
    - LOD: as the anisotropic sampler picks it, with anisotropy capped at 8 (twice the border).
    - Trilinear blends two mips, one page-table lookup each.
    - The pool is sampled with gradients scaled to the mapped mip, with the footprint shortened to stay inside the border.
    - Past the tiled mips, the tail image samples alone, filtered by the hardware.
    - Ray-traced and path-traced hits use the same helpers at their ray-cone LOD.
  - **Offline test** of the runtime logic: random ancestor-closed load and evict sequences on a 1024x512 texture, with every page
    table entry checked against its finest resident ancestor. Then 750,000 shader-style samples, each with its bilinear neighbours,
    checked against the source texels through a simulated pool filled from NTV1 tiles (borders and repeat wrapping included). All
    pass.
  - **Measured on Bistro (2026-10-30, 45 textures of 4096² cooked virtual, Release, hybrid, DLSS Quality):** texture VRAM went from
    680 MB (virtual texturing off, the 45 textures mip-streamed) to 381 MB (on). The 45 textures took about 300 MB mip-streamed;
    virtual, they take 2079 resident tiles in two pools (BC7 and BC7 sRGB, 48x48 slots, 81 MB together). The image is the same,
    and the GPU frame is unchanged (18.2 vs 18.4 ms; the G-buffer resolve costs 0.31 -> 0.37 ms for the page table lookups).
    With a deliberately small pool, the surfaces where tiles have no room fall back to coarser mips (rust on metal looks blurry
    up close) and recover when the pool is large enough again -- the expected behavior.
  - **Not covered yet:** the packaged runtime loads textures synchronously (no mip or tile streaming there yet). A virtual texture
    renamed or moved while loaded keeps reading its old path until reloaded, as a mip-streamed one does.
- **Exit criterion:** a 16K test texture renders identically to its mip-streamed version while holding only the visible tiles.
  Bistro is unchanged with SVT off, and its texture memory falls further with SVT on for its largest textures.

#### 5.12.3 Implementation (Phase 6a, September 2026)
Baseline before 6a: every texture is resident at full resolution once loaded -- Bistro holds 2141 MB of textures
(budget 2900 MB), PNG/JPG sources cook to RGBA8.

| Step | What was built | Result |
|---|---|---|
| 6a-1 | BC7 cooking of PNG/JPG through bc7enc_rdo (submodule `NoxCore/vendors/bc7enc_rdo`): the RGBA8 mip chain is encoded block row by block row on the job system, perceptual weights for sRGB, linear otherwise; DDS/KTX2 keep their own format and mips (KTX2 mips reordered largest first like every .ntex). NRI `CommandBuffer::copyTextureMips` (image -> image per mip, `vkCmdCopyImage2`) and `fillBuffer`; `Texture2D::recordInitialLayout` split out of `recordUpload`. Descriptor heap images 1000 -> 4096 (old and new image of a reallocation coexist). | **Verified 2026-09-18:** FlightHelmet textures 272 -> 68 MB, images the same. |
| 6a-2 | `Asset/TextureStreamer` (owned by `EditorAssetManager`): a texture with mips above the tail (larger side > 64) streams. Loads read only the tail (`TextureLoad` from `TailFirstMip`, `ReadCookedTextureMips` of one file range); a reallocation creates an image for `[newFirst, last]`, reads the new mips from the file into staging on an IO worker, copies the kept mips image -> image, and all copies go in one transfer submission (`BeginTextureUpload(first mip, upload mip count)`, `EndTextureUpload(kept from)`); on completion the asset manager swaps the image into the asset and invalidates the old slot so materials re-pack, the old image leaves through the deferred release. Shares the loads' 64 MB per frame upload budget, 32 reallocation starts per frame. | **Verified 2026-09-18:** Bistro loads tail first and sharpens; with every mip wanted the settled images match pre-6a (2138 MB), 208 MB after deleting Bistro. |
| 6a-3 | GPU mip feedback: a 4096-entry buffer (one uint per image slot) cleared each frame, written by the G-buffer material resolve and the transparent pass -- one pixel per 8x8 tile, rotating with the frame, takes `floor(lod) + 16` of each texture it samples against the image's own size and `InterlockedMin`s it into the slot (a plain read first skips the atomic when the slot already holds it) -- and read back per frame slot. The streamer maps slot -> texture and the first mip that image had (a replaced image keeps mapping for 8 more readbacks) to the finest mip each texture is asked for. | **Verified 2026-09-18:** far textures stay low-res and sharpen when approached; Bistro textures 1408 MB after flying through (was 2138). |
| 6a-4 | Pool, priority, hysteresis, eviction: the pool is the Textures budget minus what other images hold (the streamer's own images in transit -- new images of reallocations in flight, replaced ones not released yet -- are not others), or an editor override. A finer request counts at once; a texture asked for less keeps its mip for 120 readbacks, then relaxes one mip at a time. Plan per frame: the pool is a cache (resident stays, wanted is added); over it, textures out of view give first (longest unseen first), then those in view (larger first, handle as tie-break -- an order that holds still while the view does), first mips nothing wants, then one mip at a time round after round, never below the tail. Shrinks start before grows. Stats (Nox Stats counters, Settings -> Texture Streaming with the pool override slider), debug view 20 "Texture Streaming Mips" (red: the pixel wants a finer mip than the image holds, also for magnified full-resolution textures; green: as needed; blue: more than needed -- a texture's mips follow its closest pixel, so far pixels of a shared texture are blue), status bar refreshed once per second. | **Verified 2026-09-18 (exit criterion):** Bistro view wanting 69 MB with the pool at 64 MB: resident 63 MB, renders correctly with reduced mips, settles with no reallocation in flight; back to the budget it grows to 129 MB. Textures category 342 MB (was 2141 MB before 6a). GPU frame 3.9 ms in both, G-buffer with feedback 0.51 ms. Found while testing: the passes declared the feedback buffer write-only, so the barrier after the clear did not cover the shader's plain read (feedback lost, everything relaxed to the tail); counting the streamer's in-transit images as others shrank the pool with every reallocation (shrinks forcing more shrinks); a texture could start its next reallocation in the frame the previous one completed, copying from the not yet swapped old image one mip off (now starts come before completions); cutting by the exact readback last seen reshuffled every frame (thrashing under pressure). Also fixed: the IBL precompute timed out under a capture tool (TDR: irradiance at 0.1 rad steps from a filtered mip, prefilter one submission per mip, roughness 0 a single sample). |

### 5.12a Level Streaming (built 2026-10-23)

Unreal 4's model, and the base World Streaming builds on: a **level** is another scene file (a scene asset), made and saved on its
own, loaded **additively** into the running scene and unloaded again.

- **Scene API:** `Scene::LoadLevel(handle)` / `UnloadLevel(handle)` / `GetLevelState(handle)` (Unloaded / Loading / Loaded). The file
  is read and parsed on a thread and its entities created into the running scene for at most 3 ms a frame (`SceneLoader` in its
  additive mode); every one carries a runtime-only `LevelComponent` with the level's handle, which `UnloadLevel` destroys. Both are
  carried out at the start of the next frame, outside every script call (so scripts may call them; unloading calls OnDestroy).
- **Starting like the scene's own entities** once the whole level is in: scripts (after the script loop), 2D bodies, particles,
  timelines; 3D bodies the components asked for are held until then (a crate made before its floor fell through).
- **IDs:** a level whose entity IDs are already in the scene (the same file twice, or a scene saved as a copy of another, which keeps
  the IDs entities, parents and scripts refer to each other by) is refused with an error naming the entity; nothing is created.
  Volumes do not retry it until the source has left them.
- **Level Streaming Volume** (`LevelStreamingVolumeComponent`): a box (HalfExtents in the entity's space, scaled by its transform) and
  its levels; loaded while the streaming source is inside -- the primary camera in Play, the editor camera in Simulate (Unreal's
  Simulate does the same) --, unloaded once the source has been outside every volume naming the level for its Unload Delay. Drawn in
  orange while editing; levels added by drop or combo, their state shown in Play.
- **Scripts:** `Scene.LoadLevel("Levels/Interior.nox")`, `UnloadLevel`, `GetLevelState`, `IsLevelLoaded` (script API version 3).
- **Packaging:** a level a volume names is a handle in the scene file, one a script names a path in its source: both are collected
  as dependencies already.
- **Volumes own their levels:** a level any volume names is unloaded once the source has been outside those volumes for the delay,
  also when a script loaded it. Levels a script loads and unloads itself are levels no volume names.
- **Test:** Facerun's `Scenes/LevelStreamingTest.nox`. In Play its camera's `LevelStreamingTest` script checks a script level
  (load, state Loading then Loaded, its entities and their script running, unload, load and unload in one frame) and a volume level
  (the camera moved into the volume, out again, the level staying for its Unload Delay, then gone) and logs each check and
  `[LevelStreamingTest] PASS` / `FAIL`; T runs it again, L / U load and unload the script level by hand, and WASD flies the camera
  into the orange pad (the volume). The levels' boxes are meshes their script builds, so the test needs no imported asset.
- Not yet: editing several levels together in the editor (Unreal's Levels window -- each level is edited as the scene it is), a level
  shown only once complete (its entities appear over the frames it loads), streaming sources other than the camera, level transforms.

### 5.13 World Streaming

- **World partition grid:** world cooked into cells (entities, components, cell-local asset
  references); cells load/unload by distance to streaming sources (camera, gameplay anchors) with
  hysteresis and per-frame instantiation budgets.
- **HLOD:** merged proxy meshes/materials for distant cells so far content stays visible when cells are
  unloaded.
- **Activation stages:** loaded (data in memory) → active (rendering) → simulated (physics/scripts).
- **Editor:** full world view with cell visualization; editing loads required cells.
- **Scene serialization** moves from one YAML file to per-cell files for streaming and version control.

#### 5.13.1 6c-1: grid, cells and streaming (designed and built 2026-10-24)

Unreal 5's World Partition, built on level streaming (§5.12a): a **cell** is streamed exactly like a level -- read and parsed on a
thread, its entities created for at most 3 ms a frame, marked with `LevelComponent` (the cell's ID), started like the scene's own
entities once complete, destroyed on unload. What World Partition adds is *which* cells: the engine decides from the world, not a
volume or a script.

- **One world, one scene asset.** A scene is world-partitioned once converted (World Partition panel: cell size, loading range).
  Its own file keeps the always-loaded entities and the **cell index** (each cell's grid level and coordinates, content bounds and
  entity count); every cell's entities are in their own file, `<Scene>_Cells/L<level>_X<x>_Z<z>.noxcell`, the scene file format.
- **D12 decided: YAML per cell.** Cells are edited as text like scenes (diffable, a merge conflict only when two people change the
  same cell -- Unreal's one-file-per-actor goes further, per entity, which is the next step if cells conflict in practice). A cooked
  binary cell form waits for a binary scene form (the runtime scene is still YAML behind a header, §5.11).
- **Which cell (Unreal's hierarchical grid).** Only root entities are placed; a root's children go with it. A root is spatially loaded
  when its tree has something in the world (a mesh, model or prefab instance, a local light, a sprite, particles, text, a physics body);
  sky, sun, environment, cameras and logic-only entities stay always loaded. `WorldPartitionComponent` overrides it (Spatial / Always
  Loaded). A spatial root's grid level is the smallest whose cell size (`CellSize * 2^level`) holds its bounds' larger horizontal side,
  its cell the one holding the bounds' center; without bounds (a mesh not loaded) it keeps the cell it was loaded from, else its
  position at level 0. Big things therefore live in big cells and load by the distance to their bounds, not their pivot.
- **Streaming.** A cell is wanted while a streaming source is within `LoadingRange` of its content bounds (horizontally), unloaded once
  every source is 10 % beyond it (hysteresis); nearer cells first, the level streaming budget shared. Sources: the primary camera in
  Play, the editor camera in Simulate, and entities with `WorldStreamingSourceComponent`. Play / Simulate start with the cells around
  the sources loaded before the first frame (Unreal blocks on initial streaming too).
- **Editor.** Opening a world loads the cells around the editor camera; the World Partition panel (a top-down map of the cells, their
  state, the camera and its range) loads and unloads cells or regions, Load All / Unload All. Saving rewrites the loaded cells' files
  (an emptied cell's file goes), merges entities moved into a cell that is not loaded into its file (and then loads that cell), and
  writes the index. Unloading a cell whose entities changed asks to save first. Play and Simulate start from the editor's state, unsaved
  changes included: the loaded cells' entities become in-memory cells the session streams like the files. Loading or unloading cells
  is not an undo step (the history starts again; undo waits while cells load), and neither is the world itself (an undo step keeps
  the settings, index and loaded cells: undoing past a conversion does not un-convert). The cell size changes only while every cell
  is loaded (saving then re-partitions the whole world).
- **Split Into Separate Entities** (Properties > Model Instance): a world partitions per root entity, so a model placed as one
  instance (Bistro: one root, about 6000 spawned parts) would be one cell. Split, every part with a mesh or light becomes a root
  where it stands, filed in a folder named after the model, and goes to its own cell (Unreal partitions per actor). Not for
  animated trees.
- **Choosing the settings:** the loading range is how far content must be present (view or fog distance, later where HLOD takes
  over); the cell size sets how coarse streaming is (about half the range; Unreal's defaults are 128 m / 256 m, used here too).
- **Packaging and references:** the packager ships the cell files of every world it packs and collects what they name; Find
  References, unused-asset detection and Replace References read cell files like scene files.
- **Not in 6c-1:** activation stages and HLOD (6c-2, 6c-3), data layers, per-entity files, a world larger than memory (6c test world).

**Built:**
- `Scene/WorldPartition.{h,cpp}`: the grid (`WorldGrid`: cell IDs, names, hierarchical placement, distances), the scene's
  `WorldPartition` (settings, cell folder, index, origins, a Play copy's in-memory cells) and Scene's world members (placement,
  editor load / unload, streaming, Play / Simulate start, saving, Save As, undo). Cells are `StreamedLevel`s (`IsCell`) in level
  streaming's list: the same loader (`SceneLoader`, which reads a cell from memory too and skips an entity already in the scene),
  budget, unload and start-up path.
- `WorldPartitionComponent` (Auto / Spatially Loaded / Always Loaded) and `WorldStreamingSourceComponent` (serialized, in the
  Properties panel, which shows an entity's cell).
- `SceneSerializer`: the scene file's `WorldPartition` map (settings, folder, one line per cell: level, x, z, entities, bounds), cell
  text (the trees in ID order) and merging into a cell file; saving a world writes its cells first and leaves their entities out.
- Editor: Window > World Partition (`Panels/WorldPartitionPanel`: convert, settings, the map with cell states, content bounds, the
  camera's range, selection, load / unload / load around camera / all), the loaded cells' bounds in the viewports, cells around the
  camera on open, undo held while cells stream.
- **Test:** Facerun's `Scenes/WorldPartitionTest.nox` -- 6 x 6 cells of 20 m (a tile and a turning pillar each), a 70 m slab in a
  level 2 cell west of them, a streaming source in the far corner, an always-loaded pillar; loading range 40 m. In Play the camera's
  `WorldPartitionTest` script checks the first cells are in before the first frame, cells follow the camera, the slab loads by its
  edge (30 m) not its center (65 m), a loaded cell stays inside the hysteresis band and goes beyond it, the source keeps its cell, the
  always-loaded pillar stays and streamed scripts run, then logs `[WorldPartitionTest] PASS` / `FAIL`; T runs it again. Editor checks
  (by hand): open it, load / unload cells in the panel, move a pillar into an unloaded cell and save (it joins that cell's file, which
  then loads), Play with unsaved changes, convert a scene and save.

#### 5.13.2 6c-3: HLOD (built 2026-10-24)

Unreal 5's World Partition HLOD (HLOD layer "Approximated Mesh": a merged, simplified shell with baked materials): what is far
away stays on screen as one cheap mesh per cell after the cell itself has streamed out.

- **One HLOD per world cell** that has static meshes, built from that cell's entities (its file, read into a scene of its own --
  the editor's scene is not touched, so every cell can be built, loaded or not). Model and prefab instances in the cell are
  spawned there first; skinned meshes are left out (Unreal's HLODs are static too).
- **Geometry (Unreal's Approximated Mesh: one closed shell, not the parts themselves):** the cell's static meshes in the cell's space
  (parts smaller than twice the HLOD's error are left out: they would be under a pixel where the HLOD shows) are voxelised -- their
  distances sampled on a grid near them, everything the outside cannot reach solid -- and the surface is taken half the error off
  them by surface nets: one closed, welded, manifold shell around the cell (thin parts such as awnings become thin slabs). That is
  simplified with meshoptimizer by the other half of the error (`meshopt_simplify`, then `meshopt_simplifySloppy` when over the
  triangle budget), hard edges split by normal angle, a new UV atlas laid out by xatlas, tangents computed and written with the mesh
  (the baked normal map is in exactly that frame). The grid is capped (32M samples, 2 bytes each): a larger cell gets larger voxels
  and the log says which. Simplifying the parts directly (first version, `Settings::Remesh` off) tore Bistro's open, overlapping
  parts into shards and holes.
- **Materials baked (Unreal's ProxyLOD bakes the same way, on the CPU):** every texel of the atlas is a point and normal on the
  HLOD's surface; a ray along the normal (both ways, within twice the error) finds the source surface it stands for -- where neither
  does (a slab's rim, facing along its part), the nearest source point -- and
  the source material is evaluated there -- base color (alpha-masked parts are passed through), the normal map turned into the
  HLOD's tangent frame, roughness / metallic (specular-glossiness converted), emissive. Glass (alpha blended or transmissive) stays in
  the shell but is baked as what shows through it -- what is behind, by its transmission, else dark -- under its own gloss and normal
  (baked as its own colour, Bistro's windows came out as white panels). Textures are read from the cooked .ntex
  (BC1-BC7 decoded with bcdec, the mip nearest the bake's texel size). Each texel is sampled n x n (`SamplesPerTexel`, 2) and the
  source textures are read at the mip level of a sample's size (box-filtered mips on the CPU, the source triangle's texture
  coordinates per world unit): read at full resolution with one sample, fine detail aliased into speckled normals and every pin hole
  of an alpha-masked texture cut a hole in the HLOD (Bistro's walls). A texel is cut out only where most of its samples are. Empty texels are filled from their neighbours (no seams in
  the mips). Texture size from the surface area and a texel density, power of two, 128-2048.
- **Output:** a glTF (positions in meters, normals, tangents, UVs; base color, normal, metallic-roughness and emissive PNGs) in
  `<Scene>_Cells/HLOD/`, imported like any model (cooked to .nsmesh / .nmat / BC7 .ntex), so packaging, texture streaming and the
  GPU scene treat it like any mesh. Its entity (`HLODComponent`: the source cell) is kept in an HLOD cell file.
- **HLOD grid:** HLOD entities are streamed in a second grid (`WorldCellCoord::Grid` 1: its own cell size and loading range,
  default 2x the cell size and 4x the loading range) like the world's cells, by the same code.
- **Showing and hiding:** an HLOD is drawn while its source cell is not loaded and drawn -- hidden once the cell is loaded and none
  of its entities waits to be registered with the GPU scene (no frame without either), shown again when the cell starts to unload.
  Hidden HLODs are left out of the GPU scene and the TLAS; shown ones are traced too, so far shadows and GI keep the city.
- **Editor:** World Partition panel > Build HLODs (all cells, on workers, with progress; the import on the main thread), Show HLODs
  (the HLOD grid streams around the editor camera while editing, so the unloaded world is visible as its HLODs). HLOD entities are
  generated: never saved into the scene file or a world cell, never an undo step.
- **Not in the first HLOD:** HLOD levels above HLOD0 (HLODs of HLODs), instanced HLODs, baking ambient occlusion.
- **Code:** `Scene/HLODBake` (geometry and bake, engine independent), `Scene/HLODBuilder` (per cell: spawn, read, bake, import; the
  HLOD grid written), `Scene/WorldPartition.cpp` (HLOD grid streaming, `UpdateHLODVisibility`), `Panels/WorldPartitionPanel` (HLOD
  section). Vendors: xatlas, bcdec (submodules). Building is cancelled by Play, Simulate, opening or creating a scene and prefab
  editing (the HLODs imported so far stay; the HLOD grid is written only by a finished build); undo waits for it.
- **Test:** split a large scene (Bistro: Split Into Separate Entities), convert it (e.g. 32 m cells, 64-100 m loading range), save,
  Build HLODs; with Show HLODs, move the editor camera away: unloaded cells show as their HLODs, loaded cells hide theirs. Then Play:
  the cells stream around the player and the HLODs fill the distance, without a frame of hole or double drawing at the boundary.

#### 5.13.3 6c-2: activation stages (built 2026-10-24)

Unreal 5's runtime cell states: a cell is **Loaded** (in memory, not in the world) before it is **Activated** (in the world), and
each streaming source says which of the two its cells get (`EStreamingSourceTargetState`).

- **Loaded:** the cell's file (or a Play session's text) is read and parsed on a worker and its entities are built, a slice a frame,
  into a scene of its own that nothing draws, simulates or scripts (as Unreal's actors are loaded outside the world). What it
  references (meshes, models, prefabs, materials, animations ...: `Scene::CollectAssetReferences`) is requested then, and kept while
  it stays loaded (the editor's unused-asset sweep counts loaded cells).
- **Activated:** its entities are copied into the world, every component, a slice a frame within the streaming budget (3 ms), and
  the built scene is released (nothing is held twice). In Play / Simulate activation waits until its assets are in, at most 2 s
  (the HLOD shows meanwhile: it is hidden once the cell is drawn); the cells Play starts with, and the editor's, go in at once.
  Physics bodies, scripts, particles and timelines start as for any streamed level (`FinishLevelLoad`).
- **Deactivated:** a cell whose sources only load it any more leaves the world and is built again from its file (what ran in it is
  gone, as when it is unloaded; Unreal keeps the actors' state -- copying runtime state back out of the world, physics and script
  handles included, is not done). One being activated keeps its built scene.
- **Sources:** `WorldStreamingSourceComponent` has `TargetState` (Activated / Loaded) and `LoadingRangeScale` (Unreal's names); the
  camera activates. A cell is activated where an activating source reaches it, else loaded where any does; the 10 % hysteresis
  applies to both (activated cells stay in until every activating source has left by 10 % more).
- **Queries:** `Scene::GetWorldCellStage` (Unloaded / Loading / Loaded / Activating / Activated; the World Partition panel colours
  loaded cells blue), and Unreal's `IsStreamingCompleted` as `Scene::IsWorldStreamingCompleted(point, range, stage)`, also for
  scripts (`Scene.IsWorldStreamingCompleted`, `WorldCellState`; script API 5): e.g. a teleport puts a Loaded source at its
  target, waits for Loaded, then moves the player.
- **Test:** WorldPartitionTest.nox has a Loaded source (WPT_Preload); WorldPartitionTest.cs checks its cells are loaded and not in
  the world, go in within a second once the camera reaches them, and leave the world staying loaded once it has gone.

#### 5.13.4 The 6c test world: larger than RAM and VRAM (built 2026-10-29)

The roadmap's exit test for 6c: a world whose data does not fit in memory, flown through without stalls, its memory following the
camera.

- **The world:** editor, Create > Streaming Test World (`StreamingTestWorldGenerator`). N x N cells, one terrain tile each: a height
  field mesh of its own (128 x 128 quads by default, cooked `.nsmesh` with its cluster LOD) and a texture of its own (2048^2 BC1 with
  mips, `.ntex`): nothing is shared, so every cell's data is unique, as in a real open world. One height function over the whole
  world, so tiles meet without seams. Written on a worker straight in cooked form (nothing to import: `MeshImporter::
  BuildCookedGeometry`, `TextureImporter::CookGeneratedTexture`; an asset without a source file is its own cook), then registered on
  the main thread (`EditorAssetManager::RegisterGeneratedFile`) with the cell files, their index and the scene (camera, sun,
  procedural sky), which then opens. N is chosen from the first tile's size and a target size on disk (20 GB by default: about
  4.8 MB a tile, 65 x 65 cells of 64 m, 4.2 km across, against a working set of about 70 cells at the 256 m loading range).
- **Dense cells:** Props per Cell (250 by default) scatters rocks, pines and stumps on each tile, an entity each sharing three
  cooked meshes (as an open world reuses its props): the cells' entity cost -- parsing their text, building and activating their
  entities -- is what a sparse world never shows. The cells are written over frames (12 ms a frame) with the progress shown.
- **Where streaming's time goes:** every cell records reading and parsing its file (worker), building its entities and copying
  them into the world (main thread, in how many steps), waiting for its assets, and the time from being wanted to being in the
  world (`Scene::WorldStreamingCounters`, trace log per cell; script API 7: `WorldStreamingStats` carries the sums). The
  fly-through prints them per cell over the flight, with the main thread's streaming milliseconds per second of flight.
- **What the numbers showed (2026-10-29, 2 GB world, 64 m cells):** 250 props a cell: per cell 14 ms read and parsed (worker),
  7.3 ms built and 0.2 ms activated (main thread), 44 ms of main thread a second of flight, holes on 56 frames. 1000 props: 56 ms
  parsed, 28 ms built, 0.7 ms activated; 167 ms of main thread a second -- more than the 3 ms a frame streaming may take, so cells
  queued (1.8 s from wanted to in the world, holes up to 6 s). The cost was building the entities from the parsed text on the
  main thread (29 us an entity); copying them into the world is cheap (0.7 us).
- **Cells are built on their loading job (Unreal's async loading thread):** the worker that reads and parses a cell's file also
  creates its entities, into the cell's own scene (nothing else sees it until the job is done; creating an entity touches only its
  scene; UUIDs come from a generator per thread). The main thread takes the result and copies the cell into the world, a slice a
  frame; cells build in parallel. A cell unloaded while building is dropped by its job.
- **Play no longer writes the editor's cells out:** the cells the editor had loaded stay in the Play copy as activated cells, and a
  copy of each (every component, in a scene of its own: `Scene::m_WorldSnapshots`) is what it is built from if it streams out and
  back -- unsaved changes included, as the text was. Before, every loaded cell was written as text, its entities destroyed, and the
  source's cells parsed and built again before the first frame: seconds with a thousand entities a cell. Cells entities were moved
  into that the editor had not loaded still go through text (merged with their files).
- **With cells built on workers (1000 props):** the main thread's streaming cost fell from 167 ms to 4.7 ms a second of flight and a
  cell is in 0.38 s after it is wanted (1.8 s before). The frame (CPU-bound: 15.7 ms CPU, 8.1 ms GPU) then showed what 65k
  entities cost every frame, fixed where it was the engine's own waste: the Scene Hierarchy built a row for every top-level entity
  (8.2 ms; now only the rows on screen, as Unreal's Outliner), the unused-asset sweep ran nearly every frame (1-7 ms; now at most
  once a second), and the ray tracing culling computed every instance's world bounding sphere and an asin per frame (TLAS Prepare
  2.2 ms; the sphere is kept with the transform). Left as they are: Transform Propagation's walk over the roots (0.7 ms) and the
  draw list rebuilt when instances come and go (GPU Scene 1.5 ms). The fly-through's warm-up now waits until nothing loads (the
  move to the circle's start is a teleport, which a game waits out), so the flight measures streaming at speed.
- **Cooked cells (built 2026-10-29, for faster flight):** beside each cell's text, "<cell>.noxcell.cooked" holds the same cell in
  binary, stamped with the hash of the text it was cooked from (an edited text is read as text until it is cooked again). An entity
  with only the common components (ID, tag, transform, hierarchy, mesh, material overrides, folder) is stored as values; any other
  keeps its text inside the file, so the binary form never has to know every component. Each binary entity is checked when it is
  cooked: built from its text and from its values, both written back as text and compared -- one that differs keeps its text, so a
  field the binary form does not know is never lost. Cooked on save (on the workers), by the test world's generator, copied by Save
  As, shipped by packaging; the cell's loading job reads the cooked file when it matches (`SceneLoader`, worker), else the text.
  The fly-through counts the cells read cooked (script API 8: `WorldStreamingStats.CellsCooked`). Measured (1001 entities a cell,
  2 GB world): read and built on a worker in 1-2 ms cooked against ~70 ms from text (up to 640 ms when Play starts dozens at once:
  the text parse allocates heavily, and workers contend); the Read Scene scope averaged 0.8 ms cooked, 81 ms text. Same lap, both passing: cooked 0.77 ms a cell on a worker, in the world
  0.05 s after being wanted (longest 0.29 s), 99 % of frames under 10.2 ms; text 86.7 ms, 0.13 s (longest 2.31 s), 18.9 ms. The
  unused-asset sweep went from ~4.8 ms to 0.5 ms (texture paths through the registry's index).
- **Nothing written while streaming:** the generator writes each tile's material (`.nmat`) beside its mesh and registers it, as a
  model's cook does: a mesh whose material file is missing writes and registers it when it loads, and with the registry's
  deferred write on the main thread that cost the 30 GB world (20k registry entries) ~400 ms every second of flight. The
  deferred registry write now copies the entries and writes on a worker (atomically: beside the file, then renamed over it), and
  a changed file is matched to its asset through the path index, not a scan of the registry.
- **Memory follows the cells:** an unloaded or deactivated cell's entities are destroyed, and a Loaded cell's built scene dropped,
  each marking the scene's asset references changed; the next frame's unused-asset sweep (editor and runtime) frees what no
  entity, built cell or HLOD references any more (GPU resources after the frames in flight).
- **The fly-through:** `StreamingWorldTest` (Facerun) on the camera flies one lap of a circle around the world's middle (35 % of
  its side as radius, 40 m/s, 70 m up, looking ahead) and logs a line a second -- cells by state, assets loaded and loading, RAM,
  VRAM, the second's worst frame -- then PASS or FAIL: no frame over 50 ms, the cells within 96 m of the camera in the world on
  every frame, and the second half of the lap needing no more RAM, VRAM or assets than the first (15 % + 64 MB slack). For it,
  script API 6: `Scene.GetWorldStreamingStats()` (cells by state, the world's bounds) and `Diagnostics.Memory` (process private
  bytes, VRAM held by the engine's resources and by the process in all, assets loaded / loading).

### 5.14 Ray Tracing at Scale

- **TLAS from GPU scene** instances (not render queues), with RT-specific culling: distance, screen
  size, importance, and budgets for instance count.
- **BLAS per mesh/LOD** (shared across instances), built through the upload manager queue with
  per-frame budgets; **compaction** after build; refit for skinned/deforming meshes.
- **RT LOD:** coarse resident fallback geometry for off-screen/secondary rays; visible-surface
  consistency handled by ray origin offsets / matching LOD near the camera (Open Decision D8). Tried in 6b-2 and
  reverted (§5.7.1). Streamed meshes (§5.7.2) build their BLAS from the finest resident clusters instead (view-dependent through
  residency, rebuilt as pages come and go); meshes that do not stream keep the original clusters.
- **Memory:** RT category budget; eviction of BLAS for far/invisible meshes, rebuild on demand.
- Lighting/GI features and their arbitration remain defined by the RT plan.

**Status 2026-10-20:** BLAS **compaction** is built: a frame's BLAS builds write their compacted sizes into a per-frame-slot
query pool; once that slot is done the compacted structures are created, the next "BLAS Build" pass copies each BLAS into
its compacted one and the old one goes to the deferred release (`BlasCompaction` in `Renderer.h`,
`Renderer::readBlasCompactedSizes`). Skinned meshes get per-instance BLAS refit (§5.5.3).

**Status 2026-10-23: TLAS culling and BLAS eviction built.**
- **Culling** (`GpuScene::UpdateTlasCulling`, Unreal's ray tracing culling radius / angle): once a frame before the TLAS is
  prepared, a ray traced instance farther than the radius (default 300 m) whose world bounding sphere looks smaller than the
  angle (default 1 degree) is left out of the build; in front of the camera and behind it alike (reflections and GI see behind),
  within 90% of the radius or above 110% of the angle it comes back (hysteresis, no rebuild after rebuild at the edge). A change
  in the culled set rebuilds the TLAS like any instance change. Settings next to the other ray tracing switches, with the TLAS
  instance, culled and evicted counts (also Nox Stats counters).
- **BLAS eviction** (`Renderer::manageBlasResidency`): the same pass records per mesh the last frame an instance of it was traced
  (in a traced bucket and not culled). Over the RayTracing category budget -- judged once per memory sample, down to 90% -- the BLAS
  of meshes no traced instance needed for 300 culling frames go, longest unneeded first: the GPU scene stops referencing them, the
  structure leaves through the deferred release one frame later than usual (frames in flight may trace it). Each BLAS keeps its build
  inputs (geometry ranges, opacity; the geometry stays resident), so an instance needing it again queues a new structure under the
  frame's build budget; it traces again after that build. A compaction measured before the eviction no longer matches (new serial).
- Not built: an RT budget separate from the category share, eviction of skinned per-instance BLAS, out-of-memory handling.

### 5.14a Large World Coordinates (built 2026-10-29)

A float position keeps about 0.1 mm at 1 km from the origin, 1 mm at 10 km, 1 cm at 100 km and 8 cm at 1000 km (the world unit is
the centimetre): far out, objects shake, physics bodies jitter and the camera stutters. Unreal 5's answer (LWC) is double precision
on the CPU and a camera-relative ("translated world") space on the GPU; Nox does the same, with the GPU's space centred on a coarse
**render origin** (Flax's LargeWorlds origin chunks) instead of per primitive tiles, so no shader changes.

- **CPU: double precision.** `TransformComponent::Translation` is a `glm::dvec3`, `WorldTransformComponent::WorldMatrix` a
  `glm::dmat4` (rotation and scale need no more than float). The scene graph propagates in double; `WorldBounds`, streaming sources,
  the cell grid (`WorldGrid::CellFor`, `HorizontalDistance`) and the cell index's bounds (`WorldCellInfo`), prefab spawning, HLOD sources (each HLOD's vertices relative to its own
  double origin), physics, the editor camera and tools (gizmo, picking, measure, snapping, placement) all work in double.
- **Explicit narrowing.** `GLM_FORCE_EXPLICIT_CTOR` (public on NoxCore): no double becomes a float without it being written
  (`Math::RelativeTo`, `Math::Basis`, `Math::ToFloat` say why). Unreal's `FVector3f(FVector)` is explicit for the same reason.
- **GPU: relative to the render origin.** `Renderer::GetRenderOrigin` sits on a 1024 m grid; once the camera is more than a step
  away along any axis it moves to the grid point nearest the camera (vertically too: a flight, space). Everything handed to the GPU is
  relative to it (`Renderer::ToRenderSpace`): instance transforms, lights, the camera, particles, 2D and editor overlays. The camera is
  never more than ~1.8 km from it, so a vertex near the camera keeps a fraction of a millimetre. What works from a height gets the
  origin's height on the CPU, no shader change: the sky's viewer altitude, the aerial perspective's altitude, height fog's base, the
  cloud shadow map's centre (its lookup slides points down to the ground by their height), the editor grid's plane and picking plane.
- **A rebase.** `Renderer::updateRenderOrigin` (in BeginScene, once a frame): the GPU scene keeps every instance's world translation in
  double and rewrites its transforms (the previous ones too, so motion vectors see no jump), bounds and TLAS records once
  (`GpuScene::SetRenderOrigin`). Last frame's view and camera positions are moved over, so TAA, DLSS, NRD and the screen probes
  reproject across it. DDGI keeps its probes: its cascades are placed by the camera's world cell (double), so their storage does
  not change, only where the GPU space puts them. The cloud shadow map reads the cloud field at world positions. The ReSTIR DI / GI /
  PT reservoirs hold sample positions: they start over for a frame (at most once a kilometre).
- **Physics:** Jolt with `JPH_DOUBLE_PRECISION` (`DOUBLE_PRECISION` in its CMake): body and character positions, casts and contact
  points are double (`JoltUtils::ToJoltPosition`); mesh colliders stand at their entity with float vertices around it. Box2D (2D)
  stays float.
- **Particles:** world-space particles are float relative to their simulation's own double origin, which follows the emitter
  (Niagara's LWC tile).
- **Files:** scene and cell text writes translations at full double precision; cooked cells are version 2 (double translations; a
  version 1 file is read from its text and cooked again).
- **Scripts (API 9, 10):** `Nox.Vector3d` (double) for positions: `Transform.Position`, `LocalPosition` / `WorldPosition`,
  `RaycastHit.Point`, `Physics.Raycast` origin, `Scene.Instantiate` / `CreateEntity` / `IsWorldStreamingCompleted` positions,
  `WorldStreamingStats` bounds (API 10). A
  `Vector3` widens to it implicitly; the difference of two positions as a direction is an explicit `(Vector3)(a - b)`.
- **Test:** the streaming test world generator's **World Offset (km)** builds the same world that far out along x and z; the
  fly-through (`StreamingWorldTest`, its path now in double) runs there unchanged. At 1000 km nothing may shake, drift or snap that
  does not at 0.
- **Rays from raster surfaces** (`shaderIO.h raster_surface_bias`): the raster draws cluster LODs, the TLAS the original triangles,
  so a ray starting on the depth buffer's surface starts off it by the LOD's error at that distance (the larger of it and the caller's
  own offset): RTAO, the shadow mask, deferred and forward shadow rays, reflections, screen probes. Without it RTAO had black specks
  far away (the simplified surface under the traced one), gone up close.
- Editor overlays and the asset preview stage are drawn relative to the origin of the moment (a stage placed before an origin move
  stays where it was, a few km off at most).

### 5.15 Asset Lifetime (Garbage Collection)

Implemented this session (editor): mark from live scenes (meshes → materials → textures, animations,
skeletons), sweep unreferenced loaded assets not otherwise held, deferred GPU release after frames in
flight, targeted texture-slot cache eviction. Future:
- Runtime uses reference-counted `AssetRef` from §5.11 instead of scene scans.
- Unloading hands assets to the residency manager (warm cache with budget) instead of immediate release.

### 5.16 Performance Practices

- **No per-frame heap churn:** frame arenas per thread; persistent GPU scene instead of rebuilt
  vectors.
- **Parallel ECS systems** with declared read/write sets; transform propagation parallel per hierarchy
  depth; animation evaluation parallel per animator.
- **Shader compilation off the frame:** precompiled SPIR-V at cook time and a shader permutation
  registry. With shader objects + dynamic state (§3.2) there are no pipeline-state permutations; a
  pipeline cache is only needed for the classic-pipeline fallback.
- **ReBAR** direct mapping for dynamic buffers when available; staging otherwise.
- **Budgets everywhere:** uploads, IO, BLAS builds, instantiation — the frame never waits on bulk work.
- **Measure regressions:** Tracy captures for reference scenes (Sponza, Bistro, large test world)
  kept per phase.

---

## 6. Roadmap

| Phase | Name | Depends on | Key deliverables | Exit criteria (to refine) |
|---|---|---|---|---|
| **0** | Profiling | — | Nox instrumentation layer (macros, scope registry, thread buffers), NRI GPU timestamp queries, memory sampler (`VK_EXT_memory_budget`, VMA stats, process RAM), Nox Stats overlay (CPU/GPU min/max/avg, RAM/VRAM), Tracy backend | Overlay shows CPU scopes and all major GPU passes with min/max/avg and RAM/VRAM for Bistro; the same scopes appear in Tracy; no measurable cost with both disabled |
| **1** | Task system & frame graph | 0 | `Nox::JobSystem` over one Taskflow executor (+ IO executor), frame graph (Game Update → Streaming → Render Prepare; Phase 1: scene update + submit graphs, §5.3), worker-local arenas (`WorkerLocal`, `FrameArena`), ECS access declarations + deferred structural changes, Taskflow observer → profiling scopes, DOT dump command, frame-ID readbacks, parallel transform propagation + deterministic parallel mesh submission. Worker NRI command pools → Phase 2 | Frame work spreads across all cores (visible in Nox Stats/Tracy); Bistro config A CPU frame below the §3b.3 baseline; identical images; frame graph DOT viewable in GraphViz; no `std::thread` outside `JobSystem`; picking correct |
| **2** | Render graph | 1 | Worker NRI command pools (per worker × frame slot, ordered multi-buffer submit), RG core (declare/compile/execute, pass culling), transient pool + aliasing, history resources, derived pass setup, access-model synchronization with Blanket/Precise strategies, parallel partition recording, per-pass profiling scopes + debug labels, visualizer + texture inspection + validation; full frame ported | Identical images in all debug views; no manual `executionBarrier()` or reset flags left; transient memory reduced vs today; recording parallel; Blanket vs Precise barrier comparison recorded (D13) |
| **3** | GPU scene & GPU culling | 2 | Persistent instance/transform/material buffers, dirty uploads, **mesh instancing (importer keeps glTF mesh → node sharing; instances reference one `MeshTable` entry, one BLAS per unique mesh — Bistro: 551 meshes for 2909 instances, today uploaded 2909×)**, Hi-Z, GPU instance culling, indirect-count draws, multi-view | CPU per-frame render prep independent of instance count; Bistro frame time improved (target set after Phase 0 baseline) |
| **4** | Unified geometry memory & budgets | 3 | Base memory system (§5.8.6): unified geometry buffers incl. RT index stream with TLSF range allocation (page tables removed, offsets instead of addresses), grow-and-copy, memory budget polling, category accounting with soft budgets, no per-frame buffer creation, single deferred release queue | Freed geometry is reused by the next load; VRAM per category visible in Nox Stats; no `createBuffer` in a steady-state frame |
| **5** | Async IO & upload manager | 1, 4 | Request API + state machine, IO threads, decode tasks, transfer queue, staging ring, async editor drag-in | Loading Bistro never stalls the viewport beyond a frame budget |
| **6a** | Texture streaming | 5 | BC cooking for all sources, mip tail, GPU mip feedback, residency + eviction | Scene textures exceeding the texture budget render correctly with mip reduction |
| **6b** | Geometry streaming & LOD | 5 | Cluster hierarchy cook, GPU cut selection (**done, 6b-1**), page streaming pool (**built**, §5.7.2), RT fallback LOD (reverted, D8) | Geometry exceeding the geometry budget renders with streaming; no popping beyond threshold |
| **6c-0** | Level streaming | 5 | Additive scene load / unload, Level Streaming Volumes, script API (§5.12a: **built 2026-10-23**) | A level streams in and out around the camera without a stall |
| **6c** | World streaming | 6a, 6b, 6c-0 | Grid cells, per-cell files, cell streaming, editor (§5.13.1: **6c-1 built 2026-10-24**), activation stages (§5.13.3: **6c-2 built 2026-10-24**), HLOD (§5.13.2: **6c-3 built 2026-10-24**), test world and fly-through (§5.13.4: **built 2026-10-29**), cells built on workers and cooked cells (§5.13.4) | Test world larger than RAM/VRAM traversable without stalls |
| **7** | Continuous optimization | all | Profile-driven work, VT, DirectStorage, async compute expansion | Ongoing |

Phases 1, 2 and 3 are architecture-defining and each gets its own plan-mode design before code.

---

## 7. Open Decisions

| ID | Decision | Options | Notes |
|---|---|---|---|
| D1 | ~~Render thread model~~ **Decided** | **Main thread (window, input, submit/present) + Taskflow task graph for everything else; no dedicated render thread.** Frame pipelining stays available as a graph shape (§5.3). | Decided September 2026 — see §5.2.1 |
| D2 | ~~RG resource naming/API style~~ **Decided** | **Typed handles returned by the builder + typed blackboard structs** (§5.4.11) | Decided September 2026 (Phase 2) |
| D3 | ~~Texture streaming~~ **Decided** | **Mip streaming first** (reallocation into a new image, GPU feedback, §5.12.3); virtual texturing later (§5.12.2) | Decided September 2026 (Phase 6a) |
| D4 | ~~Geometry LOD~~ **Decided** | **Cluster DAG** with meshoptimizer's clusterlod (§5.7.1) | Decided September 2026 (Phase 6b) |
| D5 | Geometry buffer growth | grow-and-copy vs sparse binding reserve/commit | **Sparse binding built 2026-10-30** (§5.8.2): the streams reserve address space and commit 64 KB pages as live ranges touch them; grow-and-copy remains the fallback without device support |
| D6 | Additional queues (**transfer and async compute decided**) | ~~one combined graphics/compute queue~~ · **dedicated transfer queue for uploads (Phase 5a, §5.11.6)** · **async compute queue: a compute-only family, render graph blocks with fork / join timeline points (§5.4.4), first user the screen-probe GI** | Transfer: multi-queue device creation, queue-family detection, timeline semaphores and per-queue command pools in NRI; concurrent sharing instead of ownership transfers |
| D7 | IO backend | IoRing/overlapped IO vs DirectStorage | DirectStorage enables GPU decompression |
| D8 | RT geometry LOD policy | ~~dedicated coarse RT LOD~~ (tried in 6b-2, reverted: holes in the path tracer, shadows lost to ray offsets) · RT from the same clusters as raster: cluster acceleration structures per frame (nvpro-samples vk_lod_clusters, `VK_NV_cluster_acceleration_structure`, NVIDIA only for now) | Until then ray tracing uses the original clusters (resident, full detail); streamed meshes trace their finest resident clusters (§5.7.2) |
| D9 | ~~Editor vs runtime asset managers~~ **Decided and built** | **One loading pipeline, two modes:** `RuntimeAssetManager` is `EditorAssetManager` in read-only mode (no file watcher, reimports, registry writes or recooks); engine code uses read-only lookups on `AssetManagerBase`; a packaged game reads through `VirtualFileSystem` (`.npak`) | Decided and built October 2026 (`docs/Runtime_And_Packaging_Plan_2026-10.md` R2, R4) |
| D10 | Budgets & targets | per-category VRAM shares, frame-time targets per reference scene | set after Phase 0 baselines |
| D11 | ~~ECS parallelism~~ **Decided** | **Declared system read/write sets** (`ComponentAccess` + `SystemGraph`, §5.2.6) | Decided September 2026 (Phase 1) |
| D12 | ~~Scene file format~~ **Decided** | **YAML per cell** (the scene file format; a cooked binary form waits for a binary scene form) | Decided October 2026 (§5.13.1); per-entity files if cells conflict in version control |
| D13 | ~~Barrier strategy~~ **Decided** | **Precise** per-resource barriers (Blanket kept as a switch) | Decided September 2026 (Phase 2): equal GPU/CPU cost on Bistro A/B/C; Precise gives drivers exact information and is checkable with synchronization validation (§5.4.11) |

*(Section reserved for the user's additional points — to be added during refinement.)*

---

## 8. References

- Unreal Engine — Nanite Technical Details: https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details
- Unreal Engine — Virtual Texture Memory Pools: https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-texture-memory-pools-in-unreal-engine
- Unreal Engine — Render Dependency Graph (RDG), Tasks System, Texture Streaming, World Partition, IoStore (Epic Developer Community documentation)
- Yuriy O'Donnell — "FrameGraph: Extensible Rendering Architecture in Frostbite" (GDC 2017)
- Vulkan Guide — Memory Allocation: https://docs.vulkan.org/guide/latest/memory_allocation.html
- Steven Tovey — Vulkan Memory Management (Vulkanised 2018): https://www.khronos.org/assets/uploads/developers/library/2018-vulkanised/03-Steven-Tovey-VulkanMemoryManagement_Vulkanised2018.pdf
- `VK_EXT_memory_budget`, `VK_KHR_unified_image_layouts`, `VK_KHR_acceleration_structure` (Vulkan specification)
- Vulkan Memory Allocator: https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
- Taskflow: https://taskflow.github.io/
- Tracy Profiler: https://github.com/wolfpld/tracy
- meshoptimizer (meshlets, simplification, clusterization): https://github.com/zeux/meshoptimizer
- Sebastian Aaltonen — OffsetAllocator (TLSF-style offset allocator): https://github.com/sebbbi/OffsetAllocator
- Vulkan Tutorial — Synchronization (Synchronization 2, timeline semaphores, frames in flight, async compute, transfer
  queues, host image copies, synchronization validation): https://docs.vulkan.org/tutorial/latest/Synchronization/introduction.html
- Vulkan Tutorial — Transfer Queues & Asset Streaming: https://docs.vulkan.org/tutorial/latest/Synchronization/Transfer_Queues_Streaming/01_introduction.html
- Vulkan Tutorial — Asynchronous Compute & Execution Overlap (for moving compute work such as culling, Hi-Z or skinning
  to a compute queue later): https://docs.vulkan.org/tutorial/latest/Synchronization/Async_Compute_Overlap/01_introduction.html

---

## 9. Deferred Work

Deliverables that were deliberately left out of the phase they belong to, so they are not lost. Each names why it waited
and what it needs; pick one up whenever its trigger is reached or it blocks something else.

| Item | From | Why it waited | What it needs | Pick up when |
|---|---|---|---|---|
| ~~**Animated skinned BLAS**~~ **Built** (§5.5.3, status 2026-10-20) | Phase 3 | Compute skinning Stage 1 now writes per-instance current/previous position-normal caches and raster consumes them; capture verification is pending. | Give each ray-traced skinned instance dynamic BLAS state and update/refit it from the current compute-skinned range after the skinning pass. | Stage 1 is verified and its measured cost/reuse benefit is acceptable. |
| ~~**A second real view**~~ **Built** (status 2026-10-20: the editor's extra views, up to 7 -- 4 viewports, thumbnails, material and animation previews -- each with its own culling, forward, post and overlay passes, `Renderer::SubmitExtraView`, `Renderer/Passes/ExtraViewPasses.cpp`; raster only) (§5.6) | Phase 3 | Culling and draws are per view already (`CullView`, `ViewDrawResources`, parameterised passes), but only the camera view is instantiated; the frozen culling view is a flag on that same view. | A second `CullView` with its own draw resources and graph passes, driven by its first real consumer. | A shadow-map view, a game-camera preview viewport, or reflection/probe capture needs its own culled draws. |
| **Geometry stream sizing** (§5.8.2) | Phase 4 | Streams start at the old page sizes and double; Bistro holds 259 MB committed for 154 MB used, which is 18 % of the geometry budget, so there is no pressure. | Initial capacities from the geometry category budget, or a trim to the used size after a load (one copy at a load boundary). | Geometry committed approaches its budget, or a scene loads with many grow-and-copy steps. |

### Viewport resize loop / camera jitter (2026-09-25)
Symptom: in the editor, moving the camera fast jittered in every mode (raster, path tracing, DLSS on or off), even with an empty scene, timings looked stable. Cause: `EditorLayer::OnUpdate` compared the ImGui panel size (float) with `Renderer::getViewPortSize()` (uint); with a fractional panel size (docking, DPI scaling) they were never equal, so `onViewportSizeChange` fired every frame and, after the 120 ms settle window, `applyRenderResolution()` ran again and again: `waitIdle`, `createSceneResources`, `ResetAllHistory` (ReSTIR, NRD, accumulation, DLSS), `initNRD`, and `Profiler::ResetStats()` (which hid the cost from Nox Stats). Fix: compare whole pixels in `EditorLayer`, ignore a request equal to the applied size in `Renderer::onViewportSizeChange`, and draw the viewport image at whole-pixel size. Also fixed along the way: background (sky) motion vectors were computed from the near-plane point instead of the ray direction (`GBufferMaterial.slang`) and were zero for path tracer primary misses (`PathTracerRayGen.slang`); the editor camera eases towards its mouse-driven target (`EditorCamera`).
**Correction (2026-09-25, confirmed by the user):** the viewport-resize loop above was real but not the cause of the camera jitter. The jitter (sky / environment, raster and DLSS alike, only while the camera moved) was **float precision in the pixel->ray unprojection**: `invViewProj` was the float inverse of the badly conditioned reverse-Z `proj * view`, and every ray was an unprojected NEAR-plane point (0.01 units from the eye) minus the camera position, which cancelled most float digits and changed frame to frame with the camera. Fixed by computing `invViewProj` in double from `view` and `proj` separately (`InvertViewProjection`, one source used by every pass) and unprojecting at reverse-Z depth 0.001 (about 10 units along the ray) in `Skybox.slang`, `GBufferMaterial.slang` (background motion), `PathTracer.slang` and `PathTracerBridge.slang`. Lesson: subtracting the camera position from a near-plane unprojection is not a safe way to get a direction. (Other things changed while chasing it and kept: rotation-only sky motion vectors, 4x4 footprint filtering for the mipless environment cubemap, an eased editor camera.)
