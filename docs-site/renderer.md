# Hybrid Visibility-Buffer, Ray-Traced & Path-Traced Renderer (`NoxCore/Renderer`)

Noxiouse features a **GPU-driven, bindless, hybrid deferred visibility-buffer + hardware ray-tracing + path-tracing renderer** located in `NoxCore/src/NoxCore/Renderer/`. Every rendering stage is declared as a pass in the frame-compiled `RenderGraph`.

---

## 1. Plain-English Tour of Every Rendering Feature

### A. Unified Geometry Arena, 36-Byte Packed Vertices & Nanite-Style Cluster LOD Streaming
- **36-Byte `PackedVertex` ([`DataTypes.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/DataTypes.h))**: Every static vertex is compressed into 36 bytes: FP32 `pos[3]` (12B), oct-encoded `SNORM16x2` normal + `SNORM16x2` tangent + `uint32` signs/flags (12B), and `float16x2` `UV0` + `float16x2` `UV1` (8B). Optional RGBA8 vertex colors and 16-byte `SkinVertex` weights live in companion streams.
- **Unified Geometry Arena ([`GeometryArena.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/GeometryArena.cpp))**: Instead of binding per-mesh vertex/index buffers, all meshlets across the entire scene are sub-allocated via `OffsetAllocator` into global GPU storage buffers (`m_VertexPool`, `m_MeshletTrianglePool`, `m_MeshletVertexPool`, `m_MeshletDescriptors`, `m_MeshletBounds`, `m_RtIndexPool`).
- **Nanite-Style Cluster DAG Streaming ([`GeometryStreamer.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/GeometryStreamer.cpp))**:
  - Static meshes are cooked into a hierarchy (DAG) of 64-vertex / 124-triangle meshlet clusters packed into **128 KB pages**.
  - Only coarse **root pages** are uploaded when a mesh loads.
  - Every frame, [`GeometryStreamingFeedback.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/GeometryStreamingFeedback.slang) checks screen-space error bounds (`lodBoundsError`) and appends requests for finer pages to a readback buffer.
  - `GeometryStreamer` streams missing pages from `.nsmesh` files into a **64 KB Sparse Residency GPU Buffer** (`vkQueueBindSparse`), patches cluster pointers on the GPU, and dynamically rebuilds ray-tracing BLASes (`ClusterIndices.slang`) to match the currently resident leaf cut!

### B. Two-Phase GPU Frustum & Reverse-Z Hi-Z Occlusion Culling
- **Phase 1 Culling ([`InstanceCulling.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/InstanceCulling.slang))**: Tests every instance in `GpuScene` against the camera frustum and the **previous frame's** Reverse-Z Hierarchical Depth Pyramid (Hi-Z). Visible instances are compacted into indirect mesh-shader dispatch commands (`vkCmdDrawMeshTasksIndirectCountEXT`). Instances occluded only by last frame's Hi-Z are parked in a **Late Retest Queue**.
- **Phase 1 Visibility & Hi-Z Mip Build ([`HiZBuild.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/HiZBuild.slang))**: Phase 1 meshlets rasterize into the depth buffer, and `HiZBuild.slang` downsamples a fresh Reverse-Z farthest-depth mip chain.
- **Phase 2 Late Culling & Disocclusion Fill**: Re-tests the parked instances against the **current frame's** Hi-Z pyramid and immediately rasterizes newly disoccluded objects — eliminating 1-frame pop-in around fast-moving cameras or corners!

### C. 64-Bit Visibility Buffer & Ray-Differential G-Buffer Resolve
- **Visibility Rasterization ([`Material_PBR_MeshTask.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Material_PBR_MeshTask.slang) + [`VisibilityBuffer.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/VisibilityBuffer.slang))**:
  - The task stage performs cluster DAG cut selection and per-meshlet frustum culling.
  - The mesh & fragment stages rasterize opaque and alpha-cutout triangles into a tiny **64-bit `R32G32_UINT` Visibility Buffer** storing `(instanceSlot + 1, (meshletID << 8) | primitiveID)`, plus `R32_SINT` `entityID` and `R16G16_SFLOAT` screen-space velocity (combining camera, object, and skeletal bone motion).
- **Fullscreen G-Buffer Material Resolve ([`GBufferMaterial.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/GBufferMaterial.slang))**:
  - Evaluates materials **once per visible screen pixel** (zero overdraw).
  - Reconstructs triangle vertices from the visibility ID and computes **analytic ray-differential barycentrics** (`InterpAttributes`) for exact screen-space UV gradients (`ddx`/`ddy`).
  - Applies **Tokuyoshi & Kaplanyan 2021 Geometric Specular Anti-Aliasing** (`geometric_specular_aa`) to prevent distant normal-map shimmering.
  - Writes up to 8 MRTs including oct-encoded shading/geometry normals, albedo, metallic/roughness/AO, emissive, diffuse transmission, and packed **OpenPBR / glTF 2.0 extension lobes**.

### D. Streaming Virtual Texturing (SVT) & Feedback-Driven Mip Streaming
- ** GPU Feedback ([`MipFeedback.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/MipFeedback.slang))**: Uses wave-level deduplication (`WaveReadLaneFirst`) and `InterlockedMin` to report the finest mip or virtual tile needed by visible pixels.
- **Virtual Texture Streamer ([`VirtualTextureStreamer.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/VirtualTextureStreamer.cpp))**: Manages physical 4096x4096 tile cache atlases (`128x128` tiles + 4px border = `136x136` texels per slot) for BC7 sRGB, BC7 Linear, BC5 Normal, BC4 Grayscale, and RGBA8 formats, updating indirection page tables and evicting cold tiles via LRU.

### E. Full glTF 2.0 & OpenPBR Multi-Lobe Material Model
Both the deferred/forward rasterizer and the hardware Path Tracer evaluate a unified multi-lobe BSDF ([`MaterialLobes.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/MaterialLobes.slang), [`MaterialLobeMath.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/MaterialLobeMath.slang), [`PBRLighting.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PBRLighting.slang)):
- Metallic-Roughness & Specular-Glossiness (`KHR_materials_pbrSpecularGlossiness`)
- Clearcoat top layer (`KHR_materials_clearcoat`) with independent clearcoat normal map
- Charlie velvet/cloth sheen (`KHR_materials_sheen`)
- Belcour & Barla thin-film iridescence (`KHR_materials_iridescence`)
- Anisotropic GGX microfacet reflection (`KHR_materials_anisotropy`)
- Custom dielectric F0 tint & strength (`KHR_materials_specular`, `KHR_materials_ior`)
- Dielectric transmission, Beer-Lambert interior volume absorption (`KHR_materials_transmission`, `KHR_materials_volume`), and chromatic dispersion (`KHR_materials_dispersion`)
- Backlit foliage subsurface diffuse transmission (`KHR_materials_diffuse_transmission`)

### F. Decoupled Hardware Ray Tracing, GI & ReSTIR Suite
Noxiouse supports multiple combinable real-time ray-tracing and global illumination pipelines:
1. **Decoupled RT Shadows ([`ShadowMask.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ShadowMask.slang))**: Traces soft cone shadow rays for the directional sun light, denoised by **NRD SIGMA**.
2. **Decoupled RT Reflections ([`Reflection.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Reflection.slang))**: Traces GGX VNDF reflection rays (with optional half/quarter-res tracing + full-res smooth mirror fill), shaded via [`RayHitShading.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RayHitShading.slang) and denoised by **NRD REBLUR** or **RELAX**.
3. **Ambient Occlusion ([`RTAO.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTAO.slang) & [`SSAO.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/SSAO.slang))**: Cosine-hemisphere ray-traced AO or screen-space AO with bilateral depth-aware blur.
4. **Cascaded Toroidal DDGI (`DDGI*.slang`)**: Up to 4 camera-following cascades of irradiance (6x6 octahedral) and Chebyshev visibility distance (14x14 octahedral) probes with automatic probe relocation (`DDGIRelocation.slang`) and inactive probe classification.
5. **Lumen-Style Async-Compute Screen-Probe GI (`ScreenProbe*.slang`)**: Places jittered screen probes on 8x8 tiles, traces 64 octahedral rays + wave-shared emissive light grid NEE rays on the **Async Compute Queue**, filters across neighboring probes, and integrates with temporal moment clamping.
6. **NVIDIA RTXDI Suite (`RTXDI/*` & [`ReSTIRPasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/ReSTIRPasses.cpp))**:
   - **Light Presampling & ReGIR**: Morton Z-curve light PDF mipmap, power-RIS tiles, and 3D camera-centered **ReGIR** grid cells.
   - **ReSTIR DI**: Initial candidate generation -> temporal resampling -> spatial resampling -> final shading + NRD diffuse/specular denoising.
   - **ReSTIR GI**: Secondary bounce reservoir resampling with Jacobian validation, wave boiling filter, and disocclusion visibility ray check + NRD denoising.
   - **ReSTIR PT**: Full spatiotemporal path resampling with **Hybrid Shift Mapping** (random replay + reconnection vertices + Jacobian bias correction).
7. **Reference Hardware Ray-Tracing Path Tracer (`PathTracer/*` & [`LightingPasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/LightingPasses.cpp))**:
   - Uses a dedicated Vulkan Ray Tracing Pipeline (`RayGen`, `ClosestHit`, `AnyHit`, `Miss`) and Shader Binding Table (`bakeShaderTable`).
   - Features Owen-scrambled Sobol sampling, unified 1-shadow-ray NEE (analytic + 3D emissive triangle grid + hierarchical octahedral environment MIP-descent with MIS), smooth-glass delta reflection/refraction branching, RGB wavelength dispersion, nested 2-slot Beer-Lambert volume absorption, 3-plane DLSS Ray Reconstruction guides, and optional NRD demodulated denoising.

### G. Physical Sky, Volumetric Clouds, Transparency, DLSS/TAA & Post-Processing
- **Hillaire 2020 Physical Sky & Volumetric Clouds ([`SkyAtmosphere.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/SkyAtmosphere.slang), [`Skybox.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Skybox.slang))**: Precomputes transmittance & multi-scattering LUTs, 3D Perlin-Worley shape/detail cloud noise, raymarches volumetric clouds into a time-sliced sky cubemap + sharp 2048x256 cylindrical horizon band + 2D cloud shadow map + 32x32x32 camera froxel aerial perspective volume, and renders physical sun, cratered moon, and blackbody starfield.
- **Transparency & Stacked Glass ([`Material_TransparentLit_Mesh.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Material_TransparentLit_Mesh.slang))**: Supports McGuire-Bavoil Weighted Blended OIT (`OITComposite.slang`), roughness-indexed screen-space refraction (`ColorPyramid.slang`), multi-bounce ray-traced volumetric dielectric refraction with TIR & dispersion, and two-pass multiply/add stacked glass.
- **Upscaling ([`UpscalePasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/UpscalePasses.cpp))**: Evaluates **NVIDIA DLSS Super Resolution / DLAA**, **DLSS Ray Reconstruction** (with planar-mirror virtual-image specular motion vectors in `RRGuides.slang`), or built-in compute **TAA** (`TAA.slang`). Re-renders unjittered display-resolution depth & entity IDs (`EditorStableDepth.slang`) so editor gizmos and picking never jitter.
- **Auto-Exposure & 10 Tonemappers ([`AutoExposure.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/AutoExposure.slang), [`Tonemapping.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Tonemapping.slang))**: 256x256 log-luminance reduction pyramid with temporal eye adaptation and 10 selectable tonemappers (default **Hable Uncharted 2**, ACES Narkowicz/Hill/RTXPT, Khronos PBR Neutral, Reinhard, etc.).

---

## 2. File-by-File Reference: `NoxCore/src/NoxCore/Renderer/` (38 Files)

### Core Renderer & GPU Scene Management (29 Files)

| File | Plain-English Summary & Key Responsibilities |
| :--- | :--- |
| [`Renderer.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Renderer.h) & [`Renderer.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Renderer.cpp) | Master renderer class (`Renderer`). Manages all pipelines, `RenderGraph`, `GpuScene`, `GeometryArena`, `GeometryStreamer`, `VirtualTextureStreamer`, `UploadManager`, `MemoryBudget`, IBL precomputation, procedural sky/cloud passes, Shader Binding Table baking, 23+ debug views, 7 extra editor views, and `BuildFrameGraph`. |
| [`DataTypes.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/DataTypes.h) | Host-side renderer data structures: `Vertex`, `CookedGeometry` (with `shaderio::PackedVertex` encoding, `MeshletPage`, `ClusterGroupRecord`), `Light`, `EnvironmentSettings`, `SkyAtmosphereSettings`, `RendererStats`, `ExtraViewDesc`, `DebugVisualMode` (23 modes), `TonemapOperator` (10 modes), `GIMode`, `UpscaleMode`, `ShadowMode`, `ReflectionMode`, and `AmbientOcclusionMode`. |
| [`GpuScene.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/GpuScene.h) & [`GpuScene.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/GpuScene.cpp) | Retains all scene instances, transforms, materials, meshes, lights, and emissive triangles on the GPU. Uses free-lists and per-frame dirty staging patches (`Flush`) so static scenes require zero per-frame instance re-uploads, while updating LWC camera-relative transforms for moving entities. |
| [`GeometryArena.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/GeometryArena.h) & [`GeometryArena.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/GeometryArena.cpp) | Manages unified bindless geometry buffers via `OffsetAllocator`, plus texture mip feedback and virtual texture feedback readback buffers. |
| [`GeometryStreamer.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/GeometryStreamer.h) & [`GeometryStreamer.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/GeometryStreamer.cpp) | Nanite-style Cluster DAG page streamer. Reads GPU page feedback, streams 128 KB geometry pages from VFS/disk on IO workers, commits/evicts 64 KB sparse buffer pages via `updateSparseBuffer`, and schedules leaf-cut BLAS rebuilds (`ClusterIndices.slang`). |
| [`VirtualTextureStreamer.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/VirtualTextureStreamer.h) & [`VirtualTextureStreamer.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/VirtualTextureStreamer.cpp) | Streaming Virtual Texturing (SVT) manager. Maintains 5 format-specific 4096x4096 physical tile cache atlases (`136x136` bordered tiles), streams requested tiles from `NTV1` `.ntex` files, patches page tables, and evicts cold tiles via LRU. |
| [`UploadManager.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/UploadManager.h) & [`UploadManager.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/UploadManager.cpp) | Thread-safe async GPU upload manager. Uses per-thread staging allocators and the dedicated hardware `Transfer` DMA queue synchronized via a `TimelineSemaphore`. |
| [`MemoryBudget.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/MemoryBudget.h) & [`MemoryBudget.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/MemoryBudget.cpp) | Queries OS/driver VRAM budgets and computes dynamic byte caps for the Texture Streamer (45%), Geometry Page Pool (25%), and Virtual Texture Tile Cache (10%), shrinking pools automatically under VRAM pressure. |
| [`Mesh.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Mesh.h) & [`Mesh.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Mesh.cpp) | Defines `Mesh` (skeletal/skinned mesh asset), `StaticMesh` (static mesh asset), `Submesh`, `MeshNode`, `MeshLight`, `MeshCamera`, and `MaterialVariantSet`. |
| [`DynamicMesh.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/DynamicMesh.h) & [`DynamicMesh.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/DynamicMesh.cpp) | Runtime procedural mesh system (`DynamicMeshes`). Allows C# scripts (`new Mesh()`, `MeshBuilder`) to generate/modify meshes at runtime, auto-computing smooth normals, tangents, meshlets, GPU uploads, BLASes, and CPU triangles for Jolt physics colliders. |
| [`Renderer2D.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Renderer2D.h) & [`Renderer2D.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Renderer2D.cpp) | High-speed mesh-shader batch renderer for 2D/3D quads, sprites, SDF circles, 3D lines/AABBs, MSDF text strings, and the infinite procedural editor ground grid (`GridMesh.slang`). |
| [`Camera.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Camera.h) | Base camera class holding a `glm::mat4` projection matrix. |
| [`EditorCamera.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/EditorCamera.h) & [`EditorCamera.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/EditorCamera.cpp) | Interactive 3D viewport camera supporting **Fly** (WASD + RMB + scroll speed) and **Arcball Orbit** (`Alt+LMB`/`MMB`/`RMB`) modes, **Perspective** & **Orthographic** (`Top`, `Bottom`, `Front`, `Back`, `Right`, `Left`) Reverse-Z projections, and double-precision `glm::dvec3` LWC focal point & position. |
| [`Font.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Font.h) & [`Font.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Font.cpp) | TrueType (`.ttf`) MSDF (Multi-channel Signed Distance Field) font generator and disk cacher (`msdf-atlas-gen`). |
| [`MSDFData.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/MSDFData.h) | Holds `msdf_atlas::FontGeometry` and `GlyphGeometry` metrics for `Font`. |
| [`ShaderCompileSplash.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/ShaderCompileSplash.h) & [`ShaderCompileSplash.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/ShaderCompileSplash.cpp) | Pure SDL3 2D progress window displayed before the Vulkan renderer finishes initializing whenever uncached shaders need to be compiled on first boot. |

---

### Render Graph Pass Modules (`NoxCore/src/NoxCore/Renderer/Passes/` — 9 Files)

| File | Render Graph Passes Declared & Recorded |
| :--- | :--- |
| [`FrameGraphResources.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/FrameGraphResources.h) | Defines the 9 `RGBlackboard` structs shared between pass modules: `SceneState`, `StreamingState`, `VisibilityState`, `GBufferState`, `ShadowState`, `ReflectionState`, `AOState`, `GIState`, and `LightingState`. |
| [`RasterPasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/RasterPasses.cpp) | `Compute Skinning`, `Animated BLAS Update`, `BLAS Build`, `Streaming Update`, `Geometry Streaming Feedback`, `TLAS Build`, `Instance Culling`, `Visibility`, `Hi-Z Build`, `Instance Culling Late`, `Visibility Late`, `GBuffer` (+ cluster/mip feedback readbacks), `Particles` / `Particles OIT` / `Particle Motion`, `Forward 3D`, `Transparent OIT` + `OIT Composite`, `Refraction Source` (`ColorPyramid`), and `Forward Transparent`. |
| [`LightingPasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/LightingPasses.cpp) | `Emissive Triangles` + `Emissive Power Sum` + `Emissive Light Grid`, Hardware Path Tracer (`PT Environment Importance`, `PT Environment Reduce`, `Path Trace`, `NRD PT`, `PT Composite`), `SSAO` + `Occlusion Blur`, `RTAO` + `RTAO Upsample` + `Occlusion Blur`, and `Deferred Lighting` (including `Skybox` and `DDGIProbeSpheres`). |
| [`RTPasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/RTPasses.cpp) | `NRD Guides`, `RTUpsamplePass`, `RT Shadows` + `NRD Shadows` (SIGMA), `RT Reflections` + `RT Reflections (Smooth, Full Res)` + `NRD Reflections` (REBLUR/RELAX), Cascaded DDGI (`DDGI Radiance`, `DDGI Relocation`, `DDGI Blend Irradiance`, `DDGI Blend Distance`), and Async-Compute Screen-Probe GI (`Screen Probe Place`, `Trace`, `Irradiance`, `Filter 1..3`, `Integrate`). |
| [`ReSTIRPasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/ReSTIRPasses.cpp) | `Previous Frame Copy`, Light Presampling (`Light PDF Write`, `Light PDF Reduce`, `RIS Presample`, `ReGIR Presample`), ReSTIR DI (`Initial`, `Temporal`, `Spatial`, `Final Shading`, `NRD DI`), ReSTIR GI (`Initial`, `Temporal`, `Spatial`, `NRD GI`), and ReSTIR PT (`Initial`, `Temporal`, `Final Shading`, `NRD PT`, `PT YCoCg Decode`). |
| [`UpscalePasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/UpscalePasses.cpp) | `RR Guides` + `DLSS` (Streamline DLSS-SR / DLAA / DLSS-RR), `TAA`, `Entity/Depth Blit`, and `Stable Entity/Depth` (unjittered depth/entity ID re-render for editor gizmos/picking). |
| [`PostPasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/PostPasses.cpp) | `Auto Exposure` + `Auto Exposure Reduce` + `Auto Exposure Adapt`, `Post Process` (tonemapping), `2D Overlay` (`Renderer2D`), `Selection Mask`, and `Outline` (orange silhouette). |
| [`EditorPasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/EditorPasses.cpp) | `ImGui / Present Pass`, `Texture Readback 1..4`, `Editor Raycasts` (`EditorRaycast.slang`), `Pick Readback` (1x1 cursor entity/depth readback), and `TextureInspect` (live Render Graph texture/mip inspector). |
| [`ExtraViewPasses.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Renderer/Passes/ExtraViewPasses.cpp) | `addExtraViewPasses`: Renders up to 7 secondary views (`Viewport 1..4`, `Thumbnail`, `Material Preview`, `Animation Preview`), each with its own culling, visibility, skybox, forward PBR shading, auto-exposure, tonemapping, and 2D overlays. |
