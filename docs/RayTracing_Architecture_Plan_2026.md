# NoxEngine: Modern Ray Tracing & Lighting Architecture Plan (2026)

> **Document Version:** 6.0 (Dual-Track GI Architecture: DDGI vs ReSTIR GI & RTXDI Integration — March 2026)  
> **Target API:** Vulkan 1.3 / 1.4 (`VK_KHR_ray_query`, `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_EXT_descriptor_buffer`, `VK_EXT_shader_object`)  
> **Shading Language:** Slang (SPIR-V Target)  
> **Orchestration & Denoising Framework:** NVIDIA Streamline (SL) + DLSS 3.5 / DLSS-RR + Standalone NRD  
> **Industry Benchmarks:** Unreal Engine 5.4 (Lumen & MegaLights), Cyberpunk 2077 (REDengine 4 RT Overdrive), NVIDIA RTX SDKs (RTXPT, RTXDI, RTXGI)  

---

## 1. Collaboration Contract & Engineering Workflow

To maintain absolute code safety, stability, and mutual verification, the development of this rendering architecture strictly adheres to the following rules:

1. **Execution Policy (Strict)**:
   - Antigravity never executes terminal commands, scripts (`.bat`, `.ps1`), compilers, or background tasks unless the user explicitly requests them.
   - All code inspection and modifications are performed purely on files, with clear explanations. The user compiles and runs.
2. **Phase Advancement Gate**:
   - No phase begins until the previous phase's interactive debug view mode is visually confirmed running and stable.
3. **C++ Class Layout Scheme (Strict Engine Convention)**:
   - **Top**: `public:` section containing all public methods, interface overrides, and inline getters/setters.
   - **Middle**: `private:` section containing all private helper functions and internal methods.
   - **Bottom**: `private:` section containing all member variables (`m_*`).
4. **Strict NRI Backend Abstraction Boundary**:
   - **Zero Graphics Leak**: High-level systems interact purely with abstract `NRI` types (`NRI::Device`, `NRI::Texture`, `NRI::CommandBuffer`, `NRI::Buffer`, etc.).
   - Vulkan headers (`<vulkan/vulkan.h>`), `VkCommandBuffer`, `VkImage`, `vk::*`, and Streamline headers remain strictly inside `NoxCore/src/NRI/Vulkan/`.
5. **Infinite Reverse-Z Projection & Depth Convention**:
   - NoxEngine uses infinite Reverse-Z (`depth = 1.0` at `zNear`, `depth = 0.0` at infinity).
   - Far clip is **never used or queried** for camera projection.
   - Upscaling/denoising frameworks (DLSS, NRD) must always configure `depthInverted = sl::Boolean::eTrue` and `cameraFar = 0.0f`.

---

## 2. Official Documentation, Repositories & Programming Guides

The following official documentation, source code repositories, and technical whitepapers serve as the ground-truth specifications for NoxEngine's implementation:

### 2.1. NVIDIA Streamline (SL)
- **Official GitHub Repository**: [NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline)
- **GitHub Releases (Binaries)**: [Streamline Releases](https://github.com/NVIDIA-RTX/Streamline/releases)
- **Programming Guide**: [Streamline Programming Guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md)
- **Vulkan Integration Guide**: [Streamline Vulkan Guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideVulkan.md)
- **DLSS & Motion Vectors Guide**: [Streamline DLSS Guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)
- **DLSS Ray Reconstruction Guide**: [Streamline DLSS-RR Guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md)
- **NVIDIA Reflex Guide**: [Streamline Reflex Guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideReflex.md)

### 2.2. NVIDIA NRD (Real-Time Denoisers)
- **Official GitHub Repository**: [NVIDIAGameWorks/RayTracingDenoiser](https://github.com/NVIDIAGameWorks/RayTracingDenoiser)
- **Architecture Whitepaper**: [NRD Whitepaper (PDF)](https://github.com/NVIDIAGameWorks/RayTracingDenoiser/blob/master/NRD.pdf)
- **Engine Integration Guide**: [NRD Integration Guide](https://github.com/NVIDIAGameWorks/RayTracingDenoiser/blob/master/Integration.md)
- **SIGMA (Shadow Denoiser) Specs**: [NRD Denoisers Overview](https://github.com/NVIDIAGameWorks/RayTracingDenoiser/tree/master/Shaders)

### 2.3. Path Tracing & Vulkan Shader Binding Table (SBT)
- **NVIDIA `nvvk::SBTGenerator` Source**: [nvpro_core/nvvk/raytraceKHR_vk.hpp](https://github.com/nvpro-samples/nvpro_core/blob/master/nvvk/raytraceKHR_vk.hpp)
- **Vulkan Ray Tracing Tutorial**: [NVIDIA Vulkan Ray Tracing Tutorial (KHR)](https://nvpro-samples.github.io/vk_raytracing_tutorial_KHR/)
- **Physically Based Rendering (PBRT v4)**: [pbrt.org](https://www.pbrt.org/)

### 2.4. NVIDIA RTXGI & DDGI (Dynamic Diffuse Global Illumination)
- **Official GitHub Repository**: [NVIDIAGameWorks/RTXGI](https://github.com/NVIDIAGameWorks/RTXGI)
- **RTXGI DDGI Guide**: [RTXGI DDGI Implementation Guide (PDF)](https://github.com/NVIDIAGameWorks/RTXGI/blob/main/RTXGI-DDGI-Guide.pdf)
- **Foundational Paper**: [Majercik et al., *Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields*, JCGT 2019](https://jcgt.org/published/0008/02/01/)

### 2.5. NVIDIA RTXDI (ReSTIR DI, ReSTIR GI & ReSTIR PT)
- **Official GitHub Repository**: [NVIDIA-RTX/RTXDI](https://github.com/NVIDIA-RTX/RTXDI)
- **RTXDI Library Source**: [NVIDIA-RTX/RTXDI-Library](https://github.com/NVIDIA-RTX/RTXDI-Library)
- **Foundational ReSTIR Paper**: [Bitterli et al., *Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing with Dynamic Direct Lighting*, SIGGRAPH 2020](https://cs.dartmouth.edu/wjarosz/publications/bitterli20spatiotemporal.html)
- **ReSTIR GI Paper**: [Ouyang et al., *ReSTIR GI: Path Resampling for Real-Time Path Tracing*, HPG 2021](https://intro-to-restir.cwyman.org/)
- **ReSTIR PT (GRIS) Paper**: [Lin et al., *Generalized Resampled Importance Sampling: Foundations of ReSTIR*, SIGGRAPH 2022](https://intro-to-restir.cwyman.org/)

### 2.6. NVIDIA RTXPT (RTX Path Tracing SDK) & BSDF Reference Specifications
- **Official Overview**: [NVIDIA RTX Path Tracing](https://developer.nvidia.com/rtx/ray-tracing/path-tracing)
- **NVIDIA Falcor PBR BSDF Framework**: [NVIDIAGameWorks/Falcor](https://github.com/NVIDIAGameWorks/Falcor)
- **GGX VNDF Sampling**: [Eric Heitz, *Sampling the GGX Distribution of Visible Normals*, JCGT 2018](https://jcgt.org/published/0007/04/01/)
- **Microfacet Refraction & Glass**: [Walter et al., *Microfacet Models for Refraction through Rough Surfaces*, EGSR 2007](https://www.cs.cornell.edu/~srm/publications/EGSR07-btdf.html)
- **Multiple Importance Sampling (MIS)**: [Veach & Guibas, *Optimally Combining Sampling Techniques for Monte Carlo Rendering*, SIGGRAPH 1995](https://graphics.stanford.edu/papers/combining/)

---

## 3. Unified Lighting & Global Illumination (GI) Architecture

NoxEngine adopts a dual-engine architecture designed for maximum performance, research flexibility, and ground-truth comparison:

```
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│                                 NoxEngine Lighting Core                                 │
├────────────────────────────────────────────┬────────────────────────────────────────────┤
│   Primary: Deferred Visibility Buffer RT   │         Ground-Truth: Path Tracer          │
├────────────────────────────────────────────┼────────────────────────────────────────────┤
│ • Direct Lighting:                         │ • Direct Lighting:                         │
│   - RTXDI (ReSTIR DI Many-Light Resampling)│   - RTXDI (ReSTIR DI NEE Light Resampling) │
│   - RT Shadows (1 ray/px) + NRD SIGMA      │   - Monte Carlo Area Light Soft Shadows    │
│ • Specular Reflections:                    │ • Specular / Transmission / Diffuse:       │
│   - RT Reflections (1 ray/px GGX VNDF)     │   - NVIDIA RTXPT Multi-Lobe Microfacet     │
│   - NRD REBLUR/RELAX or DLSS-RR            │   - Cook-Torrance GGX, Fresnel, Glass Snell│
│ • Diffuse Global Illumination (Dual-Track):│ • Indirect Path Acceleration:              │
│   [Runtime Toggle: Benchmark & Compare]    │   - RTXDI ReSTIR PT / ReSTIR GI            │
│   ├─ Track A: DDGI (3D Octahedral Probes)  │   - Multi-bounce spatiotemporal path reuse │
│   ├─ Track B: RTXDI ReSTIR GI (Screen Rays)│ • Denoising / Output:                      │
│   └─ Track C: Off (IBL Cubemap Fallback)   │   - Progressive Accumulation (Stationary)  │
│ • Denoising & Upscaling:                   │   - DLSS 3.5 Ray Reconstruction (Dynamic)  │
│   - NRD Suite or Streamline DLSS-RR/SR     │                                            │
└────────────────────────────────────────────┴────────────────────────────────────────────┘
```

### 3.1. Denoising Architecture: DLSS-RR vs NRD (SIGMA, REBLUR, RELAX)

To achieve optimal visual fidelity across all hardware platforms and rendering paths, NoxEngine implements a dual-track denoising framework with strict runtime arbitration and mutual exclusion toggles:

1. **NRD SIGMA (Upstream Direct Shadow Penumbra Denoiser)**:
   - **Pipeline Location**: Runs *pre-lighting* directly on the raw screen-space 1-SPP shadow mask buffer (`m_rawShadowMask` $\rightarrow$ `m_denoisedShadowMask`).
   - **Function**: Performs physical penumbra contact hardening based on light source angular radius, occluder distance, surface normal/roughness, and linear View-Z.
   - **Coexistence**: **Fully compatible with DLSS-SR and DLSS-RR**. In hybrid AAA pipelines (e.g. Cyberpunk 2077, Alan Wake 2), NRD SIGMA supplies a clean, stable direct shadow mask to deferred lighting.

2. **NRD REBLUR / RELAX (The True Alternative & Competitor to DLSS-RR)**:
   - **Pipeline Location**: Runs on indirect specular reflection radiance (`m_rawReflection`) and indirect diffuse GI buffers.
   - **Function**: Denoises multi-bounce stochastic Monte Carlo radiance using spatiotemporal variance estimation, anti-lag, and A-trous wavelet filters.
   - **Spherical Harmonics (`_SH`) Mode**: In `REBLUR_DIFFUSE_SPECULAR_SH` and `RELAX_DIFFUSE_SPECULAR_SH`, NRD tracks directional spherical harmonics coefficients ($L_0, L_1$), preserving high-frequency specular lobes and directional color bleed comparable in visual quality to DLSS-RR.

---

### 3.2. Definitive Compatibility & Mutual Exclusion Matrix

| Feature / Pass | NRD SIGMA (Shadows) | NRD REBLUR / RELAX (Reflections & GI) | DLSS Super Resolution (DLSS-SR) | DLSS Ray Reconstruction (DLSS-RR) | Compatibility & Arbitration Status |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **Direct RT Shadows** | **Active** (Primary) | N/A | Compatible | Compatible | **Fully Compatible**. Runs upstream pre-lighting. |
| **Direct RT Shadows (Raw)** | Off (Bypassed) | N/A | Compatible | Compatible | **Compatible**. Passes 1-SPP stochastic noise to lighting. |
| **Specular Reflections** | Compatible | **Active** (Primary) | Compatible | **MUTUALLY EXCLUSIVE** | **Strict Mutual Exclusion**. Running both causes dual-filtering blur and ghosting. |
| **Specular Reflections** | Compatible | **MUTUALLY EXCLUSIVE** | Compatible | **Active** (Primary) | **Strict Mutual Exclusion**. DLSS-RR replaces downstream reflection denoisers. |
| **Diffuse GI (DDGI Probes)**| Compatible | N/A (Self-filtered via Hysteresis) | Compatible | Compatible | **Zero Denoiser Required**. Filtered via temporal hysteresis ($\alpha = 0.97$). |
| **Diffuse GI (ReSTIR GI)**  | Compatible | **Active** (REBLUR_DIFFUSE) | Compatible | **MUTUALLY EXCLUSIVE** | **Strict Mutual Exclusion**. Denoised by NRD Diffuse or DLSS-RR. |
| **Path Tracer (Full MC)** | N/A (Embedded) | N/A (Embedded) | Compatible | **Active** (Real-Time) | **DLSS-RR or Progressive Accumulation**. Standalone hybrid denoisers are bypassed. |

---

### 3.3. Runtime Arbitration & Auto-Disable Engine Rules

1. **Activating DLSS Ray Reconstruction (`m_dlssRayReconstructionEnabled = true`)**:
   - Automatically sets `m_nrdReflectionDenoiser = NRDReflectionDenoiser::Off`.
2. **Activating NRD REBLUR or RELAX (`m_nrdReflectionDenoiser != Off`)**:
   - Automatically sets `m_dlssRayReconstructionEnabled = false` (triggers DLSS context reset `m_resetDLSS = true`).
3. **Indirect GI Mode Switching (`m_diffuseGIMode`)**:
   - **Mode 0 (Off)**: Deferred lighting uses environment cubemap `irradianceMap`.
   - **Mode 1 (DDGI Probes)**: Evaluates 3D probe field. Zero denoiser overhead.
   - **Mode 2 (RTXDI ReSTIR GI)**: Evaluates 1-ray-per-pixel screen-space path resampling + NRD/DLSS-RR denoising.
4. **Path Tracing Overrides**:
   - Activating full Path Tracing (`m_pathTracingEnabled = true`) automatically disables G-Buffer hybrid ray tracing passes (RT Shadows, RT Reflections, NRD SIGMA, NRD REBLUR, DDGI).

---

## 4. Viewport Visual Debug View Modes (0 - 19)

NoxEngine provides 20 interactive debug view modes selectable in the Viewport Toolbar:

| Mode ID | Visual Mode Name | Target Shader Pass | Visual Output Description |
| :---: | :--- | :--- | :--- |
| **0** | **Full PBR Lit Scene** | `DeferredLighting.slang` | Final composition (Direct Lighting + GI + AO + Emission + Reflections). |
| **1** | **Base Color (Albedo)** | `GBufferMaterial.slang` | Raw diffuse/base color texture without lighting or shadows. |
| **2** | **Normal Map Texture** | `GBufferMaterial.slang` | Raw tangent-space normal map texture (periwinkle blue with scratches). |
| **3** | **Ambient Occlusion** | `GBufferMaterial.slang` | Material ambient occlusion map channel ($A$ channel of albedo target). |
| **4** | **Emissive Color** | `GBufferMaterial.slang` | Emissive texture multiplied by emissive factor and strength. |
| **5** | **Metallic** | `GBufferMaterial.slang` | Grayscale metalness mask ($0.0 = \text{dielectric}, 1.0 = \text{metal}$). |
| **6** | **Roughness** | `GBufferMaterial.slang` | Grayscale linear roughness mask ($0.0 = \text{mirror}, 1.0 = \text{rough}$). |
| **7** | **Perturbed Shading Normal** | `GBufferMaterial.slang` | Orthonormal world-space normal perturbed by normal map ($N \cdot 0.5 + 0.5$). |
| **8** | **Direct Lighting Only** | `DeferredLighting.slang` | Analytical & RTXDI punctual/area light contribution in HDR. |
| **9** | **IBL / Ambient Sky Only** | `DeferredLighting.slang` | Split-sum diffuse and specular environment cubemap ambient contribution. |
| **10** | **Reconstructed World Pos** | `DeferredLighting.slang` | Fractional world position ($x, y, z \pmod 1$) reconstructed from depth. |
| **11** | **Entity ID Picking** | `DeferredLighting.slang` | Pseudo-random vibrant color per unique scene entity ID. |
| **12** | **Linear Depth Buffer** | `DeferredLighting.slang` | Linear camera distance visualization ($0.0\text{m} = \text{black}, 25.0\text{m} = \text{white}$). |
| **13** | **RT Shadow Mask** | `DeferredLighting.slang` | 1-SPP raw vs NRD SIGMA denoised shadow factor ($1.0 = \text{lit}, 0.0 = \text{shadow}$). |
| **14** | **RT Reflections Radiance**| `DeferredLighting.slang` | Decoupled 1-SPP raw or NRD REBLUR/RELAX denoised reflection radiance. |
| **15** | **Motion Vectors (Velocity)**| `GBufferMaterial.slang` | Screen-space velocity buffer ($R = |\Delta X| \times 100, G = |\Delta Y| \times 100$). |
| **16** | **Indirect Diffuse GI Only** | `DeferredLighting.slang` | Pure bounced indirect diffuse light (DDGI Probes or RTXDI ReSTIR GI). |
| **17** | **DDGI Probe Grid Spheres** | Debug Overlay | Visual debug spheres rendering each probe's irradiance in world space. |
| **18** | **Path Tracer 1-SPP** | `PathTracer.slang` | Real-time 1-SPP raw path tracing output (interactive or DLSS-RR input). |
| **19** | **Path Tracer Ground Truth**| `PathTracer.slang` | Progressive multi-bounce ground-truth Monte Carlo accumulated radiance. |

---

## 5. Implementation Phases & Milestones

### Phase 1: Camera Motion Vectors & Halton Jitter Sequence ✅ COMPLETED
- Non-jittered projection for velocity buffer evaluation.
- 8-phase Halton(2, 3) subpixel projection jittering.
- Verified in Viewport Debug Mode 15 (Motion Vectors).

### Phase 2: NVIDIA Streamline Core & DLSS 3.5 Ray Reconstruction ✅ COMPLETED
- Streamline initialized with manual hooking in `DeviceVK`.
- G-Buffer attachments (Albedo, Specular $F_0$, World Normals, Roughness, Depth, Motion Vectors) tagged to Streamline.
- DLSS Super Resolution (`kFeatureDLSS`) and Ray Reconstruction (`kFeatureDLSS_RR`) running with zero Vulkan validation errors.
- Runtime toggleable without resource deletion crashes (`m_resetDLSS = true`).

### Phase 3: Interactive Path Tracer Ground Truth Core ✅ COMPLETED
- Full multi-bounce inline RayQuery path tracer (`PathTracer.slang`) with mesh shader invocation.
- Monte Carlo area light soft shadows with angular diameter and PCG random numbers.
- Camera-stationary progressive accumulation + real-time DLSS Ray Reconstruction integration.
- IBL ambient sky scale slider (0.0 to disable ambient, punctual lights only).

### Phase 3.5: NVIDIA RTXPT Microfacet Multi-Lobe BSDF Model ✅ COMPLETED
- **Objective**: Upgraded `PathTracer.slang` to NVIDIA RTXPT's full multi-lobe microfacet BSDF standard.
- **Implemented**:
  1. Extended `InstanceLUT` in `shaderIO.h` and `Renderer.cpp` with `metallicFactor`, `roughnessFactor`, `metallicRoughnessTextureIndex`, `normalTextureIndex`, `transmissionFactor`, `transmissionTextureIndex`, and `workflow` (160 bytes).
  2. Integrated Eric Heitz 2018 GGX VNDF sampling (`sample_ggx_vndf`) for specular reflection bounce rays.
  3. Cook-Torrance direct lighting (NEE) with height-correlated Smith $G_2$, GGX $D$, and Schlick Fresnel.
  4. Multi-lobe indirect ray generation: Fresnel-weighted specular bounce for metals/glossy, Snell's law refraction ray generation for glass/transmission ($n = 1.5$), and cosine diffuse bounce for dielectrics.
  5. Tangent-space normal mapping perturbation (`perturb_normal`).

### Phase 4: Standalone NRD SIGMA & Shadow Denoising ✅ COMPLETED
- Dedicated penumbra-aware contact hardening for 1-SPP shadow rays in Deferred Lighting.
- HAL interface in `NRI::Device.h` (`initNRD`, `evaluateNRDShadows`, `destroyNRD`).
- NRD SIGMA integration in `NRI/Vulkan/DeviceVK` using `nrd::Integration` and `NRIWrapperVK` (Zero Graphics Leak).
- Debug View 13 displays raw vs denoised shadow mask in real time.

### Phase 4.5: NRD REBLUR / RELAX Decoupled Specular Reflections ✅ COMPLETED
- Decoupled 1-SPP GGX VNDF ray-traced reflections denoised via NRD REBLUR (variance-guided) and RELAX (A-Trous wavelet).
- `Reflection.slang` evaluates 1-SPP GGX VNDF reflection rays against TLAS with hit radiance and hit distance output.
- Strict runtime arbitration UI toggle (mutually exclusive with DLSS-RR).
- Physically correct specular Fresnel blending in `DeferredLighting.slang` (preserving 96% dielectric albedo on foliage and 100% metal reflections).

### Phase 5: Dual-Track Indirect Diffuse GI (DDGI Probes vs RTXDI ReSTIR GI) 🚀 IN PROGRESS
- **Objective**: Implement real-time multi-bounce diffuse indirect illumination with a runtime toggle between **3D Octahedral Probe Fields (DDGI)** and **Screen-Space Ray Resampling (RTXDI ReSTIR GI)** to benchmark graphics, VRAM, and performance.
- **Track A: Dynamic Diffuse Global Illumination (DDGI Probes)**: ✅ COMPLETED
  1. Complete DDGI shader suite created:
     - `DDGICommon.slang`: Octahedral mapping, spherical Fibonacci sampling, Rodrigues 3D rotation, Chebyshev visibility test, and multi-probe trilinear interpolation.
     - `DDGIRadiance.slang`: Inline `RayQuery` tracing 128 rays per probe against TLAS evaluating direct lights, emissive, and multi-bounce irradiance.
     - `DDGIBlendIrradiance.slang`: Octahedral irradiance atlas integration with temporal hysteresis ($\alpha = 0.97$) and 1-pixel border wrapping.
     - `DDGIBlendDistance.slang`: Octahedral distance moments ($r, r^2$) integration with temporal hysteresis and border wrapping.
     - `DDGIProbeSpheres.slang`: Mesh/task shader rendering 3D instanced octahedron spheres (18 vertices, 32 triangles per workgroup) colored by probe irradiance.
  2. Integration in Deferred Lighting:
     - Multi-probe trilinear interpolation with normal biasing and Chebyshev visibility weighting in `DeferredLighting.slang`.
  3. Visual Debug Modes:
     - **Mode 16**: Indirect Diffuse GI Only (DDGI).
     - **Mode 17**: DDGI Probe Grid Spheres (Reverse-Z depth tested in Forward 3D Pass).
  4. Interactive Editor Controls:
     - Real-time sliders for Grid Origin, Grid Spacing, Temporal Hysteresis, Normal Bias, Debug Sphere Radius, and "Reset DDGI History" button.
- **Track B: RTXDI ReSTIR GI (Screen-Space Path Resampling)**: 🚀 NEXT UP
  1. Trace 1 indirect diffuse ray per screen pixel from primary G-Buffer hit points.
  2. Resample and share indirect paths across spatial neighbors and temporal history via RTXDI ReSTIR GI reservoirs.
  3. Denoise the resulting diffuse radiance buffer using NRD `REBLUR_DIFFUSE` or DLSS-RR.
- **Comparison & UI Controls**:
  - Runtime combo dropdown: `[ Indirect GI: DDGI Probes | RTXDI ReSTIR GI | Off (IBL Cubemap) ]`.
  - Side-by-side performance profiling (ms/frame, VRAM consumption, visual comparison).

### Phase 6: Many-Light Direct Illumination & Area Lights via RTXDI (ReSTIR DI)
- **Objective**: Scale direct lighting to thousands of dynamic shadow-casting lights (point, spot, directional, rectangular/disc area lights) at 1-ray-per-pixel across both the Hybrid Deferred Renderer and the Path Tracer.
- **Components**:
  1. **Linearly Transformed Cosines (LTC)** LUT evaluation for rectangular and disc area lights.
  2. **RTXDI ReSTIR DI Core Pipeline**:
     - Light tile presampling pass.
     - Spatiotemporal reservoir generation and reuse.
     - Single shadow ray test for the winning reservoir light.
  3. **Path Tracer Integration (ReSTIR DI + ReSTIR PT)**:
     - Replace random Next Event Estimation (NEE) in `PathTracer.slang` with RTXDI reservoir sampling for instantaneous noise-free direct lighting.
     - Integrate ReSTIR PT for multi-bounce path importance resampling.

### Phase 7: Ray-Traced Ambient Occlusion (RTAO) & Volumetric Froxels
- **Objective**: Physical contact darkening and atmospheric light shafts.
- **Tasks**:
  1. RTAO 1-spp cosine-weighted hemisphere ray query ($d \le 3.0\text{m}$) replacing SSAO.
  2. Allocate 3D Frustum-aligned Froxel Grid ($160 \times 90 \times 64$, `RGBA16_SFLOAT`).
  3. Compute froxel in-scattering from lights shadowed against the TLAS.
  4. Front-to-back raymarching composite into HDR scene.

### Phase 8: NVIDIA Reflex Low-Latency Integration
- **Objective**: Ultra-low input latency and frame pacing synchronization.
- **Tasks**:
  1. Integrate `sl.reflex.dll` and `NvLowLatencyVk.dll`.
  2. Implement PCL markers (`eSimulationStart`, `eRenderSubmitEnd`, `ePresentStart`, etc.).
  3. Reflex refresh-rate synchronization toggle in Editor UI.

---

## 6. Current Implementation Progress Checkpoint

| Feature / Phase | Technology | Status |
| :--- | :--- | :---: |
| **Meshlet Rasterization** | `VK_EXT_mesh_shader` + Task Shader | ✅ Complete |
| **Visibility Buffer** | $R32G32\_UINT$ Primitive/Meshlet IDs | ✅ Complete |
| **Decoupled G-Buffer** | BaseColor, Normal, Metallic, Roughness, Emission, Specular $F_0$ | ✅ Complete |
| **Acceleration Structures** | TLAS & BLAS Rebuild & Compaction | ✅ Complete |
| **Ray Query Hard Shadows** | Directional, Point, Spot (`VK_KHR_ray_query`) | ✅ Complete |
| **Alpha Cutout Shadows** | Dynamic `alphaMode` evaluation via InstanceLUT | ✅ Complete |
| **Camera Motion Vectors & Jitter** | Pre/Post Projection delta + Halton(2,3) | ✅ Complete (Phase 1) |
| **Streamline DLSS & DLSS-RR** | `sl.dlss`, `sl.dlss_d` Vulkan Host Integration | ✅ Complete (Phase 2) |
| **Interactive Path Tracer Core** | `VK_KHR_ray_tracing_pipeline` + SBT + Progressive Accumulation | ✅ Complete (Phase 3) |
| **RTXPT Microfacet Multi-Lobe BSDF** | GGX VNDF, Metals, Roughness, Tangent Normals, Glass Transmission | ✅ Complete (Phase 3.5) |
| **Standalone NRD SIGMA** | `RayTracingDenoiser` Shadow Mask Penumbra Hardening | ✅ Complete (Phase 4) |
| **NRD REBLUR / RELAX & Auto-Arbitration** | Specular Reflections Denoising (Decoupled 1-SPP Pass) | ✅ Complete (Phase 4.5) |
| **Dual-Track Indirect Diffuse GI** | **DDGI Probes vs RTXDI ReSTIR GI (Hybrid Renderer Switch)** | 🚀 **NEXT UP (Phase 5)** |
| **Many-Light RTXDI & Area Lights** | **ReSTIR DI (Deferred + Path Tracer) + ReSTIR PT + LTC Area Lights**| ⏳ Phase 6 |
| **RTAO & Volumetric Froxels** | 3D Frustum Grid + Ray Query Occlusion | ⏳ Phase 7 |
| **NVIDIA Reflex Low-Latency** | `sl.reflex` + PCL Markers + Refresh Synchronization | ⏳ Phase 8 |

