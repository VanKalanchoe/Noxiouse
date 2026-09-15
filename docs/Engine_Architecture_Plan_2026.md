# NoxEngine: Scalable Engine Architecture Plan (2026)

> **Document Version:** 0.1 — Draft for refinement (September 2026)
> **Scope:** Threading & job system, render graph, GPU-driven rendering, GPU memory & residency, asset
> pipeline, async loading, texture/geometry/world streaming, ray tracing at scale, profiling.
> **Target API:** Vulkan 1.4 through NRI — full extension/feature/shader baseline in §3 (Renderer Technology Baseline)
> **Companion documents:**
> - `docs/RayTracing_Architecture_Plan_2026.md` — lighting, GI, denoising, RTXDI/NRD/DLSS (unchanged by this plan)
> - `architecture.md` — earlier NRI / RenderGraph / TaskGraph proposals (v2/v3) and the completed
>   visibility-buffer migration record. Where this document differs, this document wins (see §5.4.6).
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
- Networking, audio, scripting runtime, physics beyond the existing Box2D integration.
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

Differences from the design above: GPU samples are not matched to CPU frame indices yet (GPU windows are kept
separately); stats reset on scene new/open and render-resolution changes (not on every denoiser toggle).
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
   into per-worker command buffers and applied on the main thread at the frame sync point.
3. **Vulkan:** queue submit and present only on the main thread; command recording on any worker using
   that worker's pool; NRI resource creation is thread-safe through an allocation lock; GPU object
   release goes through the deferred release queue (§5.8.4) processed at the frame sync point.
4. **Blocking** only on the main thread, only at defined points: frame graph completion, present,
   frame-in-flight backpressure.
5. **Cross-task data** flows through graph edges, per-frame snapshot structures, or lock-free queues —
   no shared mutable state without a declared owner.

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
and compare CPU record time, GPU frame time and frame pacing using §5.1. Default to Blanket until data
shows Precise is better (Open Decision D13).

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

### 5.6 GPU-Driven Culling & Draw Generation

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
- **Ray tracing geometry** uses a coarser, resident fallback LOD per mesh (§5.14).

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
- **Later (D5):** reserve a sparse-bound buffer at the full category budget and commit memory on demand
  instead of copying. Streaming pools for cluster pages (Phase 6b) are fixed-size at the budget from the
  start (UE Nanite streaming pool model).
- **Freeing:** ranges return to the allocator after frames-in-flight (§5.8.4). Unlike pages, freed
  memory is immediately reusable by the next load.
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
- Completion tracked with fences polled on the main thread at frame start — no `submitAndWait` on hot paths
  (today's single-time command buffers remain only for startup/tools).
- BLAS builds queued with per-frame primitive budgets (generalizing this session's upload batching).

#### 5.11.5 Editor integration
Drag-and-drop import/load becomes asynchronous: the entity hierarchy is created immediately with
placeholder bounds; geometry/textures appear when ready; progress shown in a status bar.

### 5.12 Texture Streaming

#### 5.12.1 Approach (Open Decision D3): mip streaming first, virtual texturing later
- **Mip streaming (UE Texture Streaming model):** each texture has resident mip range
  `[firstResidentMip, lastMip]`; mip tail (e.g. ≤ 64²) always resident.
- **Demand:** GPU computes required mip per material/texture from screen-space UV derivatives
  (written to a small feedback buffer), combined with CPU distance heuristics for not-yet-visible
  content.
- **Update:** new mips uploaded into a new image or into a pre-allocated full image with per-mip
  memory commit; shader sampling clamped via a per-texture min-LOD value in the material buffer to
  avoid sampling non-resident mips (bindless-friendly, no descriptor churn).
- **Budget:** texture pool share of VRAM; over budget drops highest mips of lowest-priority textures.

#### 5.12.2 Virtual texturing (later)
Page-table indirection textures + physical page cache (UE Virtual Texture pools): for terrain,
decals and very large unique textures where mip streaming granularity is insufficient.

### 5.13 World Streaming

- **World partition grid:** world cooked into cells (entities, components, cell-local asset
  references); cells load/unload by distance to streaming sources (camera, gameplay anchors) with
  hysteresis and per-frame instantiation budgets.
- **HLOD:** merged proxy meshes/materials for distant cells so far content stays visible when cells are
  unloaded.
- **Activation stages:** loaded (data in memory) → active (rendering) → simulated (physics/scripts).
- **Editor:** full world view with cell visualization; editing loads required cells.
- **Scene serialization** moves from one YAML file to per-cell files for streaming and version control.

### 5.14 Ray Tracing at Scale

- **TLAS from GPU scene** instances (not render queues), with RT-specific culling: distance, screen
  size, importance, and budgets for instance count.
- **BLAS per mesh/LOD** (shared across instances), built through the upload manager queue with
  per-frame budgets; **compaction** after build; refit for skinned/deforming meshes.
- **RT LOD:** coarse resident fallback geometry for off-screen/secondary rays; visible-surface
  consistency handled by ray origin offsets / matching LOD near the camera (Open Decision D8).
- **Memory:** RT category budget; eviction of BLAS for far/invisible meshes, rebuild on demand.
- Lighting/GI features and their arbitration remain defined by the RT plan.

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
| **1** | Task system & frame graph | 0 | `Nox::JobSystem` over one Taskflow executor (+ IO executor), frame graph (Game Update → Streaming → Render Prepare), worker-local arenas + NRI command pools, ECS access declarations + deferred structural changes, Taskflow observer → profiling scopes, DOT dump command, frame-ID readbacks | Frame work spreads across all cores (visible in Nox Stats/Tracy); frame graph DOT viewable in GraphViz; no `std::thread` outside `JobSystem`; picking correct |
| **2** | Render graph | 1 | RG core (declare/compile/execute, pass culling), transient pool + aliasing, history resources, derived pass setup, access-model synchronization with Blanket/Precise strategies, parallel partition recording, per-pass profiling scopes + debug labels, visualizer + texture inspection + validation; full frame ported | Identical images in all debug views; no manual `executionBarrier()` or reset flags left; transient memory reduced vs today; recording parallel; Blanket vs Precise barrier comparison recorded (D13) |
| **3** | GPU scene & GPU culling | 2 | Persistent instance/transform/material buffers, dirty uploads, **mesh instancing (importer keeps glTF mesh → node sharing; instances reference one `MeshTable` entry, one BLAS per unique mesh — Bistro: 551 meshes for 2909 instances, today uploaded 2909×)**, Hi-Z, GPU instance culling, indirect-count draws, multi-view | CPU per-frame render prep independent of instance count; Bistro frame time improved (target set after Phase 0 baseline) |
| **4** | Unified geometry memory & budgets | 3 | Base memory system (§5.8.6): unified geometry buffers incl. RT index stream with TLSF range allocation (page tables removed, offsets instead of addresses), grow-and-copy, memory budget polling, category accounting with soft budgets, no per-frame buffer creation, single deferred release queue | Freed geometry is reused by the next load; VRAM per category visible in Nox Stats; no `createBuffer` in a steady-state frame |
| **5** | Async IO & upload manager | 1, 4 | Request API + state machine, IO threads, decode tasks, transfer queue, staging ring, async editor drag-in | Loading Bistro never stalls the viewport beyond a frame budget |
| **6a** | Texture streaming | 5 | BC cooking for all sources, mip tail, GPU mip feedback, residency + eviction | Scene textures exceeding the texture budget render correctly with mip reduction |
| **6b** | Geometry streaming & LOD | 5 | Cluster hierarchy cook, GPU cut selection, page streaming pool, RT fallback LOD | Geometry exceeding the geometry budget renders with streaming; no popping beyond threshold |
| **6c** | World streaming | 6a, 6b | World partition cooking, cell streaming, HLOD, per-cell serialization | Test world larger than RAM/VRAM traversable without stalls |
| **7** | Continuous optimization | all | Profile-driven work, VT, DirectStorage, async compute expansion | Ongoing |

Phases 1, 2 and 3 are architecture-defining and each gets its own plan-mode design before code.

---

## 7. Open Decisions

| ID | Decision | Options | Notes |
|---|---|---|---|
| D1 | ~~Render thread model~~ **Decided** | **Main thread (window, input, submit/present) + Taskflow task graph for everything else; no dedicated render thread.** Frame pipelining stays available as a graph shape (§5.3). | Decided September 2026 — see §5.2.1 |
| D2 | RG resource naming/API style | handles returned by builder (Frostbite) vs string-keyed blackboard (UE RDG uses both) | affects pass authoring ergonomics |
| D3 | Texture streaming | mip streaming first vs virtual texturing first | recommendation: mip streaming first |
| D4 | Geometry LOD | Nanite-like cluster DAG vs discrete LODs first | DAG is more work; discrete LODs as stepping stone? |
| D5 | Geometry buffer growth | grow-and-copy vs sparse binding reserve/commit | Base (Phase 4) uses grow-and-copy at load boundaries; sparse evaluated later, needs driver support validation |
| D6 | Additional queues | today: one combined graphics/compute queue (§3.1) · add dedicated transfer queue for uploads · add async compute queue(s) for culling/RT/denoising | requires multi-queue device creation in NRI, queue-family detection, cross-queue sync2 barriers, per-queue command pools |
| D7 | IO backend | IoRing/overlapped IO vs DirectStorage | DirectStorage enables GPU decompression |
| D8 | RT geometry LOD policy | shared raster LOD vs dedicated coarse RT LOD | self-intersection vs memory trade-off |
| D9 | Editor vs runtime asset managers | split now vs after async loading | runtime needs containers + handle-based refs |
| D10 | Budgets & targets | per-category VRAM shares, frame-time targets per reference scene | set after Phase 0 baselines |
| D11 | ECS parallelism | declared system read/write sets vs coarse phases | affects gameplay code authoring |
| D12 | Scene file format | per-cell YAML vs binary cooked cells with YAML for editing | version control friendliness |
| D13 | Barrier strategy | Blanket (today's `executionBarrier`) vs Precise per-resource barriers | Decided by the Phase 2 profiling experiment (§5.4.5); Blanket is the default until then |

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
