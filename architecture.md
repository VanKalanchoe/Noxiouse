# NoxEngine Rendering Architecture: NRI, Render Graph and Job System

> **Last checked against the code:** 2026-10-20.
> This file describes what is built. The engine overview, directory map and frame flow are in `PROJECT_CONTEXT.md`; the
> device baseline (every extension and feature and why) is in `docs/Engine_Architecture_Plan_2026.md` §3, and the design and
> measurements of the render graph in its §5.4. The v2 / v3 proposals that used to fill this file are summarized in
> section 7.

---

## 1. Layers

```text
Scene / Editor / Runtime
        |  BeginScene / Submit* / SyncGpuScene (Scene.cpp)
        v
Renderer (NoxCore/src/NoxCore/Renderer)       GpuScene, UploadManager, GeometryArena, TextureStreamer, MemoryBudget
        |  add*Pass functions (Renderer/Passes/*.cpp)
        v
RenderGraph (NoxCore/src/NoxCore/RenderGraph)  declare -> Compile -> Execute
        |  NRI::CommandBuffer, NRI::Texture2D, NRI::Buffer, ...        records in parallel on JobSystem workers
        v
NRI (NoxCore/src/NRI)                          abstract interfaces, no Vulkan types
        |
        v
NRI Vulkan backend (NoxCore/src/NRI/Vulkan)    Vulkan 1.4, VMA, Streamline, NRD
```

Rules that hold across the layers:

- **Zero graphics leak.** Vulkan headers, `vk::` types, `vk_mem_alloc.h`, Streamline and NRD appear only in
  `NoxCore/src/NRI/Vulkan/`. The renderer and everything above it use NRI types only.
- **Single source of truth.** Each subsystem tracks only what it owns. `DeviceVK` keeps no pointer to the window after
  creation; SDL3 (`SDL_Window*`) is the platform handle the swapchain is created from.
- **Abstract factory.** `NRI::Device` creates buffers, textures, pipelines, acceleration structures, query pools, timeline
  semaphores and command allocators. Only one backend exists (Vulkan); a Metal device would be another implementation.

---

## 2. NRI (`NoxCore/src/NRI`)

| Interface | File | Vulkan implementation |
|---|---|---|
| `Device` | `Device.h` | `DeviceVK` (instance, device, queues, swapchain creation, Streamline / DLSS, NRD) |
| `CommandBuffer`, `CommandAllocator` | `CommandBuffer.h`, `CommandAllocator.h` | `CommandBufferVK`, `CommandAllocatorVK` |
| `Buffer`, `Texture2D` | `Buffer.h`, `Texture.h` | `BufferVK`, `TextureVK` (VMA through `MemoryAllocatorVK`) |
| `DescriptorHeap` | `DescriptorHeap.h` | `DescriptorHeapVK` |
| `Pipeline` | `Pipeline.h` | `PipelineVK` (shader objects when supported, else classic pipelines; ray tracing pipelines) |
| `AccelerationStructure`, `ShaderBindingTable` | `AccelerationStructure.h`, `ShaderBindingTable.h` | `AccelerationStructureVK` |
| `QueryPool`, `GpuProfiler`, `TimelineSemaphore` | `QueryPool.h`, `GpuProfiler.h`, `TimelineSemaphore.h` | `QueryPoolVK`, `GpuProfilerVK`, `TimelineSemaphoreVK` |
| `Swapchain` | `Swapchain.h` | `SwapchainVK` |
| `ShaderCompiler` | `ShaderCompiler.h`, `SlangCompiler.h/.cpp` | Slang -> SPIR-V 1.6 (not backend specific) |

### 2.1 Device baseline (short form)

- **Vulkan 1.4** (`vk::ApiVersion14`), `vk::raii` wrappers, VMA.
- **Queues:** one graphics + compute + present queue and a transfer queue for uploads (a dedicated family when the GPU has
  one, else the graphics queue).
- **Required:** `VK_EXT_descriptor_heap`, `VK_KHR_unified_image_layouts`, buffer device address, `VK_EXT_mesh_shader`,
  `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_ray_tracing_pipeline`, compute shader derivatives,
  synchronization2, dynamic rendering, extended dynamic state 3, vertex input dynamic state, `VK_KHR_maintenance5`,
  `VK_KHR_shader_untyped_pointers`.
- **Optional:** `VK_EXT_shader_object` (classic pipelines are built from the same descriptions when it is missing),
  `VK_EXT_memory_budget`.
- Source of truth: `NoxCore/src/NRI/Vulkan/DeviceVK.cpp` (extension list and feature chain). Full table:
  `docs/Engine_Architecture_Plan_2026.md` §3.

### 2.2 Bindless with descriptor heaps

The engine uses **`VK_EXT_descriptor_heap`**, not `VK_EXT_descriptor_buffer` and not descriptor sets.

1. The renderer creates one global **resource heap** and one **sampler heap** (`NRI::DescriptorHeap`, `m_resourceHeap`,
   `m_samplerHeap`). Every command buffer binds them once at its start (`CommandBuffer::bindDescriptorHeaps` ->
   `vkCmdBindResourceHeapEXT` / `vkCmdBindSamplerHeapEXT`), from the render graph's command buffer setup
   (`Renderer::applyCommandBufferBaseline`).
2. A texture is registered once and gets a **slot**: a sampled slot, and per-mip storage slots for storage textures. Render
   graph textures keep their slots while their pool entry lives.
3. Buffers are not bound at all: shaders get their **device addresses** (buffer device address) and read them as typed
   pointers (`UniformBufferObject*`, `InstanceData*`, ...).
4. A pass passes slots and addresses in a **push constant** struct defined in `shaderIO.h`. Slang reads a texture by slot:
   `Texture2D<float4>.Handle(uint2(pc.sourceIndex, 0))`, samplers likewise with `SamplerState.Handle(...)`.

Consequences: no descriptor set layouts, pools or per-pass binding code; a freed slot must not be reused while a frame in
flight can still read it (deferred release). Classic descriptor sets exist only inside `DeviceVK`: the Dear ImGui Vulkan
backend gets its own descriptor pool, and a dummy set is bound before DLSS evaluation as a workaround for a Streamline bug
(`DeviceVK.h`, `ensureDummyDescriptorSet`).

### 2.3 Images and synchronization

- With `VK_KHR_unified_image_layouts` every image stays in `General` for its whole life. There are **no layout transitions**
  except the swapchain image for present, which the graph's present pass handles.
- Barriers are synchronization2 memory barriers only. Passes do not place them by hand: the render graph derives them from
  declared accesses (section 3.3). Acceleration structure builds use `CommandBuffer::accelerationStructureBarrier`
  (`BuildToBuild`, `BuildToShaderRead`, `TransferToBuild`).

### 2.4 Shaders

- Slang, compiled at run time by `SlangCompiler` (SPIR-V 1.6 emitted directly, scalar layout, column-major matrices,
  `spvDescriptorHeapEXT`, `spvRayQueryKHR`, RTXDI include paths). One file may hold task, mesh, fragment and compute entry
  points.
- Binaries are cached by a hash of the source and its includes (`SlangCompiler::sourceHash`): shader-object binaries and
  SPIR-V (`.spv`). A packaged game can run from that cache without Slang (Slang is delay-loaded on Windows).
- Hot reload: the renderer watches shader files (`watchShader` in `Renderer.cpp`) and recreates the affected pipelines at
  the start of the next frame.
- Structs shared with C++ live in `NoxEditor/assets/shaders/shaderIO.h` and must be layout-identical in C++ and Slang.

---

## 3. Render graph (`NoxCore/src/NoxCore/RenderGraph`)

The render graph is rebuilt every frame from pass declarations. It schedules GPU work and owns frame-lifetime GPU
resources; persistent data (GPU scene, geometry, textures, TLAS) belongs to other systems and is imported each frame.

### 3.1 Declaring passes

```cpp
// Sketch of a compute pass (field names shortened; see Renderer/Passes/*.cpp for real ones).
m_renderGraph.AddPass("SSAO", RGPassFlags::None,
    [&](RGBuilder& builder)                 // setup: main thread, every CPU decision happens here
    {
        builder.Read(depth);                 // RGTextureAccess::Sampled by default
        builder.Write(ssao);                 // RGTextureAccess::StorageWrite by default
    },
    [=](RGPassContext& context)              // execute: records only, may run on any worker thread
    {
        NRI::CommandBuffer& cmd = context.Cmd();
        push.depthIndex = context.Slot(depth);
        push.outputIndex = context.StorageSlot(ssao);
        // bind pipeline, pushData, dispatch
    });
```

- **Pass flags:** `Raster` (the graph begins and ends rendering with the declared color / depth targets, viewport and scissor
  from the render area, flipped Y by default), `NeverCull` (readbacks, present, CPU-visible side effects), `RayTracing`.
- **Accesses:** textures `Sampled`, `StorageRead`, `StorageWrite`, `ColorTarget`, `DepthTarget`, `CopySource`,
  `CopyDestination`; buffers `Read`, `IndirectRead`, `Write`, acceleration structure build input, and others (`RGTypes.h`).
- **Resources:** `CreateTexture` / `CreateBuffer` (transient, sized at render resolution, output resolution or absolute,
  allocated only if an executed pass uses them), `GetHistoryTexture` / `GetHistoryBuffer` (persistent under a key, rotated,
  reset by `ResetHistory(key)`; NRD and DLSS read the same reset through `WasHistoryReset`), `ImportTexture` /
  `ImportBuffer` (owned elsewhere).
- **Blackboard:** typed structs (`RGBlackboard`, `Passes/FrameGraphResources.h`) carry handles between the `add*Pass`
  functions.
- **Groups:** `PushGroup` / `PopGroup` nest passes under a profiler scope ("ReSTIR GI", ...).

### 3.2 Pass order

Passes execute in the order they are added; there is no automatic reordering. `Renderer::recordFrame` calls the `add*Pass`
functions in a fixed frame order (see `PROJECT_CONTEXT.md` §3). A feature that is off adds no passes. The pass code is
split by area:

| File | Passes |
|---|---|
| `Renderer/Passes/RasterPasses.cpp` | GPU scene update, compute skinning, animated BLAS update, BLAS build and compaction, TLAS build, instance culling dispatches (early / late), visibility (early / late), Hi-Z build, G-buffer, forward 3D, forward transparent / OIT and its composite, particles, refraction source, cull / cluster / mip feedback readbacks |
| `Renderer/Passes/LightingPasses.cpp` | deferred lighting, SSAO, RTAO, occlusion blur, path tracer (environment importance, path trace, NRD PT, composite) |
| `Renderer/Passes/RTPasses.cpp` | NRD guides, RT shadows + NRD, RT reflections + NRD, RT upsampling, DDGI (radiance, relocation, blend), screen-probe GI (place, trace, irradiance, filter, integrate) |
| `Renderer/Passes/ReSTIRPasses.cpp` | light PDF and presampling (RIS, ReGIR), ReSTIR DI, ReSTIR GI, ReSTIR PT, previous-frame copy, NRD for DI / GI / PT |
| `Renderer/Passes/UpscalePasses.cpp` | DLSS-RR guides, DLSS, TAA, entity / depth blits for the editor |
| `Renderer/Passes/PostPasses.cpp` | auto exposure, post process (tonemapping), 2D overlay, selection mask, outline |
| `Renderer/Passes/EditorPasses.cpp` | ImGui / present, editor raycasts, pick readback, texture readbacks |
| `Renderer/Passes/ExtraViewPasses.cpp` | the editor's extra views: culling, forward, particles, post, outline, overlay per view |
| `Renderer/Renderer.cpp` | procedural sky, cloud shadows, aerial perspective |

There are 89 `AddPass` call sites in these files.

### 3.3 Compile

1. **Culling, back to front:** a pass is kept if it is `NeverCull`, targets the swapchain, writes an imported resource, or
   writes something a kept pass reads.
2. **Resources:** transients come from `RGResourcePool`, keyed by resolved size, format, usage and mips, and are **aliased**
   between passes whose lifetimes do not overlap. Unused pool entries are released after a delay through the deferred
   release queue.
3. **Synchronization, front to back:** where a resource written by one pass is accessed by a later executed pass, the graph
   records a barrier. `RGSynchronization::Precise` (the default, decision D13) records per-resource
   `ImageMemoryBarrier2` / `BufferMemoryBarrier2` with exact stages and accesses (layouts stay `General`) at the start of
   the consuming pass; `Blanket` records one global barrier instead and is kept as a switch.

### 3.4 Execute

- The executed passes are split into consecutive **chunks**, one command buffer each, submitted together in order. The
  chunk count is chosen from each pass's measured record time.
- Chunks record in parallel as `JobSystem` tasks, each worker with its own NRI command pool per frame slot. Passes that call
  into libraries that are not thread-safe (NRD, Streamline) declare an exclusive key (`RGBuilder::RecordExclusive`) so they
  never record at the same time.
- Every pass gets a CPU and a GPU profiler scope and a debug label.
- `Renderer::setParallelCommandRecording(false)` records the whole frame into one command buffer on the calling thread.

### 3.5 Tooling

- **Render Graph panel** (editor, `NoxEditor/src/Panels/RenderGraphPanel.cpp`, filled from `RGFrameReport`): passes in order
  with groups, culled state, command buffer, CPU and GPU time, accesses; resources with sizes, memory, lifetimes and
  aliasing; history resets; DOT export.
- **Texture inspection:** any graph texture of the frame can be shown (`FindTexture` / `InspectTexture`), recorded right
  after its last writer.
- **Validation** (`RGValidation.cpp`): undeclared resource use in execute callbacks, stale handles, reads of transients nobody
  wrote this frame, writes to read-only imports, a frame without a presenting pass.

---

## 4. GPU scene and draw generation

- `Renderer/GpuScene` keeps persistent tables of instances, transforms (world, normal, previous world for motion vectors),
  shared materials, meshes and per-instance ray tracing records, with CPU mirrors. Changed slots are scattered from
  per-frame staging in the first graph pass ("GPU Scene Update"). The scene feeds it changes (`Scene::SyncGpuScene`);
  meshes are not resubmitted every frame.
- Draws are grouped in buckets (`RenderBucket`: opaque, masked, unlit, transparent, each single or double sided). GPU culling
  (`InstanceCulling.slang`) writes visible instance lists and indirect commands per bucket; raster passes draw with
  `drawMeshTasksIndirectCount`. Two-phase Hi-Z occlusion culling re-tests the early pass's rejects after the pyramid is
  rebuilt.
- The task shader (`Material_PBR_MeshTask.slang`) chooses the cluster LOD cut and culls meshlets; mesh shaders pull vertices
  through device addresses from the unified geometry buffers (`GeometryArena`).
- Ray tracing: one BLAS per unique mesh, compacted after build; skinned instances get their own BLAS, rebuilt or refit
  after compute skinning ("Animated BLAS Update"); the TLAS is rebuilt when ray-traced instances change.

---

## 5. Job system (`NoxCore/src/NoxCore/Tasks`)

- `JobSystem` owns a Taskflow executor for frame work and a second one for IO, worker-local state (`WorkerLocal`) and
  per-frame arenas (`FrameArena`), reset at the end of every frame.
- `SystemGraph` runs ECS systems as a Taskflow graph built from their declared reads and writes (`ComponentAccess`).
- Users: the scene's update and submit graphs, asset loading and decoding, texture streaming, Jolt (`NoxJoltJobSystem`) and
  render graph recording. (C# script jobs, `Nox.Jobs`, run on .NET's own thread pool, not here.)
- There is no render thread (decision D1): the main thread owns the window, input, submission and present.

---

## 6. Renderer (`NoxCore/src/NoxCore/Renderer/Renderer.cpp`)

`Renderer` owns the device, the swapchain, pipelines and settings, and builds each frame's graph. `drawFrame`: wait for
the frame slot and acquire the image, run deferred releases and the upload manager, read back the previous results of this
slot, `recordFrame` (build, `Compile`, `Execute`), submit and present. Feature switches (`setDiffuseGIMode`,
`setDirectLightingMode`, `setPathTracingEnabled`, upscale mode, quality presets, ...) only decide which passes the next
frame adds.

---

## 7. History: the v2 / v3 proposals

Earlier versions of this file proposed the NRI and render graph design (v2, then v3) before they were built. What came of
them:

- **Kept:** a pure abstract NRI with one backend per API; the render graph as a compiler (passes declare reads and writes,
  the graph culls, allocates, aliases and inserts synchronization); Taskflow for execution; RAII ownership.
- **Changed:**
  - Descriptor buffers (`VK_EXT_descriptor_buffer`) -> descriptor heaps (`VK_EXT_descriptor_heap`), section 2.2.
  - The `NRI::ResourceState` layout model -> an access model with no layouts (`VK_KHR_unified_image_layouts`); NRI
    `ResourceState` now carries access and stage bits for precise barriers.
  - The separate `Tasking/TaskGraph` with `RenderJob`s and semaphores -> `JobSystem` plus command buffer chunks that the
    render graph records in parallel and submits in order on the main thread.
  - RAII render pass scopes -> the graph begins and ends rendering itself from declared targets (`RGPassFlags::Raster`).
  - Directory layout: the graph is `NoxCore/src/NoxCore/RenderGraph/`, the job system `NoxCore/src/NoxCore/Tasks/`, the
    backend folder `NRI/Vulkan/`.
  - C++23 -> the build is C++20.
- The visibility-buffer migration record (forward -> visibility buffer + G-buffer, steps 0-7) is summarized in
  `PROJECT_CONTEXT.md` §7.
