# Hybrid renderer, sky and VFX plan -- October 2026

> Future.md items 4 (hybrid renderer close to the path tracer, fallbacks, DLSS edge jitter, robust transparency /
> transmission), 6 (procedural sky) and 13 (VFX, particles, clouds). Companion to `RayTracing_Architecture_Plan_2026.md`
> (lighting techniques) and `Engine_Architecture_Plan_2026.md` (frame / memory / render graph). Same contract: the user
> builds and tests, each phase ends with a user test, nothing here is built yet unless it says so.

## 1. Where the hybrid renderer stands (checked in code, 2026-10-18)

| Part | Raster (hybrid) path today | Path tracer |
|---|---|---|
| Primary visibility | visibility buffer, meshlets, GPU culling, cluster LOD | ray traced |
| Direct light | analytic lights, RT shadow rays + NRD SIGMA, ReSTIR DI (RTXDI) for many lights | NEE / ReSTIR DI |
| Indirect diffuse | DDGI probes or ReSTIR GI, IBL fallback | multi-bounce |
| Reflections | RT reflections + NRD REBLUR/RELAX, IBL fallback | multi-bounce |
| AO | SSAO or RTAO | implicit |
| Transparency | **forward pass, unshadowed lights, IBL/DDGI ambient, plain alpha blend** (`Material_TransparentLit_Mesh.slang` header says it) | full BSDF |
| Transmission / refraction | **only lowers alpha** (`diffuseAlpha = baseColor.a * (1 - transmission)`), no refraction, no absorption | RTXPT BSDF: refraction, IOR, volume |
| Denoise / upscale | NRD per signal, DLSS / DLAA, DLSS-RR optional | DLSS-RR or NRD |

So the gap to the path tracer is mostly: transparent and transmissive surfaces, multi-bounce specular (glossy
inter-reflections), and the noise / lag of the single-bounce GI. The baseline the user asked for is "RT support
required, my modern rendering features required, no shadow maps", which matches the RT plan's baseline already.

## 2. The quality ladder (what "fallback" means here)

Every expensive feature gets one cheaper rung, all of them still ray traced (no shadow maps, no screen-space-only
paths as the *only* answer):

| Feature | High (target, near path tracer) | Fallback (lower-end RT GPUs) |
|---|---|---|
| Direct light | ReSTIR DI, 1 shadow ray per pixel, NRD SIGMA | RT shadow rays for the N strongest lights only, SIGMA |
| Indirect diffuse | ReSTIR GI (+ DDGI as a world-space cache for secondary hits) | DDGI only |
| Reflections | RT reflections, 1 ray, rough reflections fall back to DDGI/IBL above a roughness cut | RT reflections at half resolution, roughness cut lower |
| AO | RTAO | SSAO |
| Transparency | RT refraction ray for transmissive materials (§4) | raster refraction from the opaque scene color (screen-space) |
| Upscale | DLAA | DLSS Quality / Balanced |

Implementation: one `RenderQuality` preset struct in `Renderer` (today the toggles are individual Settings
checkboxes), applied as a whole and saved with the project; the Settings panel keeps the individual overrides.
Auto-detect picks the fallback preset on GPUs below a VRAM / shader-model threshold.

## 3. DLSS edge jitter (Future.md: "when DLSS is enabled, edge detection jittering")

**Cause (found, see `Status_Audit_2026-10.md`):** `Renderer::addEntityDepthBlitPass` (`UpscalePasses.cpp`) copies the
jittered render-resolution `Entity` and `Depth` targets to display resolution with a nearest blit. Everything that reads
`EntityHi` / `DepthHi` (the selection outline's edge detection, picking, the 2D overlay's depth test for the grid, gizmo
lines and collider wireframes) therefore shifts by the camera's sub-pixel jitter every frame. DLSS off = no jitter = no
problem, which is exactly what the user sees.

**Fix (built 2026-10-18, untested): the outline reads a non-jittered selection mask.** `Renderer::addSelectionMaskPass`
(`PostPasses.cpp`) draws only the selected entities' instances again, at display resolution, with `nonJitteredProj`
(`SelectionMask.slang`; the task stage is the visibility pass's own `Material_PBR_MeshTask.slang`, one direct draw per
selected instance, its GPU scene slot read from a small per-frame list). `Outline.slang` finds its edges in that mask
instead of `EntityHi`; with no mask (nothing selected in the draw list) it falls back to the entity IDs. The mask covers
the selection's whole silhouette, hidden parts included (UE also outlines selected objects behind others).

Why not a "de-jittered blit" (shifting the nearest blit by the jitter, the first idea here): at DLAA (1:1) the nearest
jittered sample of a pixel is the same one with or without the shift, so it changes nothing; with upscaling it halves the
error but edges still move. Only geometry rendered without jitter holds still.

**Still open:** picking and the 2D overlay's depth test (grid, gizmo lines, collider wireframes) keep reading the jittered
`EntityHi` / `DepthHi`. A one-pixel jitter in picking is invisible; the grid crawling along geometry edges with DLSS on is
the remaining visible part (fix: a non-jittered depth of the scene for the overlay, or depth testing the grid in the shader
against a stabilized depth).

## 4. Transparency and transmission that hold up

| Phase | What | Notes |
|---|---|---|
| H2 | **Shadowed transparent lighting**: the forward transparent pass traces its own shadow rays (ray query in the fragment shader, the TLAS is bound) for the directional light and the ReSTIR DI reservoir light of the pixel underneath | today every light is unshadowed on glass |
| H3 | **Refraction (high rung)**: transmissive materials (`TransmissionFactor > 0`) are drawn in a separate pass after the opaque lighting; each pixel traces one refraction ray (IOR, thickness / volume absorption from `KHR_materials_volume`, already imported) against the TLAS and shades the hit from the G-buffer-free hit shading the path tracer already has (`PathTracerHit.slang`) | the path tracer's BSDF code is the reference |
| H3b | **Refraction (fallback)**: sample the lit opaque scene color behind the surface with an offset from the normal and IOR (screen-space), rough glass = blurred mip of the scene color | what most raster engines ship |
| H4 | **Order-independent transparency**: weighted blended OIT for many overlapping layers (foliage cards, particles), sorting by object as today otherwise | needed once particles exist (§6) |
| H5 | Denoising for the transparent signals (RT shadows on glass, refraction) through the existing NRD front end | |

## 5. Procedural sky (Future.md item 6: "hazemut or something")

The user means one of the analytic / physically based sky models. Options:

| Model | What it is | For us |
|---|---|---|
| Hosek-Wilkie (2012) | fitted analytic clear-sky radiance, sun + sky, cheap closed form | the likely meant "hazemut"; easy, but no ground-level haze, no sunset color beyond the fit, no space view |
| Preetham (1999) | older analytic model | superseded by Hosek-Wilkie |
| **Hillaire 2020 ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique", UE's SkyAtmosphere)** | physically based: transmittance LUT, multi-scattering LUT, sky-view LUT, aerial-perspective froxels, all from Rayleigh + Mie + ozone parameters | **recommended**: matches UE, gives sunsets, haze, aerial perspective on distant geometry, and works from the ground to space |

**Plan (Hillaire):**

| Phase | What |
|---|---|
| S1 | `SkyAtmosphereComponent` (planet radius, Rayleigh / Mie / ozone coefficients, sun = the `DirectionalLightComponent`), selected instead of the HDR cubemap on the `EnvironmentLightComponent` (mode: HDR / Procedural) |
| S2 | Transmittance LUT (256x64) and multi-scattering LUT (32x32) compute passes, recomputed only when the parameters change |
| S3 | Sky-view LUT (192x108) per frame, `Skybox.slang` samples it instead of the cubemap; sun disc |
| S4 | IBL from the sky: the existing irradiance / prefilter passes run on a cubemap rendered from the sky-view LUT when the sun moves (the RT miss shaders sample the same cubemap, so path tracer and raster agree) |
| S5 | Aerial perspective: a 32x32x32 froxel volume of in-scattering / transmittance applied to the lit opaque scene (this is also the start of the RT plan's Phase 7 volumetrics) |

Exit test per phase: sun at noon / sunset / below the horizon looks right in raster and path tracer alike, and the
path tracer and raster agree on the sky color.

## 6. VFX, particles and clouds (Future.md item 13)

**How it works in real engines (the user's question):** effects are *authored inside the engine*, not imported --
UE's Niagara, Unity's VFX Graph / Shuriken and Godot's GPUParticles are node or module editors that run in the editor.
What comes from outside are the *assets* effects use: flipbook textures (smoke, fire, sparks) rendered in Houdini /
EmberGen / Blender, meshes for mesh particles, and sometimes baked simulations (VDB volumes, vertex animation textures).

**Plan:**

| Phase | What |
|---|---|
| V1 | GPU particle system: emitter component (spawn rate, burst, lifetime, initial velocity / size / color ranges), compute passes for spawn / update (gravity, drag, curl noise) / compaction, rendered as camera-facing quads (flipbook UVs) or mesh particles; sorting per emitter |
| V2 | Particles in the hybrid renderer: lit by the same direct light + DDGI as transparents (H2), soft particles from depth, OIT from H4 |
| V3 | Module editor: emitters as a stack of modules (UE Niagara-style), built on the existing node-graph core (`NodeGraph`, the canvas backend) instead of a new UI |
| V4 | Volumetric clouds: Hillaire-style raymarched cloud layer (weather map + 3D noise), lit by the sky atmosphere of §5 (needs S1-S4), temporal reprojection |
| V5 | Volumes from files (OpenVDB) for explosions / smoke, ray marched in the path tracer and the raster volumetric pass |

## 7. Order

H1 outline (built) -> H1 overlay depth -> S1-S4 (sky) -> H2/H3b (transparency) -> preset ladder (§2) -> H3 -> V1/V2 -> S5 + V4 -> V3, V5.
