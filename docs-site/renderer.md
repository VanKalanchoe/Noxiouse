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

---

## Raster Pipeline, Quality Presets & Async Compute (November 2026)

Ray tracing stays required, but every ray-traced effect now has a **raster version** next to it, built to be measured against
it in the same scene. Each one is selectable on its own in **Settings**, and the quality presets switch them together.

### Quality presets (Settings > Quality Preset, saved in the `.nproj`)

| Preset | What it turns on |
| :--- | :--- |
| **Low** | RT sun shadows, screen-probe GI (24 px probes), SSAO, DLSS Performance |
| **Medium** | + RT reflections, screen-probe GI (16 px), DLSS Balanced |
| **High** | + RTAO, RT glass, DLSS Quality |
| **Ray Traced (All)** | High with every light shadowed, DLSS Ray Reconstruction, Auto Exposure, OIT |
| **Raster (All)** | Raster everything except reflections: shadow maps + contact shadows, GTAO, raster probe volume GI + SSGI, RT reflections (NRD REBLUR), DLSS Quality, Auto Exposure, OIT |
| **Custom** | Keeps the settings as they are (no preset at start) |

Raster (All) uses ray-traced reflections rather than SSR: screen-space reflections change with what is on screen (they move
and vanish at the screen edges), which no amount of filtering fixes.

### The raster features

| Feature | Setting | How it works | Equivalent |
| :--- | :--- | :--- | :--- |
| **Sun cascaded shadow maps** | Sun Shadows | 4 stable cascades of 2048, PCSS soft edges (filter 1.5-24 texels), resolved into the same shadow mask the RT shadows write | Unreal / Godot CSM |
| **Cascade caching** | Cache Cascades | A cascade is drawn again only where the scene changed inside it (the GPU scene reports changed bounds); the far cascades take turns | Unreal's cached shadow maps |
| **Local-light shadow atlas** | Local Light Shadows | Spot / point / rect light shadows in one 4096 atlas, tile size by screen size, up to 8 views a frame | Godot's shadow atlas |
| **Contact shadows** | Contact Shadows | A short screen-space ray towards the light per pixel, for the detail shadow maps miss | Unreal contact shadows |
| **GTAO** | Ambient Occlusion | Ground-truth AO (horizon slices), classic SSAO still selectable | Unreal / Godot GTAO |
| **SSR + reflection probes** | Screen-Space Reflections, Reflection Probe component | A deterministic mirror ray with roughness filtering, falling back to box-projected reflection probes, then the sky | Godot SSR |
| **Raster probe volume GI** | Raster GI | Irradiance Volume probes capture an unlit G-buffer once and are **relit every frame** (sun, sky, lights): dynamic lighting without rays. Captures / relights per frame are settings | Godot DDGI-style probes, Unreal's relit captures |
| **SSGI** | Raster GI > Screen-Space GI | Screen-space bounce light on top of the probe volume | Godot SSIL |
| **Volumetric fog** | Volumetric Fog | Lit froxel fog with light shafts, sampling the shadow mask or maps per froxel | Unreal volumetric fog |

### Post effects (Settings > Post Effects)

Bloom (downsample / upsample chain), depth of field (bokeh from the camera's aperture and focus distance), per-pixel motion
blur from the motion vectors, color grading LUT, vignette, film grain, chromatic aberration, lens flares and CAS sharpening.

### Async compute

Passes flagged `RGPassFlags::AsyncCompute` run on the compute queue; the render graph forks and joins them. On the compute
queue now: the screen-probe GI, the **NRD denoisers** (shadows and reflections: NRD only dispatches compute work, its README
allows `queueType = COMPUTE`; NRD is created with both queue families and each call passes its command buffer's queue type with
compute-only stages), the DDGI blends and the emissive light grid. The stats show **Async Compute Passes**.

### Streamed levels drawn once complete

A streamed level or world cell is registered with the GPU scene **hidden** while it comes in (its records uploaded, in no draw
bucket and not in the TLAS) and all of it is shown in one frame once complete -- Unreal's `ULevel::bRequireFullVisibilityToRender`.
No level is ever seen half built.

### Shading models: Default Lit, Unlit, Toon (November 2026)

`MaterialData::ShadingModel` (`MaterialShadingModel`, `DataTypes.h`) replaces the old `Unlit` flag -- the same byte in the `.nmat` and cooked-mesh layouts (0 / 1 read as before). The Toon settings (`ToonBands`, `ToonBandSoftness`, `ToonRampTexture`, `ToonSpecular*`, `ToonRim*`) are stored only in `.nmat` files (magic `NMA4`; `NMA3` files load with their defaults); a cooked mesh's glTF materials are never toon. Plan: `docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md` (step 1 of 7).

- **GPU:** `GpuMaterial::shadingModel` (was `unlit`) and the `toon*` fields (`Renderer.cpp` packs them; the ramp through `GetTextureIndex`). Unlit materials keep their forward buckets; toon ones use the Default Lit buckets.
- **G-buffer:** the flag byte's free bit `GBufferToon` (0x40) marks toon pixels; their lobe target (RGBA32_UINT, unused: lobes are Default Lit only) carries the settings (`gbuffer_encode_toon` / `gbuffer_decode_toon`, `shaderIO.h`) -- as Unreal keeps a shading model's data in its CustomData target.
- **Lighting:** `ToonShading.slang` -- `toon_direct_light` per directional / point / spot light: diffuse = albedo x light x tone(N.L x shadow), the tone banded (`toon_bands`, band k starting at k / bands) or read from the ramp; a sharp N.H highlight; a rim at grazing angles on the lit side. `DeferredLighting.slang` calls it in its light loop (a toon pixel skips ReSTIR DI's physical result and takes the loop); `Material_TransparentLit_Mesh.slang` uses it too (there the shadow map is in the attenuation, so a soft shadow edge dims instead of bands). Ambient, reflections and rect / disc lights stay Default Lit.
- **Ray tracing / path tracer:** they read the material as Default Lit (the fallback).
- **Editor and MCP:** the Material Editor's Shading Model combo and Toon section, with Unreal's labels (Blend Mode, Opacity Mask Clip Value, Two Sided, Ambient Occlusion); MCP material JSON `ShadingModel` ("DefaultLit" / "Unlit" / "Toon") and the `Toon*` fields, the old `Unlit: true` still read.
- **Tests:** NoxToolsTest -- the JSON codec and a `.nmat` round trip, an `NMA3` file still loading (167 checks).

### Post Process Volume and outlines (November 2026)

`PostProcessSettings` (`Renderer/PostProcessSettings.h`) is the post effects' settings (bloom, depth of field, motion blur, grading, vignette, grain, chromatic aberration, CAS, LUT) plus the outlines. The renderer keeps the editor's copy (`getPostEffects`, Settings > Post Effects, not saved) and a scene's (`SetScenePostProcess`); `activePostEffects()` is what this frame's passes use.

- **`PostProcessVolumeComponent`** (Unreal's Post Process Volume): `Enabled`, `Unbound` (the whole scene) or a box (`HalfExtents`, entity space), `Priority`, `Settings`. `Scene::SubmitPostProcess` (a submit system after `BeginScene`) picks, at `Renderer::GetCameraWorldPosition` (double precision: render origin plus the render-space camera), the highest-priority enabled volume that applies, or none. Saved in the scene file (`PostProcessVolumeComponent` with a `Settings` map, every field by name); always loaded in a partitioned world; MCP's component tools reach it. No blending between volumes yet.
- **Outlines** (`SceneOutline.slang`, `Renderer::addSceneOutlinePass` after the SSR history copy, before DLSS / TAA): a full-screen raster draw into the HDR scene at the render resolution, blended Zero / SrcColor -- the scene times the outline colour on an edge, white elsewhere, so no colour is read back. An edge is a neighbour on a ring `thickness` pixels out (eight directions; thickness scaled from output to render pixels) that is farther by more than a share of this pixel's distance (silhouettes; the sky counts as infinitely far) or, at about the same distance, faces away by more than the normal threshold (creases) -- seen from the nearer side, so the line lies on the object. Toon-only reads the G-buffer's toon bit; fade by distance; an orthographic view measures depth steps against 10 m. Skipped with the path tracer and the debug views.
- **Editor:** `Panels/PostProcessSettingsUI` draws the controls for both the Renderer Settings panel (greyed while a volume applies) and the volume's Inspector; bounded volumes are drawn as violet boxes in Edit and Simulate.

### Custom Lit: Material Shaders (November 2026)

`MaterialShadingModel::CustomLit` (3): a material lit by a **Material Shader** -- a `.slang` asset (`AssetType::MaterialShader`) in the project's Assets with `float3 light(CustomLitSurface, CustomLitLight)` (per light) and `float3 ambient(CustomLitSurface, CustomLitAmbient)` (once); the pixel is their sum plus the emissive, then the fog. The material names it by path (`MaterialData::CustomShader`) and gives it four parameters (`CustomParameters`) and two textures (`CustomTexture1` sRGB, `CustomTexture2` linear), stored in `.nmat` files only (magic `NMA5`; `NMA4` / `NMA3` files load with the defaults). Plan: `docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md` (step 3, as built: the old steps 3, 5 and 6's lighting part together).

- **Why not a forward pass:** with a visibility buffer every opaque pixel already has its exact triangle, depth and G-buffer; a second geometry pass would need bit-identical depth from another mesh shader. A Custom Lit surface goes through the visibility buffer and the G-buffer like any opaque one (depth, normals, velocity, Hi-Z, SSAO, GI, SSR, outlines and TAA / DLSS all see it), and only its lighting differs: a per-material lighting pass over the G-buffer, which is how a deferred renderer allows custom lighting.
- **G-buffer:** the flag byte's top two bits are now the shading model (Default Lit 0, toon 0x40, unlit 0x80, Custom Lit 0xC0; `gbuffer_unlit` / `gbuffer_toon` / `gbuffer_custom_lit` compare the field). A Custom Lit pixel's lobe target holds its Material Shader's slot, its material record, its UV (two halves) and the UV's footprint (`gbuffer_encode_custom_lit`; `CustomLit.slang` decodes it).
- **Generated shaders** (`Renderer/CustomLitShaders.h`): every Material Shader of the project goes into `CustomLitShaders.slang`, each in `namespace CustomLit<slot>` after a `#line` with its path (compile errors name the user's file and line), followed by `custom_lit_light(slot, ...)` / `custom_lit_ambient(slot, ...)` switches. `DeferredLightingCustomLit.slang` and `TransparentLitCustomLit.slang` define `NOX_CUSTOM_LIT` and include it before `DeferredLighting.slang` / `Material_TransparentLit_Mesh.slang`; `CustomLitCheck.slang` compiles the Material Shaders alone first (about a second) so an error is reported instead of a hot reload silently keeping the last working binary. They are written next to the executable in `shaders/Generated` (with `CustomLitShaders.txt`, the slots in order) -- beside the precompiled binaries, so a packaged game ships them and finds the binaries by the same keys. Slots stay stable while the editor runs (`AssignSlots`: kept, appended, removed ones left empty).
- **Passes:** `DeferredLighting.slang` with `NOX_CUSTOM_LIT` discards every pixel but Custom Lit ones, the plain build discards those (outside the debug views). The **Custom Lit** pass (`addDeferredLightingPass`, right after it, `LoadOP::load`, only while `GpuScene::GetCustomLitMaterialCount() > 0`) runs the same light loop -- shadow mask, local shadow maps, RT shadow rays, contact and cloud shadows -- and calls `light()` for every light in reach, the ones the surface faces away from with shadow 0; rect / disc lights reach it as a point light at their centre (area x facing / d^2, their range window). ReSTIR DI is skipped. The ambient is computed as for a white diffuse surface and an F0 = F90 = 1 mirror of its roughness (GI, sky, SSGI, SSR / RT reflections, probes) and handed to `ambient()`.
- **Forward:** `forwardLitPipeline()` is the forward shader's Custom Lit variant while the project has Material Shaders (it shades every other material as the plain one): transparent surfaces (not with OIT), the editor's other viewports and the Material Editor's preview. Probe captures, ray-traced reflections, GI bounces and the path tracer see a Custom Lit surface as Default Lit.
- **Building:** `NRI::PipelineDesc::buildNow` builds on the calling thread and throws at once (no shader-object pool build left for the first bind). A Material Shader without a slot, or with the pipelines failed, is packed as Default Lit (`PackMaterial`, `Renderer::FindCustomLitSlot`). The editor builds on project open (`InitializeCustomLitShaders`) and polls the Material Shaders' paths, sizes and write times twice a second (`EditorLayer::PollCustomLitShaders`); NoxRuntime builds from the project's files in a development run and from the manifest and binaries in a packaged game (`VFS::HasMounts`). The engine's own `CustomLit.slang`, `DeferredLighting.slang`, `Material_TransparentLit_Mesh.slang` and `PBRLighting.slang` rebuild them on a hot reload. `ubo->timeSeconds` (wrapped hourly) is new for animated shaders.
- **Editor and MCP:** Content Browser > + Add > Material Shader (the toon template, `CustomLitShaders::TemplateSource`), double-click opens it in the system's editor; the Material Editor's Custom Lit section (shader combo / drop target, build status, Parameter 1-4, Custom Texture 1-2). MCP: material JSON `ShadingModel: "CustomLit"`, `CustomShader`, `CustomParameters`, `CustomTexture1/2`; tools `write_material_shader` and `get_material_shaders`. The packager warns when the Material Shaders do not compile.
- **Fixed after the user's first test:** creating a Material Shader froze the editor for about 40 seconds (the forward variant compiled on the main thread) and its name could not be typed or renamed (`CanRename` did not list the type). The build now runs on a worker thread (`startCustomLitBuild` / `pollCustomLitBuild`, one at a time, the newest request waiting; the Custom Lit pass swaps in first, the forward variant after; an unknown slot lights as Default Lit's diffuse meanwhile) -- the forward variant compiles only `fragMain` (its OIT / AVBOIT / probe-capture entry points are left out of the Custom Lit build: 24 s to 8 s here); Material Shaders can be renamed (materials find a renamed one by its old path, `GetCustomLitSlot`). Code editors: the template starts with `#include "CustomLit.slang"` (blanked in the generated file, line numbers kept) and the editor writes `<project>/slangdconfig.json` with the engine's shader folder as a search path for the Slang language server.
- **Tests:** NoxToolsTest -- the generated file (namespaces, `#line`, dispatch cases), the slot assignment, the template, the JSON fields, an `NMA5` round trip and an `NMA4` file (182 checks). Slang: the template and a second shader through the real generator output compile in all three generated files, and an error names the user's file and line.
