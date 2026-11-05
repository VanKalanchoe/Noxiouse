# NRI (Nox Rendering Interface) & Vulkan 1.4 Backend

Noxiouse enforces a strict **Zero Graphics API Leak** rule: all Vulkan headers (`vulkan/vulkan_raii.hpp`, `vk_mem_alloc.h`), **NVIDIA Streamline** (DLSS / DLSS-RR), and **NVIDIA NRD** (Real-Time Denoisers) are completely encapsulated inside `NoxCore/src/NRI/Vulkan/`. All engine and editor code interacts purely with the clean, modern C++20 `NRI` interfaces in `NoxCore/src/NRI/`.

---

## 1. Core Architectural Pillars of the Vulkan 1.4 Backend

### A. 100% Bindless via `VK_EXT_descriptor_heap`
Traditional Vulkan engines manage complex pools of `VkDescriptorSetLayout` and `VkDescriptorSet` objects. Noxiouse eliminates descriptor sets entirely:
- **Resource & Sampler Heaps ([`DescriptorHeapVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/DescriptorHeapVK.cpp))**: Allocates two persistently mapped GPU buffers with `VK_BUFFER_USAGE_2_DESCRIPTOR_HEAP_BIT_EXT`:
  1. **Resource Heap**:
     - **Image Partition (`[0 .. m_bufferHeapOffset)`)**: Starts at byte offset `0` so any `uint32_t` texture slot maps directly to `slot * imageDescriptorSize` (up to `65,536` textures/mips).
     - **Buffer / TLAS Partition**: Holds raw buffer and `VkAccelerationStructureKHR` descriptors.
     - **Hardware Reserved Range**: Reserved at the tail of the buffer for the GPU driver.
  2. **Sampler Heap**: Pre-populated at startup with 7 standard samplers at fixed indices `0..6` (`LinearRepeat`, `LinearClamp`, `NearestClamp`, `Aniso4xClamp`, `Aniso16xRepeat`, `NearestRepeat`, and `ShadowGreaterOrEqual` for Reverse-Z shadow comparison).
- **Push Data (`vkCmdPushDataEXT`)**: Per-draw and per-dispatch constants (up to 256 bytes, containing 64-bit `BufferDeviceAddress` pointers and bindless texture indices) are pushed directly via `CommandBuffer::pushData()` without needing a `VkPipelineLayout`.

### B. Unified Image Layouts (`VK_KHR_unified_image_layouts`)
- Every `TextureVK` image is transitioned from `VK_IMAGE_LAYOUT_UNDEFINED` to `VK_IMAGE_LAYOUT_GENERAL` once when created (or on the async transfer command buffer during initial upload) and **never changes layout again**.
- Pipeline barriers emitted by the Render Graph (`CommandBufferVK::resourceBarriers`) always specify `oldLayout = VK_IMAGE_LAYOUT_GENERAL` and `newLayout = VK_IMAGE_LAYOUT_GENERAL`, synchronizing purely on `VkPipelineStageFlags2` and `VkAccessFlags2`.

### C. Shader Objects (`VK_EXT_shader_object`) & Two-Level Caching
- **Runtime Slang Compiler ([`SlangCompiler.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/SlangCompiler.cpp))**: Compiles `.slang` files to `spirv_1_6` with capabilities `spvDescriptorHeapEXT`, `spvShaderClockKHR`, `SPV_EXT_mesh_shader`, `SPV_KHR_ray_tracing`, `SPV_KHR_ray_query`, and `SPV_KHR_compute_shader_derivatives`.
- **Two-Level Disk Cache ([`PipelineVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/PipelineVK.cpp))**:
  1. **`.spv` Cache (`cache/shaders/*.spv`)**: Keyed by a 64-bit FNV-1a hash of the shader source, transitive `#include`/`import` tree, entry point, stage, and preprocessor defines.
  2. **`.bin` Driver Binary Cache (`cache/shaders/*_<pipelineCacheUUID>.bin`)**: Saves the compiled `VkShaderEXT` driver binary (`NXSB` header) so subsequent launches skip both Slang and driver compilation.
- **Hot-Reload Crash Protection**: `PipelineVK` caches the last known good binary per stage (`s_lastWorkingBinaries`). If you make a syntax error while live-editing a `.slang` shader, the engine logs the Slang diagnostic and stays running on the previous working shader!

### D. Multi-Queue Execution & 64 KB Sparse Buffer Streaming
- **3 Hardware Queue Families ([`DeviceVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/DeviceVK.cpp))**:
  - `Graphics` Queue (Graphics + Compute + Present)
  - `Transfer` Queue (Dedicated DMA engine for lock-free background asset/mip/page uploads)
  - `Compute` Queue (Dedicated Async Compute queue synchronized via `TimelineSemaphoreVK` and `vkQueueSubmit2`)
- **Sparse Residency Buffers ([`BufferVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/BufferVK.cpp))**:
  - When supported by the GPU, `GeometryStreamer`'s cluster page pool is created as a single virtual `VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT` address space without upfront physical VRAM, binding and unbinding 64 KB VMA physical memory pages on demand via `vkQueueBindSparse`.

### E. Integrated NVIDIA Streamline (DLSS / DLSS-RR) & 11 NRD Denoiser Instances
- **NVIDIA Streamline (`DeviceVK::initStreamline`, `evaluateDLSS`)**:
  - Uses manual Vulkan hooking (`slSetVulkanInfo`) to evaluate **DLSS Super Resolution / DLAA** (`ePresetK`/`M`/`L`) and **DLSS Ray Reconstruction** (`ePresetE`) with full Reverse-Z depth, motion vectors, diffuse/specular albedos, normal+roughness, specular hit distance / virtual-image specular motion vectors, and `colorBeforeTransparency`.
  - Automatically binds a dummy empty descriptor set right before `slEvaluateFeature` so NGX's internal compute shaders coexist peacefully with `VK_EXT_descriptor_heap`.
- **NVIDIA NRD (`DeviceVK::initNRD`, `evaluateNRD*`)**:
  - Maintains **11 independent NRD denoiser identifiers** so every ray-traced feature has isolated temporal history:
    - `0`: `SIGMA_SHADOW` (Ray-traced directional soft shadows)
    - `1–2`: `REBLUR_SPECULAR` & `RELAX_SPECULAR` (RT Reflections)
    - `3–4`: `REBLUR_DIFFUSE` & `RELAX_DIFFUSE` (ReSTIR GI)
    - `5–6`: `REBLUR_DIFFUSE_SPECULAR` & `RELAX_DIFFUSE_SPECULAR` (ReSTIR DI)
    - `7–8`: `REBLUR_DIFFUSE` & `RELAX_DIFFUSE` (ReSTIR PT)
    - `9–10`: `REBLUR_DIFFUSE_SPECULAR` & `RELAX_DIFFUSE_SPECULAR` (Hardware Path Tracer)

---

## 2. File-by-File Reference: `NoxCore/src/NRI/` (20 Files)

| File | Plain-English Summary & Key Types |
| :--- | :--- |
| [`NRI.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/NRI.h) | Umbrella header that includes every public NRI header so engine code only needs `#include "NRI/NRI.h"`. |
| [`NRITypes.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/NRITypes.h) | Defines all API-agnostic graphics enums and structs: `BackendType`, `QueueType` (`Graphics`, `Transfer`, `Compute`), `TimelinePoint`, `ImageFormat` (including BC1/3/4/5/7 & depth formats), `TextureUsage`, `BufferUsage`, `PipelineStageFlags`, `AccessFlags`, `TextureBarrierDesc`, `BufferBarrierDesc`, `RenderingInfo`, `DLSSMode`, `DLSSPreset`, `DLSSInputs`, `NRDCommonSettings`, and `NRDSignalInput`. |
| [`Device.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Device.h) | Pure abstract factory and GPU manager interface (`NRI::Device`). Declares resource creation (`createBuffer`, `createTexture2D`, `createGraphicsPipeline`, `createComputePipeline`, `createRayTracingPipeline`, `createAccelerationStructure`, etc.), multi-queue submission, Sparse Buffer page binding (`updateSparseBuffer`), Streamline DLSS evaluation, and NRD denoiser dispatch. |
| [`Device.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Device.cpp) | Implements `NRI::Device::create()`, instantiating `DeviceVK` when `BackendType::Vulkan` is requested. |
| [`CommandAllocator.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/CommandAllocator.h) | Abstract command pool (`NRI::CommandAllocator`) supporting bulk pool reset (`CommandBufferReset::WithAllocator`) for worker threads or individual command buffer reset. |
| [`CommandBuffer.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/CommandBuffer.h) | Abstract command recording interface (`NRI::CommandBuffer`): dynamic rendering (`beginRendering`/`endRendering`), dynamic states, `bindPipeline`, `bindDescriptorHeaps`, `pushData` (with templated `pushConstants<T>`), classic draws, mesh shader dispatches (`drawMeshTasksEXT`, `drawMeshTasksIndirectEXT`, `drawMeshTasksIndirectCountEXT`), compute dispatches, hardware ray tracing (`traceRaysKHR`, `buildAccelerationStructuresKHR`), transfers, and `resourceBarriers`. |
| [`TimelineSemaphore.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/TimelineSemaphore.h) | Abstract monotonically increasing GPU/CPU timeline counter (`getValue()`, `wait(uint64_t)`). |
| [`Buffer.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Buffer.h) | Abstract GPU buffer (`NRI::Buffer`) supporting persistent CPU mapping (`map`/`unmap`/`uploadData`), 64-bit `getDeviceAddress()`, cross-queue sharing, and 64 KB sparse page ranges (`SparseBufferRange`). |
| [`Texture.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Texture.h) | Abstract 2D/Cube/Array texture (`NRI::Texture2D`) supporting mip generation, partial tile uploads (`TextureUploadRegion` for virtual texturing), GPU-to-CPU readback, blits, and Dear ImGui `getImTextureID()`. |
| [`Sampler.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Sampler.h) | Defines `SamplerDesc` (`Filter`, `AddressMode`, `MipmapMode`, `maxAnisotropy`, `CompareOp`) for registering custom samplers in the sampler heap. |
| [`DescriptorHeap.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/DescriptorHeap.h) | Abstract bindless descriptor heap (`NRI::DescriptorHeap`) with `DefaultSamplerSlot` constants (`LinearRepeat = 0` .. `ShadowLinear = 6`) and registration methods for textures, storage mips, buffers, TLASes, and samplers. |
| [`Pipeline.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Pipeline.h) | Defines `ShaderStageDesc`, `GraphicsPipelineDesc` (supporting both vertex and task+mesh pipelines), `ComputePipelineDesc`, and `RayTracingPipelineDesc`, plus `NRI::Pipeline`. |
| [`AccelerationStructure.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/AccelerationStructure.h) | Defines BLAS/TLAS geometry descriptors (`AccelerationStructureTriangles`, `AccelerationStructureInstances`, `AccelerationStructureBuildDesc`) and `NRI::AccelerationStructure`. |
| [`ShaderBindingTable.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/ShaderBindingTable.h) | Helper struct `ShaderBindingTableRegions` computing aligned GPU device address regions for RayGen, Miss, Hit, and Callable shader groups. |
| [`Swapchain.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Swapchain.h) | Abstract window swapchain (`acquireNextImage`, `present`, `recreate`, `setVSync`, `MAX_FRAMES_IN_FLIGHT = 2`). |
| [`QueryPool.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/QueryPool.h) | Abstract GPU query pool used for non-blocking readback of `AccelerationStructureCompactedSize`. |
| [`GpuProfiler.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/GpuProfiler.h) | Lock-free multi-threaded GPU timestamp and mesh/fragment pipeline statistics profiler interface (`allocateQueryPair`, `writeTimestamp`, `beginStatistics`/`endStatistics`, `calibrate`). |
| [`ShaderCompiler.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/ShaderCompiler.h) | Abstract interface for compiling shader source files into SPIR-V bytecode. |
| [`SlangCompiler.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/SlangCompiler.h) | Declares `SlangCompiler : public ShaderCompiler`, including lazy session creation (for `/DELAYLOAD:slang.dll`) and transitive dependency hashing (`sourceHash`). |
| [`SlangCompiler.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/SlangCompiler.cpp) | Implements Slang-to-SPIR-V 1.6 compilation, recursive `#include` and `import` file timestamp/content hashing for instant `.spv` cache invalidation, and detailed line-numbered compiler error reporting. |

---

## 3. File-by-File Reference: `NoxCore/src/NRI/Vulkan/` (27 Files)

| File | Plain-English Summary & Technical Highlights |
| :--- | :--- |
| [`VulkanCommon.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/VulkanCommon.h) | Configures `VULKAN_HPP_DISPATCH_LOADER_DYNAMIC` and includes `vulkan/vulkan_raii.hpp`. Strictly isolated to the `NRI/Vulkan/` directory. |
| [`DeviceVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/DeviceVK.h) & [`DeviceVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/DeviceVK.cpp) | Core Vulkan 1.4 backend implementation: initializes Streamline (`slInit` with DLSS-SR and DLSS-RR), enables Vulkan 1.4 + `descriptor_heap` + `unified_image_layouts` + `shader_object` + `mesh_shader` + ray tracing, manages Graphics/Transfer/Compute queues (`submit2`), executes `vkQueueBindSparse` page updates, integrates Dear ImGui, and manages all 11 NRD denoiser instances. |
| [`MemoryAllocatorVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/MemoryAllocatorVK.h) & [`MemoryAllocatorVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/MemoryAllocatorVK.cpp) | Integrates Vulkan Memory Allocator (`vk_mem_alloc_raii.hpp`) with `VK_KHR_maintenance5` (`BufferUsageFlags2`), `VK_EXT_memory_budget` VRAM tracking, 64 KB sparse page allocation (`allocateSparsePage`), and allocation ID leak debugging (`setLeakID`). |
| [`BufferVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/BufferVK.h) & [`BufferVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/BufferVK.cpp) | Implements `NRI::Buffer` with persistent host mapping, 64-bit `vkGetBufferDeviceAddress`, and `prepareSparseBinds` for committing/releasing 64 KB physical pages on sparse residency buffers. |
| [`TextureVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/TextureVK.h) & [`TextureVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/TextureVK.cpp) | Implements `NRI::Texture2D` in permanent `VK_IMAGE_LAYOUT_GENERAL`, supporting async transfer-queue uploads, virtual texture tile region uploads, cubemap/array mip uploads, mipmap generation, readbacks, and deferred ImGui descriptor release. |
| [`DescriptorHeapVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/DescriptorHeapVK.h) & [`DescriptorHeapVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/DescriptorHeapVK.cpp) | Implements `VK_EXT_descriptor_heap` via `writeResourceDescriptorsEXT` and `writeSamplerDescriptorsEXT`, with free-list slot recycling (`m_freeImageSlots`) and automatic cube-to-2DArray view wrapping for storage mips. |
| [`CommandAllocatorVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/CommandAllocatorVK.h) & [`CommandAllocatorVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/CommandAllocatorVK.cpp) | Wraps `vk::raii::CommandPool` for Graphics, Transfer, or Compute queues. |
| [`CommandBufferVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/CommandBufferVK.h) & [`CommandBufferVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/CommandBufferVK.cpp) | Implements all GPU command recording: dynamic rendering (`beginRendering`), `bindShadersEXT` / `bindPipeline`, `bindResourceHeapEXT` / `bindSamplerHeapEXT`, `pushDataInfoEXT`, mesh shader draws, BLAS/TLAS builds & compaction, `traceRaysKHR`, and `pipelineBarrier2`. |
| [`PipelineVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/PipelineVK.h) & [`PipelineVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/PipelineVK.cpp) | Compiles Graphics, Compute, and Ray Tracing pipelines using `VK_EXT_shader_object` (`createShadersEXT`) or `eDescriptorHeapEXT` `vk::Pipeline`s, backed by the two-level `.spv` + `.bin` disk cache and hot-reload fallback. |
| [`AccelerationStructureVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/AccelerationStructureVK.h) & [`AccelerationStructureVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/AccelerationStructureVK.cpp) | Wraps `vk::raii::AccelerationStructureKHR` (BLAS/TLAS) and exposes its 64-bit GPU device address. |
| [`QueryPoolVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/QueryPoolVK.h) & [`QueryPoolVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/QueryPoolVK.cpp) | Wraps `vk::raii::QueryPool` for non-blocking 64-bit availability readback of compacted BLAS sizes. |
| [`GpuProfilerVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/GpuProfilerVK.h) & [`GpuProfilerVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/GpuProfilerVK.cpp) | Triple-buffered GPU timestamp and task/mesh/fragment invocation query pools with `VK_EXT_calibrated_timestamps` CPU/GPU clock alignment and atomic query allocation for parallel worker threads. |
| [`TimelineSemaphoreVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/TimelineSemaphoreVK.h) & [`TimelineSemaphoreVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/TimelineSemaphoreVK.cpp) | Wraps `vk::raii::Semaphore` configured with `vk::SemaphoreType::eTimeline`. |
| [`SwapchainVK.h`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/SwapchainVK.h) & [`SwapchainVK.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/Vulkan/SwapchainVK.cpp) | Manages `vk::raii::SwapchainKHR`, `eImmediate`/`eMailbox`/`eFifo` present modes, per-frame fences/semaphores, and Streamline `slPresent` routing. |
