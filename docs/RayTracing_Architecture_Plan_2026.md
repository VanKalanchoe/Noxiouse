# NoxEngine: Modern Ray Tracing & Lighting Architecture Plan (2026)

> **Document Version:** 3.0 (Master Blueprint & Collaboration Contract — March 2026)  
> **Target API:** Vulkan 1.3 / 1.4 (`VK_KHR_ray_query`, `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_EXT_descriptor_buffer`, `VK_EXT_shader_object`)  
> **Shading Language:** Slang (SPIR-V Target)  
> **Orchestration & Denoising Framework:** NVIDIA Streamline (SL) + NRD + DLSS / DLSS-RR  
> **Industry Benchmarks:** Unreal Engine 5.4 (Lumen & MegaLights), Cyberpunk 2077 (REDengine 4 RT Overdrive), NVIDIA RTX SDKs  

---

## 1. Collaboration Contract & Engineering Workflow

To maintain absolute code safety, stability, and mutual verification, the development of this rendering architecture strictly adheres to the following rules:

1. **User Role**:
   - The **User exclusively performs all code editing**, file saves, compilations, and engine executions.
   - The User tests each visual debug view mode in the viewport and provides feedback/validation before advancing to the next step.
2. **Assistant Role**:
   - The **Assistant never edits engine or editor code files directly via tool calls**.
   - The Assistant provides precise, step-by-step instructions, exact file paths, line numbers, and clean copy-pasteable code snippets.
   - The Assistant provides direct links to the official programming guides, GitHub repositories, and papers for every referenced technology so every algorithm and parameter can be cross-checked.
3. **Phase Advancement Gate**:
   - No phase begins until the previous phase's interactive debug view mode is visually confirmed running and stable.

---

## 2. Official Documentation, Repositories & Programming Guides

The following official documentation, source code repositories, and technical whitepapers serve as the ground-truth specifications for NoxEngine's implementation:

### 2.1. NVIDIA Streamline (SL)
- **Official GitHub Repository**: [NVIDIAGameWorks/Streamline](https://github.com/NVIDIAGameWorks/Streamline)
- **Programming Guide**: [Streamline Programming Guide](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuide.md)
- **Vulkan Integration Guide**: [Streamline Vulkan Guide](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideVulkan.md)
- **DLSS & Motion Vectors Guide**: [Streamline DLSS Guide](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)

### 2.2. NVIDIA NRD (Real-Time Denoisers)
- **Official GitHub Repository**: [NVIDIAGameWorks/RayTracingDenoiser](https://github.com/NVIDIAGameWorks/RayTracingDenoiser)
- **Architecture Whitepaper**: [NRD Whitepaper (PDF)](https://github.com/NVIDIAGameWorks/RayTracingDenoiser/blob/master/NRD.pdf)
- **Engine Integration Guide**: [NRD Integration Guide](https://github.com/NVIDIAGameWorks/RayTracingDenoiser/blob/master/Integration.md)
- **SIGMA (Shadow Denoiser) Specs**: [NRD Denoisers Overview](https://github.com/NVIDIAGameWorks/RayTracingDenoiser/tree/master/Shaders)

### 2.3. NVIDIA RTXGI & DDGI (Dynamic Diffuse Global Illumination)
- **Official GitHub Repository**: [NVIDIAGameWorks/RTXGI](https://github.com/NVIDIAGameWorks/RTXGI)
- **RTXGI DDGI Guide**: [RTXGI DDGI Implementation Guide (PDF)](https://github.com/NVIDIAGameWorks/RTXGI/blob/main/RTXGI-DDGI-Guide.pdf)
- **Foundational Paper**: [Majercik et al., *Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields*, JCGT 2019](https://jcgt.org/published/0008/02/01/)

### 2.4. NVIDIA RTXDI (ReSTIR Direct Illumination)
- **Official GitHub Repository**: [NVIDIAGameWorks/RTXDI](https://github.com/NVIDIAGameWorks/RTXDI)
- **RTXDI Programming Guide**: [RTXDI Programming Guide (PDF)](https://github.com/NVIDIAGameWorks/RTXDI/blob/main/doc/RTXDI_Programming_Guide.pdf)
- **Foundational ReSTIR Paper**: [Bitterli et al., *Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing with Dynamic Direct Lighting*, SIGGRAPH 2020](https://cs.dartmouth.edu/wjarosz/publications/bitterli20spatiotemporal.html)

### 2.5. Vulkan Ray Tracing Pipeline & Shader Binding Table (SBT)
- **NVIDIA `nvvk::SBTGenerator` Source**: [nvpro_core/nvvk/raytraceKHR_vk.hpp](https://github.com/nvpro-samples/nvpro_core/blob/master/nvvk/raytraceKHR_vk.hpp)
- **Vulkan Ray Tracing Tutorial**: [NVIDIA Vulkan Ray Tracing Tutorial (KHR)](https://nvpro-samples.github.io/vk_raytracing_tutorial_KHR/)

### 2.6. Area Lights (Linearly Transformed Cosines - LTC)
- **Foundational Paper & Code**: [Heitz et al., *Real-Time Polygonal-Light Shading with Linearly Transformed Cosines*, ACM SIGGRAPH 2016](https://eheitzresearch.wordpress.com/415-2/)

### 2.7. Camera Jitter & Temporal Sequences
- **Halton Sequence**: [Halton Sequence Reference (Wikipedia)](https://en.wikipedia.org/wiki/Halton_sequence)
- **DLSS Camera Jitter Specification**: [NVIDIA DLSS Camera Jitter Requirements](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS.md#camera-jitter)

---

## 3. Executive Summary & Architecture Philosophy

NoxEngine adopts a modern, high-performance hybrid rendering architecture combining:
1. **Visibility Buffer Rasterization**: Nanite-style meshlet rasterization producing decoupled visibility geometry (`R32G32_UINT`).
2. **Decoupled Deferred G-Buffer**: Material evaluation decoupled from geometry rasterization.
3. **NVIDIA Streamline (SL) Plugin Ecosystem**: A unified, vendor-neutral orchestration layer managing:
   - **NVIDIA Real-Time Denoisers (NRD)**: Cross-vendor denoisers running on AMD, Intel, and NVIDIA for soft shadows (`SIGMA`), reflections (`ReBLUR`), and ambient occlusion.
   - **DLSS 3.5 / DLSS-RR (Ray Reconstruction)**: AI neural reconstruction replacing separate heuristic denoisers for reflections, GI, and path tracing on NVIDIA RTX hardware.
   - **Multi-Vendor Upscalers**: Streamline plugin support for AMD FSR and Intel XeSS alongside DLSS.
4. **Dynamic Diffuse Global Illumination (DDGI)**: Probe-based multi-bounce indirect GI providing rock-solid, leak-free bounce lighting with zero screen-space boiling noise.
5. **RTXDI (ReSTIR Direct Illumination)**: Reservoir-sampled direct lighting supporting hundreds of dynamic shadow-casting area and punctual lights at 1-2 rays per pixel.
6. **Path Tracing Pipeline (`VK_KHR_ray_tracing_pipeline`)**: Monolithic RT pipeline with `nvvk::SBTGenerator` for interactive ground-truth reference, offline baking, and cinematic rendering.

---

## 4. Industry Standards Comparison (UE 5.4, Cyberpunk 2077, NoxEngine)

| Architecture Component | Unreal Engine 5.4 (Lumen & MegaLights) | Cyberpunk 2077 (REDengine 4 RT Overdrive) | NoxEngine (2026 Plan) |
| :--- | :--- | :--- | :--- |
| **Geometry Raster** | Nanite Meshlets | Traditional Mesh Pipeline | Nanite-Style VisBuffer Meshlets (`VK_EXT_mesh_shader`) |
| **Direct Lighting** | MegaLights (ReSTIR DI) | RTXDI (ReSTIR DI) | Analytical Direct Lights -> RTXDI (Phase 4) |
| **Soft Shadows** | Virtual Shadow Maps + HWRT Shadows | Ray Tracing Shadows + NRD SIGMA | Ray Query Stochastic Penumbra + NRD SIGMA (Phase 2 & 4) |
| **Global Illumination** | Lumen Radiance Cache (World Probes + Screen Probes) | Multi-Bounce Path Traced Indirect GI | DDGI Probe Irradiance Field (Phase 3) |
| **Specular Reflections** | Lumen HWRT Rough Reflections + Bilateral Filter | Ray Traced GGX Reflections | Ray Query Mirror (Done) + Rough GGX + NRD ReBLUR (Phase 2) |
| **Denoising Framework** | Custom Spatio-Temporal Bilateral & TAA | NVIDIA Streamline (NRD + DLSS-RR) | NVIDIA Streamline (NRD + DLSS-RR) (Phase 2) |
| **Cinematic Ground Truth** | Unreal Path Tracer (Offline / Reference) | RT Overdrive Path Tracer (Real-Time) | Monolithic RT Pipeline + `nvvk::SBTGenerator` |

---

## 5. The Testing Dilemma: How Do We Test Streamline in Phase 2?

Streamline is modular: it hosts **DLSS Super Resolution**, **NRD SIGMA** (soft shadows), and **NRD ReBLUR** (reflections). We verify each step-by-step with immediate visual feedback:

```
                                  Streamline Phase 2 Test Plan
                                                │
         ┌──────────────────────────────────────┼──────────────────────────────────────┐
         ▼                                      ▼                                      ▼
   [Test 1: DLSS SR]                    [Test 2: NRD SIGMA]                    [Test 3: NRD ReBLUR]
   Render at 50% res (960x540)          Add 1-spp cone jitter to               Add 1-spp GGX jitter to
   Upscale to 1080p via DLSS            shadow rays in DeferredLighting        reflection rays in DeferredLighting
   Verify: Camera jitter &              Verify: Raw noisy penumbra             Verify: Rough surface speckles
   motion vector stability              instantly smooths out                  instantly smooth out
```

1. **Test 1: DLSS Super Resolution (Immediate with Phase 1)**:
   * **Input**: Current Clean Deferred Lighting Output + Depth + Motion Vectors + Subpixel Jitter.
   * **Verification**: Render the viewport at $50\%$ internal resolution (e.g. $960 \times 540$) and run `slEvaluateFeature(kFeatureDLSS)`.
   * **Expected Result**: Viewport renders razor-sharp at $1920 \times 1080$ with stable edges. If motion vectors or camera jitter are inverted, you see instant ghosting/smearing. If correct, the image is rock-solid.
2. **Test 2: NRD SIGMA Shadow Denoising Verification**:
   * **Input**: In `DeferredLighting.slang`, fire 1 stochastic cone ray per pixel (producing Monte Carlo noise).
   * **Verification**: Feed the 1-spp noisy shadow mask to Streamline `slEvaluateFeature(kFeatureNRD)` targeting `SIGMA`.
   * **Expected Result**: 
     - **NRD OFF (Debug View 13)**: Grainy, noisy penumbra.
     - **NRD ON**: Smooth, contact-hardened penumbra with crisp contact shadows near occluder base.
3. **Test 3: NRD ReBLUR Reflection Denoising Verification**:
   * **Input**: In `DeferredLighting.slang`, add GGX importance-sampling jitter to the reflection ray direction based on surface roughness ($1$ ray per pixel). On rough materials, this produces noisy reflection speckles.
   * **Verification**: Feed noisy reflections to Streamline `slEvaluateFeature(kFeatureNRD)` targeting `ReBLUR`.
   * **Expected Result**:
     - **NRD OFF (Debug View 14)**: Rough surfaces have boiling white/colored speckles.
     - **NRD ON**: Smooth, physically accurate glossy reflections.

---

## 6. Comprehensive Debugging Suite & Interactive Viewport Toggles

In line with Unreal Engine 5 and Cyberpunk 2077, every stage of the pipeline is isolated with an interactive debug view mode selectable directly from the editor viewport:

| Mode ID | Name | Visual Output & Debugging Purpose |
| :---: | :--- | :--- |
| **0** | **Final Lit (PBR + GI + Post)** | Full engine output with Tonemapping, DDGI, Soft Shadows, Reflections, and Denoising. |
| **1** | **VisBuffer Meshlets** | Pseudo-colored mosaic verifying indirect meshlet rasterization and primitive culling. |
| **2** | **Barycentrics / UVs** | Perspective-correct reconstructed UV coordinates (`frac(u), frac(v)`). |
| **3** | **Albedo (BaseColor)** | Demodulated surface diffuse color without any lighting. |
| **4** | **World Normals** | Octahedral / tangent perturbed normals transformed to world space ($[-1, 1] \to [0, 1]$). |
| **5** | **Roughness** | Grayscale roughness map (black = pure mirror, white = completely rough). |
| **6** | **Metallic** | Grayscale metallic mask (black = dielectric, white = conductor). |
| **7** | **Emission** | Raw emissive intensity and HDR bloom candidate pixels. |
| **8** | **Entity ID** | Unique integer colors per scene entity for viewport mouse-picking validation. |
| **9** | **Linear Depth** | Linearized Reverse-Z depth buffer verifying near/far planes. |
| **10** | **Direct Lighting Only** | Shading from analytical lights & area lights without bounce GI or reflections. |
| **11** | **DDGI Indirect Diffuse** | Multi-bounce indirect GI isolated without direct lighting or materials. |
| **12** | **Ambient Occlusion (RTAO)** | Grayscale contact shadowing factor ($d \le 3.0\text{m}$). |
| **13** | **Shadows: Raw vs Denoised** | Side-by-side or toggle: 1-spp stochastic penumbra vs NRD `SIGMA` smoothed shadow. |
| **14** | **Reflections: Raw vs Denoised** | Side-by-side or toggle: 1-spp rough GGX jitter vs NRD `ReBLUR` / DLSS-RR reflection. |
| **15** | **Motion Vectors (Velocity Buffer)**| Screen-space velocity encoded as RGB ($R = \Delta X, G = \Delta Y, B = 0$). |
| **16** | **DDGI Probe Grid Debug** | 3D visualizer rendering spheres at probe locations displaying probe irradiance. |
| **17** | **Path Tracing Reference** | Interactive progressive path tracer accumulating ground-truth samples for comparison. |

---

## 7. The 6-Phase Implementation Roadmap

```
Current Engine Baseline:
[Visibility Buffer] -> [Decoupled G-Buffer] -> [Deferred PBR + Hard RT Shadows & Mirror RT Reflections]
```

### Phase 1: Temporal Foundation (Motion Vectors & Camera Jitter)
- **Objective**: Establish the temporal history and subpixel jitter required by Streamline, NRD, DLSS, and TAA.
- **Reference**: [NVIDIA DLSS Motion Vector Guide](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS.md#motion-vectors)
- **Tasks**:
  1. Add `m_velocityResource` (`RG16_SFLOAT`) to G-Buffer resolve pass.
  2. In `GBufferMaterial.slang`, compute screen-space velocity:
     $$\text{velocity} = (\text{currHClip.xy} / \text{currHClip.w}) - (\text{prevHClip.xy} / \text{prevHClip.w})$$
  3. Store `m_prevViewProj` across frames in `Renderer`.
  4. Implement Halton(2, 3) 8-phase subpixel projection jitter in `Camera` / `EditorCamera`.
  5. Add **Debug View 15 (Motion Vectors)** in `DeferredLighting.slang` and Editor UI.

### Phase 2: NVIDIA Streamline (SL) Core Framework + NRD & DLSS Verification
- **Objective**: Establish the unified denoising and upscaling pipeline.
- **Reference**: [Streamline Programming Guide](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuide.md)
- **Tasks**:
  1. Integrate Streamline SDK headers and DLL binaries (`sl.interposer.dll`, `sl.nrd.dll`, `sl.dlss.dll`).
  2. Initialize Streamline in `DeviceVK` during Vulkan device creation.
  3. Wrap G-Buffer, Depth, and Motion Vectors into `sl::ResourceTag`.
  4. Verify Step 1: **DLSS Super Resolution** (`kFeatureDLSS`) to upscale the viewport and verify motion vector accuracy.
  5. Verify Step 2: Add 1-spp stochastic jitter to shadows and hook up **NRD SIGMA** (`kFeatureNRD`).
  6. Verify Step 3: Add 1-spp GGX roughness jitter to reflections and hook up **NRD ReBLUR**.
  7. Add UI toggles for DLSS quality and Denoiser modes (Raw Noisy vs Denoised).

### Phase 3: Dynamic Diffuse Global Illumination (DDGI)
- **Objective**: Noise-free, multi-bounce real-time indirect GI.
- **Reference**: [Majercik et al. JCGT 2019](https://jcgt.org/published/0008/02/01/), [NVIDIA RTXGI Guide](https://github.com/NVIDIAGameWorks/RTXGI/blob/main/RTXGI-DDGI-Guide.pdf)
- **Tasks**:
  1. Allocate DDGI probe grid buffers ($32 \times 16 \times 32$ probes) and 2D octahedral atlases:
     - Irradiance Atlas ($8 \times 8$ texels per probe, `RGBA16_SFLOAT`).
     - Distance Atlas ($16 \times 16$ texels per probe, `RG16_SFLOAT`).
  2. Implement `DDGIRadiance.comp.slang`: Trace 64–128 rays per probe against TLAS using `RayQuery`.
  3. Implement `DDGIUpdate.comp.slang`: Octahedral mapping and temporal accumulation with hysteresis ($\alpha = 0.97$).
  4. Sample DDGI in `DeferredLighting.slang` with trilinear probe interpolation and Chebyshev visibility test.
  5. Add **Debug View 11 (DDGI Indirect Diffuse)** and **Debug View 16 (Probe Grid Debug Spheres)**.

### Phase 4: Area Lights & Soft Shadows + RTXDI (ReSTIR DI)
- **Objective**: Physical area lights and many-light scaling with ReSTIR reservoir sampling.
- **Reference**: [RTXDI Programming Guide](https://github.com/NVIDIAGameWorks/RTXDI/blob/main/doc/RTXDI_Programming_Guide.pdf), [Heitz et al. LTC Paper](https://eheitzresearch.wordpress.com/415-2/)
- **Tasks**:
  1. Linearly Transformed Cosines (LTC) LUT evaluation for rectangular and disc area lights.
  2. Stochastic cone-sampled soft shadows denoised via Streamline NRD `SIGMA`.
  3. Integrate RTXDI (ReSTIR DI) compute passes:
     - Initial candidate light sampling.
     - Temporal reservoir reuse across frames.
     - Spatial reservoir reuse across neighbor pixels.
     - 1-shadow-ray visibility test for winning reservoir light.
  4. Support 500+ dynamic shadow-casting lights at fixed 1-ray-per-pixel cost.

### Phase 5: Ray-Traced Ambient Occlusion (RTAO) & Volumetric Froxels
- **Objective**: Physical contact darkening and atmospheric light shafts.
- **Tasks**:
  1. RTAO 1-spp cosine-weighted hemisphere ray query ($d \le 3.0\text{m}$) replacing SSAO.
  2. Denoise RTAO via Streamline NRD / bilateral filter (Debug View 12).
  3. Allocate 3D Frustum-aligned Froxel Grid ($160 \times 90 \times 64$, `RGBA16_SFLOAT`).
  4. Compute froxel in-scattering from lights shadowed against the TLAS.
  5. Front-to-back raymarching composite into HDR scene.

### Phase 6: Monolithic RT Pipeline & Path Tracing (`nvvk::SBTGenerator` + DLSS-RR)
- **Objective**: Ground-truth reference and cinematic offline rendering.
- **Reference**: [nvvk::SBTGenerator Source](https://github.com/nvpro-samples/nvpro_core/blob/master/nvvk/raytraceKHR_vk.hpp)
- **Tasks**:
  1. Integrate `nvvk::SBTGenerator` (header-only C++ utility) for Vulkan Shader Binding Table management.
  2. Add `RayTracingPipelineVK` in NRI wrapping `vkCreateRayTracingPipelinesKHR`.
  3. Write Slang RT shaders:
     - `PathTracer.rgen.slang`
     - `PathTracer.rchit.slang`
     - `PathTracer.rmiss.slang`
  4. Progressive accumulation pass with interactive camera freeze/reset.
  5. Enable **Streamline DLSS Ray Reconstruction (DLSS-RR)** on RTX hardware for real-time neural path tracing denoising.
  6. Add **Debug View 17 (Path Tracing Ground Truth)**.

---

## 8. Current Implementation Progress Checkpoint

| Feature | Technology | Status |
| :--- | :--- | :---: |
| Meshlet Rasterization | `VK_EXT_mesh_shader` + Task Shader | ✅ Complete |
| Visibility Buffer | $R32G32\_UINT$ Primitive/Meshlet IDs | ✅ Complete |
| Decoupled G-Buffer | BaseColor, Normal, Metallic, Roughness, Emission | ✅ Complete |
| Acceleration Structures | TLAS & BLAS Rebuild & Compaction | ✅ Complete |
| Ray Query Hard Shadows | Directional, Point, Spot (`VK_KHR_ray_query`) | ✅ Complete |
| Alpha Cutout Shadows | Dynamic `alphaMode` evaluation via InstanceLUT | ✅ Complete |
| Ray Query Mirror Reflections | G-Buffer depth unproject + TLAS reflection ray | ✅ Complete |
| Hot-Reload & Mesh Swap Safety | Device idle synchronization & TLAS cleanup | ✅ Complete |
| Camera Motion Vectors & Jitter | Pre/Post Projection delta + Halton(2,3) | ⏳ **Phase 1 (In Progress)** |
| RTX Streamline & NRD / DLSS | `sl.interposer`, `sl.nrd`, `sl.dlss` Vulkan Host | ⏳ **Phase 2** |
| Dynamic Diffuse GI (DDGI) | Slang Octahedral Probe Field | ⏳ **Phase 3** |
| Soft Shadows & RTXDI | ReSTIR DI + LTC Area Lights + NRD SIGMA | ⏳ **Phase 4** |
| RTAO & Volumetric Froxels | 3D Frustum Grid + Ray Query Occlusion | ⏳ **Phase 5** |
| Full Path Tracer | `VK_KHR_ray_tracing_pipeline` + SBTGenerator + DLSS-RR | ⏳ **Phase 6** |
