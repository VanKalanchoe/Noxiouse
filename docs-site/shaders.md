# Complete Shader Catalog (`NoxEditor/assets/shaders/` — 113 Files)

All shaders in Noxiouse are written in **Slang** (with shared HLSL/C++ headers) and live in `NoxEditor/assets/shaders/`. They are compiled to **SPIR-V 1.6** by [`SlangCompiler.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NRI/SlangCompiler.cpp) and use 100% bindless descriptor heaps (`VK_EXT_descriptor_heap`) and 64-bit buffer device addresses (`vkCmdPushDataEXT`).

:::{important}
**Rendering Pipeline Invariants**:
- **Primary Opaque Path**: `Material_PBR_MeshTask.slang` (task) + `VisibilityBuffer.slang` (mesh/frag) $\rightarrow$ `GBufferMaterial.slang` $\rightarrow$ `DeferredLighting.slang`.
- **Live Forward Path**: `Material_PBR_MeshTask.slang` (task) + `Material_PBR_Mesh.slang` (`meshMain` only; its `fragMain` is unused) paired with fragment shaders `Material_Unlit_Mesh.slang`, `Material_TransparentLit_Mesh.slang` (transparents & editor extra views), or `Wireframe.slang`.
- **Hardware Path Tracer**: `PathTracer/PathTracerRayGen.slang`, `PathTracerHit.slang`, `PathTracerAnyHit.slang`, `PathTracerMiss.slang` + `PTComposite.slang` (`PathTracer.slang` at root is legacy/uncompiled).
:::

---

## 1. Shared Headers, Material Lobes, GI & Ray-Hit Helpers (9 Files)

| # | Shader File | Plain-English Purpose & Key Functions |
| :--- | :--- | :--- |
| 1 | [`shaderIO.h`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/shaderIO.h) | Master 3,270-line C++/Slang shared header. Defines `UniformBufferObject`, `GpuInstance`, `GpuTransform`, `GpuMaterial`, `GpuMesh`, `PackedVertex` (36B), `SkinVertex` (16B), `LightData`, `EmissiveTriangle`, `DDGICascadeGpu`, and all `PushConstant*` structs, plus G-buffer packing, WBOIT weights, Virtual Texture sampling (`SampleVirtualTexture`), ray transmittance, area light solid-angle helpers, and Tokuyoshi-Kaplanyan specular AA (`geometric_specular_aa`). |
| 2 | [`Math.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Math.slang) | Slang `module common;` providing `SRGBtoLINEAR`. |
| 3 | [`MipFeedback.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/MipFeedback.slang) | Wave-deduplicated (`WaveReadLaneFirst`) atomic mip-streaming (`reportMipFeedback`) and virtual-texture tile-streaming (`reportVirtualTextureFeedback`) feedback helpers. |
| 4 | [`MaterialLobes.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/MaterialLobes.slang) | Evaluates and packs/unpacks glTF 2.0 extension textures (`evaluate_material_lobes`) for Clearcoat, Sheen, Specular, Iridescence, Anisotropy, and Dispersion. |
| 5 | [`MaterialLobeMath.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/MaterialLobeMath.slang) | Pure BRDF math shared by raster PBR and the Path Tracer: anisotropic GGX (`D_GGX_anisotropic`, `V_GGX_anisotropic`), Charlie sheen (`D_charlie`, `V_charlie`), and Belcour-Barla thin-film iridescence (`iridescence_fresnel`). |
| 6 | [`LightSelection.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/LightSelection.slang) | Implements `select_lights`: picks the 4 strongest analytic lights deterministically + 1 weighted RIS sample from all remaining lights so secondary ray hits shade in $O(1)$ shadow rays. |
| 7 | [`SceneGI.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/SceneGI.slang) | Unified indirect diffuse lookup (`sample_scene_gi`): queries Screen-Probe GI if on-screen, else Cascaded DDGI volumes, else IBL irradiance cubemap. |
| 8 | [`RayHitShading.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RayHitShading.slang) | Shades secondary ray hits (`shadeRayHit`) in RT Reflections and RT Glass Refraction with full PBR + glTF lobes, `select_lights`, colored shadow rays, `sample_scene_gi`, and IBL. |
| 9 | [`NRDFrontEnd.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/NRDFrontEnd.slang) | Slang port of NVIDIA NRD v4.17 signal packing/unpacking (`REBLUR_FrontEnd_*`, `RELAX_FrontEnd_*`, `SIGMA_FrontEnd_*`, `NRD_PackSignal`, YCoCg helpers). |

---

## 2. GPU Culling, Hi-Z, Skinning & Geometry Streaming (5 Files)

| # | Shader File | Entry Points | Plain-English Purpose |
| :--- | :--- | :--- | :--- |
| 10 | [`InstanceCulling.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/InstanceCulling.slang) | `cullMain`, `countMain`, `offsetMain`, `writeMain`, `lateMain` | Two-phase GPU frustum + Reverse-Z Hi-Z occlusion culling and indirect mesh-shader command compaction (`VkDrawMeshTasksIndirectCommandEXT`). |
| 11 | [`HiZBuild.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/HiZBuild.slang) | `buildMain` | Builds the Reverse-Z farthest-depth (`min`) mip pyramid for conservative occlusion culling. |
| 12 | [`ComputeSkinning.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ComputeSkinning.slang) | `compMain` | Evaluates vertex morph target deltas followed by 4-bone Linear Blend Skinning into the `SkinnedVertex` buffer. |
| 13 | [`GeometryStreamingFeedback.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/GeometryStreamingFeedback.slang) | `computeMain` | Evaluates screen-space error of cooked Cluster DAG pages (`GeometryPageGpuHeader`), marks resident pages touched, and requests missing finer pages. |
| 14 | [`ClusterIndices.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ClusterIndices.slang) | `computeMain` | Unpacks triangles from the finest resident Cluster DAG cut into flat 32-bit triangle indices for ray-tracing BLAS construction. |

---

## 3. Visibility Buffer, G-Buffer, Forward, Transparency, OIT & Particles (11 Files)

| # | Shader File | Entry Points | Plain-English Purpose |
| :--- | :--- | :--- | :--- |
| 15 | [`Material_PBR_MeshTask.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Material_PBR_MeshTask.slang) | `taskMain` | Shared task shader for **all** meshlet pipelines: evaluates Cluster LOD DAG cuts (`lodBoundsError`), frustum-culls meshlets, and compacts visible meshlets via wave ballot. |
| 16 | [`Material_PBR_Mesh.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Material_PBR_Mesh.slang) | `meshMain` (`fragMain` unused) | Shared mesh stage for all forward pipelines: transforms meshlet vertices, applies skinning and clip-space depth bias, and outputs triangles to forward fragment shaders. |
| 17 | [`VisibilityBuffer.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/VisibilityBuffer.slang) | `meshMain`, `fragMain` | Primary deferred mesh/fragment shader: performs alpha-cutout test and writes 64-bit `(instanceSlot + 1, meshletID << 8 \| primID)`, `entityID`, and 2D motion vectors. |
| 18 | [`GBufferMaterial.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/GBufferMaterial.slang) | `taskMain`, `meshMain`, `fragMain`, `fragMainNoVelocity` | Fullscreen Visibility Buffer resolve pass: computes analytic ray-differential barycentrics (`ddx`/`ddy`), reports mip/VT feedback, applies geometric specular AA, and writes up to 8 G-Buffer MRTs. |
| 19 | [`Material_Unlit_Mesh.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Material_Unlit_Mesh.slang) | `fragMain`, `fragMainOIT` | Forward fragment shader for `KHR_materials_unlit` meshes (opaque/alpha-blend and WBOIT) with height fog and aerial perspective. |
| -- | [`ToonShading.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ToonShading.slang) | (library) | The Toon shading model: banded or ramp diffuse, sharp highlight, rim (`toon_direct_light`), used by the deferred lighting and the transparent forward shader. |
| 20 | [`Material_TransparentLit_Mesh.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Material_TransparentLit_Mesh.slang) | `fragMain`, `fragMainOIT` | Forward PBR fragment shader for transparent/glass meshes and editor extra views: supports inline RT shadows, multi-bounce RT volume refraction with TIR + Beer-Lambert absorption + RGB dispersion, `ColorPyramid` rough refraction, and 2-pass stacked glass. |
| 21 | [`OITComposite.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/OITComposite.slang) | `vertMain`, `fragMain` | Fullscreen composite pass for McGuire & Bavoil Weighted Blended Order-Independent Transparency. |
| 22 | [`ColorPyramid.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ColorPyramid.slang) | `copyMain`, `downsampleMain` | Builds the mipmapped scene color pyramid behind transparent objects for roughness-based screen-space refraction. |
| 23 | [`Particles.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Particles.slang) | `meshMain`, `fragMain`, `fragMainOIT`, `fragMainMotion` | Mesh-shader billboard particle renderer with per-particle volumetric lighting (DDGI/IBL + analytic lights + RT sun shadow ray), flipbook animation, soft depth fade, and particle motion vectors. |
| 24 | [`Wireframe.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Wireframe.slang) | `fragMain` | Fragment shader for `PolygonMode::Line` meshlet wireframe overlay. |
| 25 | [`LightShapes.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/LightShapes.slang) | `taskMain`, `meshMain`, `fragMain` | Procedurally generates and renders visible emissive `Rect` and `Disc` area light geometry in the forward pass. |

---

## 4. Physical Sky, Volumetric Clouds, Height Fog & IBL Precomputation (10 Files)

| # | Shader File | Entry Points / Role | Plain-English Purpose |
| :--- | :--- | :--- | :--- |
| 26 | [`SkyAtmosphereCommon.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/SkyAtmosphereCommon.slang) | Shared module | Planetary radii (`6360km`/`6460km`), ray-sphere intersections, Bruneton horizon-aware transmittance LUT UVs, and cylindrical horizon-band mapping. |
| 27 | [`SkyAtmosphere.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/SkyAtmosphere.slang) | 9 compute entry points | Precomputes Hillaire transmittance & multi-scatter LUTs, 3D Perlin-Worley cloud shape/detail noise & 2D weather map, raymarches sky + volumetric clouds into the sky cubemap & 2048x256 horizon band, builds the 2D cloud shadow map, and raymarches the 32x32x32 camera aerial perspective froxel volume. |
| 28 | [`CloudNoise.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/CloudNoise.slang) | Shared module | Tileable 3D Perlin, Worley, and Perlin-Worley FBM procedural noise functions for volumetric clouds. |
| 29 | [`Skybox.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Skybox.slang) | `taskMain`, `meshMain`, `fragMain` | Renders the fullscreen skybox: blends the sky cubemap + sharp horizon band, limb-darkened sun disc, cratered Lommel-Seeliger moon disc with earthshine, and anti-aliased blackbody starfield. |
| 30 | [`HeightFog.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/HeightFog.slang) | Shared module | Exponential height fog with directional Henyey-Greenstein sun in-scattering combined with 3D aerial perspective froxel lookup (`sceneFogTerms`). |
| 31 | [`EquirectToCubemap.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/EquirectToCubemap.slang) | `taskMain`, `meshMain`, `fragMain` | Converts an equirectangular `.hdr` panorama into a 6-face environment cubemap. |
| 32 | [`CubemapResample.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/CubemapResample.slang) | `taskMain`, `meshMain`, `fragMain` | Downsamples environment cubemap mip chains before specular prefiltering. |
| 33 | [`IrradianceConvolution.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/IrradianceConvolution.slang) | `taskMain`, `meshMain`, `fragMain` | Convolves an environment cubemap over the hemisphere into a diffuse irradiance cubemap. |
| 34 | [`PrefilterEnv.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PrefilterEnv.slang) | `taskMain`, `meshMain`, `fragMain` | GGX importance-samples the environment cubemap across roughness mips for split-sum specular IBL. |
| 35 | [`BRDFIntegration.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/BRDFIntegration.slang) | `taskMain`, `meshMain`, `fragMain` | Integrates the 2D split-sum BRDF LUT (`R,G` = GGX scale/bias, `B` = Charlie sheen directional albedo). |

---

## 5. Deferred Lighting, RT Shadows/Reflections/AO, DDGI & Screen-Probe GI (23 Files)

| # | Shader File | Entry Points / Role | Plain-English Purpose |
| :--- | :--- | :--- | :--- |
| 36 | [`PBRLighting.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PBRLighting.slang) | Shared module | Cook-Torrance GGX + Smith height-correlated BRDF, exact polygon solid-angle area lights (`evaluateRectLight`), split-sum IBL with Lagarde specular occlusion, and multi-lobe evaluation (`evaluateDirectLightLobes`). |
| 37 | [`DeferredLighting.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/DeferredLighting.slang) | `taskMain`, `meshMain`, `fragMain` | Master fullscreen deferred lighting shader: combines direct lighting (analytic/area lights + NRD shadow mask + inline local shadow rays + cloud shadows, or ReSTIR DI), indirect diffuse (Screen-Probe GI, ReSTIR GI, or Cascaded DDGI), indirect specular (RT Reflections + IBL), SSAO/RTAO, fog, and 23 debug view modes. |
| 38 | [`EmissiveLightGrid.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/EmissiveLightGrid.slang) | `trianglesMain`, `scanMain`, `buildMain` | Extracts world-space emissive mesh triangles, computes a prefix-sum power CDF, and builds a 3D camera-centered RIS light grid (16 lights/cell). |
| 39 | [`NRDGuides.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/NRDGuides.slang) | `taskMain`, `meshMain`, `fragMain` | Writes linear `viewZ` and packed normal+roughness guide textures for NRD denoisers. |
| 40 | [`RRGuides.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RRGuides.slang) | `guidesMain` | Generates DLSS Ray Reconstruction guides: diffuse/specular albedos and planar-mirror virtual-image specular motion vectors. |
| 41 | [`ShadowMask.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ShadowMask.slang) | `taskMain`, `meshMain`, `fragMain` | Decoupled ray-traced soft directional shadow pass (Vogel-disk cone sampling + NRD SIGMA penumbra encoding). |
| 42 | [`Reflection.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Reflection.slang) | `taskMain`, `meshMain`, `fragMain` | Decoupled GGX VNDF ray-traced reflection pass with clearcoat/anisotropy support and full-res smooth mirror fill. |
| 43 | [`RTAO.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTAO.slang) | `taskMain`, `meshMain`, `fragMain` | Ray-traced ambient occlusion pass using stratified cosine-weighted hemisphere `RayQuery` rays. |
| 44 | [`SSAO.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/SSAO.slang) | `taskMain`, `meshMain`, `fragMain` | Screen-space ambient occlusion pass over Reverse-Z depth and G-buffer normals. |
| 45 | [`SSAOBlur.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/SSAOBlur.slang) | `taskMain`, `meshMain`, `fragMain` | 5x5 depth-preserving bilateral blur for SSAO and RTAO. |
| 46 | [`RTUpsample.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTUpsample.slang) | `taskMain`, `meshMain`, `fragMain` | 3x3 joint depth-and-normal-weighted bilateral upsampler for half/quarter-res RT passes. |
| 47 | [`DDGICommon.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/DDGICommon.slang) | Shared module | Toroidal cascade scrolling, octahedral atlas encoding, and Chebyshev visibility-weighted trilinear probe sampling (`sample_ddgi_irradiance`). |
| 48 | [`DDGIRadiance.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/DDGIRadiance.slang) | `taskMain`, `meshMain`, `fragMain` | Traces Spherical Fibonacci rays per DDGI probe and evaluates direct lighting + infinite-bounce DDGI feedback. |
| 49 | [`DDGIRelocation.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/DDGIRelocation.slang) | `compMain` | Moves DDGI probes out of geometry/walls and classifies deeply trapped probes as `INACTIVE`. |
| 50 | [`DDGIBlendIrradiance.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/DDGIBlendIrradiance.slang) | `compMain` | Shared-memory compute pass blending probe rays into 6x6 octahedral irradiance tiles with temporal hysteresis and 1px border wrap. |
| 51 | [`DDGIBlendDistance.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/DDGIBlendDistance.slang) | `compMain` | Shared-memory compute pass blending probe ray hit distances into 14x14 Chebyshev visibility tiles. |
| 52 | [`DDGIProbeSpheres.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/DDGIProbeSpheres.slang) | `taskMain`, `meshMain`, `fragMain` | Renders procedural 3D debug spheres visualizing DDGI probe locations, states, and live irradiance. |
| 53 | [`ScreenProbeCommon.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ScreenProbeCommon.slang) | Shared module | Octahedral mapping, R2 jitter offsets, and plane-distance weighting for Lumen-style Screen-Probe GI. |
| 54 | [`ScreenProbePlace.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ScreenProbePlace.slang) | `placeMain` | Places one screen probe per 8x8 pixel tile on the G-buffer surface. |
| 55 | [`ScreenProbeTrace.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ScreenProbeTrace.slang) | `traceMain` | Traces 64 octahedral rays per screen probe, shades hits with DDGI bounce, and performs wave-collaborative 64-candidate RIS NEE against the 3D `EmissiveLightGrid`. |
| 56 | [`ScreenProbeIrradiance.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ScreenProbeIrradiance.slang) | `irradianceMain` | Convolves 8x8 probe radiance rays into a 6x6 octahedral irradiance tile (+1px border). |
| 57 | [`ScreenProbeFilter.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ScreenProbeFilter.slang) | `filterMain` | 3-iteration spatial filter (`stride = 1, 2, 4`) across neighboring screen probes weighted by plane distance and normal similarity. |
| 58 | [`ScreenProbeIntegrate.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/ScreenProbeIntegrate.slang) | `integrateMain` | Interpolates screen probes at full-res G-buffer pixels, traces short-range screen-space bent-normal occlusion, and accumulates temporal GI history with variance moment clamping. |

---

## 6. Hardware Path Tracer (`PathTracer/` & `PathTracer/RTXPT/` — 25 Files)

| # | Shader File | Role / Entry Point | Plain-English Purpose |
| :--- | :--- | :--- | :--- |
| 59 | [`PathTracer.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer.slang) | Legacy reference | Older fullscreen inline `RayQuery` path tracer (superseded by the `PathTracer/` RT pipeline). |
| 60 | [`PathTracerSignals.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracerSignals.slang) | Shared module | Demodulates path-traced primary-hit radiance into separate diffuse and specular signals for NRD denoising. |
| 61 | [`PTComposite.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PTComposite.slang) | `compositeMain` | Re-modulates NRD-denoised path-tracer diffuse & specular signals with G-buffer material albedos and adds primary emission. |
| 62 | [`YCoCgDecodeInPlace.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/YCoCgDecodeInPlace.slang) | `compMain` | Decodes NRD REBLUR YCoCg color output back to linear RGB in-place. |
| 63 | [`PathTracer/PathTracerRayGen.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/PathTracerRayGen.slang) | `rayGenMain` | Hardware RT RayGen shader: traces multi-bounce paths, executes smooth-glass delta branching & RGB wavelength dispersion, writes DLSS-RR specular motion vectors & albedos, and accumulates progressive samples. |
| 64 | [`PathTracer/PathTracerHit.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/PathTracerHit.slang) | `closestHitMain` | Hardware RT ClosestHit shader: evaluates Beer-Lambert volume absorption, dispersive IOR, 3-plane DLSS-RR guides (`writeSplitRRGuides`), unified NEE, multi-layer `StandardBSDF`, smooth-glass branch splitting, firefly suppression, and Russian roulette. |
| 65 | [`PathTracer/PathTracerAnyHit.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/PathTracerAnyHit.slang) | `anyHitMain` | Hardware RT AnyHit shader: alpha-cutout and stochastic alpha-blend opacity testing. |
| 66 | [`PathTracer/PathTracerMiss.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/PathTracerMiss.slang) | `missMain` | Hardware RT Miss shader: evaluates skybox/environment cubemap, visible sun/moon/area-light shapes, and hierarchical environment MIS weight. |
| 67 | [`PathTracer/EnvironmentImportance.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/EnvironmentImportance.slang) | `buildMain`, `reduceMain` | Builds an equal-area octahedral luminance mipmap quadtree over the environment cubemap for MIP-descent importance sampling. |
| 68 | [`PathTracer/PathPayload.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/PathPayload.slang) | Shared module | Compact `TraceRay` payload struct (`PathPayload`) and pack/unpack routines. |
| 69 | [`PathTracer/PathState.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/PathState.slang) | Shared module | Full path state (`PathState`), `RayCone` texture LOD spread propagation, 2-slot dielectric interior stack, and delta glass branch state. |
| 70 | [`PathTracer/PathTracerBridge.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/PathTracerBridge.slang) | Shared module | Connects Noxiouse's bindless GPU scene, virtual textures, and ray-cone LODs to `SurfaceInteraction` and `TraceVisibilityRay`. |
| 71 | [`PathTracer/PathTracerNEE.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/PathTracerNEE.slang) | Shared module | Unified Next-Event Estimation (`evaluateNEE`): combines 1 analytic light + 1 emissive triangle + 1 hierarchical environment sample into a single reservoir so only 1 visibility ray is traced per bounce. |
| 72 | [`PathTracer/EmissiveLights.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/EmissiveLights.slang) | Shared module | Samples the 3D emissive light grid (with global CDF binary-search fallback) and computes exact solid-angle MIS PDFs. |
| 73 | [`PathTracer/StandardBSDF.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/StandardBSDF.slang) | Shared module | Multi-layer BSDF wrapping `FalcorBSDF` with Clearcoat, Charlie Sheen, Thin-Film Iridescence, Anisotropy, and Specular extensions. |
| 74 | [`PathTracer/Ray.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/Ray.slang) | Shared module | `Ray` helper struct converting to `RayDesc`. |
| 75 | [`PathTracer/RTXPTSampling.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/RTXPTSampling.slang) | Shared module | Stateless Owen-scrambled Sobol sequence generator keyed by `(pixel, bounce, sampleIndex, effect)`. |
| 76 | [`PathTracer/LobeType.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/LobeType.slang) | Shared module | Bitmask flags for diffuse/specular/delta reflection and transmission lobes. |
| 77 | [`PathTracer/RTXPTCompat.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/RTXPTCompat.slang) | Shared module | ULP scale-aware ray origin offsetting (`ComputeRayOrigin`), firefly clamping, and ray-cone expansion helpers. |
| 78 | [`PathTracer/RTXPT/EnvMap.hlsli`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/RTXPT/EnvMap.hlsli) | Shared HLSL header | Equal-area octahedral mapping and `EnvMapSampler::MIPDescentSample` / `MIPDescentEvalPdf`. |
| 79 | [`PathTracer/RTXPT/BxDF.hlsli`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/RTXPT/BxDF.hlsli) | Shared HLSL header | Core Frostbite/Disney diffuse, Turquin multi-scatter energy compensation, anisotropic microfacet reflection, dielectric refraction, and `FalcorBSDF`. |
| 80 | [`PathTracer/RTXPT/BxDFConfig.hlsli`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/RTXPT/BxDFConfig.hlsli) | Shared HLSL header | Configures Frostbite diffuse and Smith correlated GGX masking. |
| 81 | [`PathTracer/RTXPT/Fresnel.hlsli`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/RTXPT/Fresnel.hlsli) | Shared HLSL header | Schlick, generalized Schlick, exact dielectric Fresnel (with TIR), and conductor Fresnel. |
| 82 | [`PathTracer/RTXPT/Microfacet.hlsli`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/RTXPT/Microfacet.hlsli) | Shared HLSL header | Isotropic & anisotropic GGX NDF, bounded VNDF sampling (`sampleGGX_BVNDF`), and Smith correlated masking. |
| 83 | [`PathTracer/RTXPT/MathConstants.hlsli`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PathTracer/RTXPT/MathConstants.hlsli) | Shared HLSL header | High-precision $\pi$, $1/\pi$, and floating-point epsilon constants. |

---

## 7. NVIDIA RTXDI / ReSTIR Shaders (`RTXDI/` — 15 Files)

| # | Shader File | Entry Points / Role | Plain-English Purpose |
| :--- | :--- | :--- | :--- |
| 84 | [`RTXDI/RTXDILightSampling.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/RTXDILightSampling.slang) | Shared module | RTXDI Application Bridge (`RAB_*`) and `RTXDI_SampleLightsAt` for Power-RIS and ReGIR 3D grid sampling. |
| 85 | [`RTXDI/Presampling/ReSTIRDIWriteLightPDF.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/Presampling/ReSTIRDIWriteLightPDF.slang) | `compMain` | Writes local light power into mip 0 of the Z-curve (Morton) PDF texture. |
| 86 | [`RTXDI/Presampling/ReSTIRDIReduceLightPDFMip.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/Presampling/ReSTIRDIReduceLightPDFMip.slang) | `compMain` | 2x2 sum-reduces the local light PDF mip chain down to 1x1. |
| 87 | [`RTXDI/Presampling/ReSTIRDIPresample.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/Presampling/ReSTIRDIPresample.slang) | `compMain` | Populates power-weighted RIS light tiles from the local-light PDF mipmap. |
| 88 | [`RTXDI/Presampling/ReSTIRDIPresampleReGIR.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/Presampling/ReSTIRDIPresampleReGIR.slang) | `compMain` | Populates 3D camera-centered ReGIR grid cells with distance-and-power-weighted light reservoirs. |
| 89 | [`RTXDI/DI/ReSTIRDIInitial.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/DI/ReSTIRDIInitial.slang) | `taskMain`, `meshMain`, `fragMain` | Generates initial per-pixel direct light reservoir candidates using Power-RIS or ReGIR. |
| 90 | [`RTXDI/DI/ReSTIRDITemporal.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/DI/ReSTIRDITemporal.slang) | `taskMain`, `meshMain`, `fragMain` | Temporally resamples direct light reservoirs across frames using G-buffer motion vectors and permutation sampling. |
| 91 | [`RTXDI/DI/ReSTIRDISpatial.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/DI/ReSTIRDISpatial.slang) | `taskMain`, `meshMain`, `fragMain` | Spatially resamples direct light reservoirs across neighboring pixels. |
| 92 | [`RTXDI/DI/ReSTIRDIFinalShading.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/DI/ReSTIRDIFinalShading.slang) | `taskMain`, `meshMain`, `fragMain` | Traces visibility rays for the winning DI reservoir + directional lights and outputs demodulated NRD diffuse & specular signals. |
| 93 | [`RTXDI/GI/ReSTIRGIInitial.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/GI/ReSTIRGIInitial.slang) | `taskMain`, `meshMain`, `fragMain` | Traces secondary diffuse bounce rays, shades hits via `RTXDI_SampleLightsAt`, and packs initial `RTXDI_GIReservoir`s. |
| 94 | [`RTXDI/GI/ReSTIRGITemporal.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/GI/ReSTIRGITemporal.slang) | `taskMain`, `meshMain`, `fragMain` | Temporally resamples GI reservoirs with Jacobian validation clamped to `[1/3, 3]`. |
| 95 | [`RTXDI/GI/ReSTIRGISpatial.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/GI/ReSTIRGISpatial.slang) | `taskMain`, `meshMain`, `fragMain` | Spatially resamples GI reservoirs, runs a wave firefly boiling filter, traces a final visibility ray to prevent disocclusion leaks, and packs NRD diffuse irradiance. |
| 96 | [`RTXDI/PT/ReSTIRPTInitial.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/PT/ReSTIRPTInitial.slang) | `taskMain`, `meshMain`, `fragMain` | Traces multi-bounce paths from the G-buffer, identifies reconnection vertices, and streams path samples into an `RTXDI_PTReservoir`. |
| 97 | [`RTXDI/PT/ReSTIRPTTemporal.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/PT/ReSTIRPTTemporal.slang) | `taskMain`, `meshMain`, `fragMain` | Performs ReSTIR PT Temporal Resampling with **Hybrid Shift Mapping** (random number replay + reconnection Jacobians + forward/inverse bias correction). |
| 98 | [`RTXDI/PT/ReSTIRPTFinalShading.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/RTXDI/PT/ReSTIRPTFinalShading.slang) | `taskMain`, `meshMain`, `fragMain` | Unpacks the final resampled path reservoir, adds bounce-1 direct lighting, and encodes YCoCg for NRD REBLUR. |

---

## 8. Upscaling, Post-Processing, Editor & 2D Overlay Shaders (15 Files)

| # | Shader File | Entry Points / Role | Plain-English Purpose |
| :--- | :--- | :--- | :--- |
| 99 | [`TAA.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/TAA.slang) | `computeMain` | Built-in Temporal Anti-Aliasing with 3x3 nearest-depth velocity dilation, 5-tap Catmull-Rom history sampling, and YCoCg variance AABB clipping. |
| 100 | [`PostProcess.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/PostProcess.slang) | `taskMain`, `meshMain`, `fragMain` | Applies adapted auto-exposure / manual EV compensation and executes the selected tonemapper. |
| 101 | [`Tonemapping.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Tonemapping.slang) | Shared module | Implements 10 tonemapping operators (Hable UC2, ACES Narkowicz/Hill/RTXPT, Khronos PBR Neutral, Reinhard, etc.) and inverse tonemapping. |
| 102 | [`AutoExposure.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/AutoExposure.slang) | `buildMain`, `reduceMain`, `adaptMain` | Builds a 256x256 log-luminance mip pyramid, reduces it to 1x1, and smoothly adapts exposure over time. |
| 103 | [`SelectionMask.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/SelectionMask.slang) | `meshMain`, `fragMain` | Renders selected instances at display resolution with `nonJitteredProj` into an `R8_UNORM` mask. |
| 104 | [`Outline.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/Outline.slang) | `taskMain`, `meshMain`, `fragMain` | Fullscreen pass that draws a crisp 2px orange outline around the `SelectionMask` silhouette. |
| 105 | [`EditorStableDepth.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/EditorStableDepth.slang) | `meshMain`, `fragMain` | Re-renders visible instances at display resolution with `nonJitteredProj` so 3D gizmos, grid depth testing, and mouse picking never jitter under DLSS/TAA. |
| 106 | [`EditorRaycast.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/EditorRaycast.slang) | `computeMain` | GPU TLAS raycast shader for editor tools (e.g., "Snap to Floor"), ignoring transparent/blended surfaces. |
| 107 | [`TextureInspect.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/TextureInspect.slang) | `compMain` | Powers the Render Graph Texture Inspector: visualizes integer IDs, 64-bit visibility pairs, Reverse-Z depth, or float channels with exposure/channel masks and reads back the cursor texel. |
| 108 | [`present.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/present.slang) | `vertMain`, `fragMain` | Draws the final viewport texture onto the swapchain when running standalone without ImGui. |
| 109 | [`QuadMesh.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/QuadMesh.slang) | `taskMain`, `meshMain`, `fragMain` | Mesh-shader batch renderer for 2D/3D textured quads (`Renderer2D`). |
| 110 | [`CircleMesh.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/CircleMesh.slang) | `taskMain`, `meshMain`, `fragMain` | Mesh-shader batch renderer for procedural anti-aliased SDF circles and rings (`Renderer2D`). |
| 111 | [`TextMesh.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/TextMesh.slang) | `taskMain`, `meshMain`, `fragMain` | Mesh-shader batch renderer for MSDF text glyphs (`median3(r, g, b)`) with optional glyph outlines. |
| 112 | [`LineMesh.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/LineMesh.slang) | `taskMain`, `meshMain`, `fragMain` | Mesh shader (`[outputtopology("line")]`) emitting hardware line segments for debug wireframes, colliders, and bounds. |
| 113 | [`GridMesh.slang`](file:///E:/dev/noxiouse/NoxEditor/assets/shaders/GridMesh.slang) | `taskMain`, `meshMain`, `fragMain` | Godot-inspired procedural 3D editor grid mesh shader with colored origin axes, multi-decade subdivision fading, and horizon angle fade. |
