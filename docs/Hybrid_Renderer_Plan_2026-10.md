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

**Built (2026-10-18, untested):** `Renderer::RenderQuality` Low / Medium / High (+ Custom), Settings > **Render Quality**,
saved in the project file (`RenderQuality: Auto | Low | Medium | High | Custom`; the editor and NoxRuntime apply it when the
project opens). Auto picks by the GPU's memory budget: < 6.5 GB Low, < 10 GB Medium, else High.

| Preset | Shadows | Reflections | AO | Diffuse GI | Direct light | DLSS |
|---|---|---|---|---|---|---|
| Low (fallback) | RT, sun only (the loop shadows light 0) + SIGMA | IBL | SSAO | DDGI | analytic loop | Performance |
| Medium | RT + SIGMA | RT + REBLUR | SSAO | DDGI | ReSTIR DI (every light shadowed) + REBLUR | Balanced |
| High (near the path tracer) | RT + SIGMA | RT + REBLUR | RTAO | ReSTIR GI + REBLUR | ReSTIR DI + REBLUR | Quality |

Glass refraction is on in all three. Changing one setting afterwards shows Custom for the session. Not yet: half-resolution
reflections and a roughness cut as their own rungs.

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

**Built (2026-10-18, untested): the rest.** With a jittered camera the editor no longer blits the jittered `Entity` /
`Depth` to display resolution: `Renderer::addStableEntityDepthPass` (`UpscalePasses.cpp`, `EditorStableDepth.slang`) draws the
camera's visible instances again at display resolution with `nonJitteredProj` (both culling phases; depth-writing buckets,
then the transparent ones for their entity IDs, depth tested), so the 2D overlay's depth test (grid, gizmo lines, collider
wireframes), picking and the measure tool's depth hold still. Settings > DLSS > **Stable Overlays** (on by default; a depth-only
pass of the scene, editor only).

## 4. Transparency and transmission that hold up

| Phase | What | Notes |
|---|---|---|
| H2 | **Shadowed transparent lighting**: the forward transparent pass traces its own shadow rays (ray query in the fragment shader, the TLAS is bound) for the directional light and the ReSTIR DI reservoir light of the pixel underneath | today every light is unshadowed on glass |
| H3 | **Refraction (high rung)**: transmissive materials (`TransmissionFactor > 0`) are drawn in a separate pass after the opaque lighting; each pixel traces one refraction ray (IOR, thickness / volume absorption from `KHR_materials_volume`, already imported) against the TLAS and shades the hit from the G-buffer-free hit shading the path tracer already has (`PathTracerHit.slang`) | the path tracer's BSDF code is the reference |
| H3b | **Refraction (fallback)**: sample the lit opaque scene color behind the surface with an offset from the normal and IOR (screen-space), rough glass = blurred mip of the scene color | what most raster engines ship |
| H4 | **Order-independent transparency**: weighted blended OIT for many overlapping layers (foliage cards, particles), sorting by object as today otherwise | needed once particles exist (§6) |
| H5 | Denoising for the transparent signals (RT shadows on glass, refraction) through the existing NRD front end | |

**Built (2026-10-18, untested):**

- **H2:** `Material_TransparentLit_Mesh.slang` traces one hard shadow ray (ray query, `forwardShadowed`) for the four strongest
  lights of each pixel (by their unshadowed contribution; the rest stay unshadowed) while RT shadows are on. The ray forces
  non-opaque so every candidate is decided in the shader: blended and transmissive occluders let the light through (glass
  neither throws a black shadow nor shadows itself), alpha-masked ones are cut out. The forward passes declare the TLAS read.
  The editor's extra viewports use the same shader for all surfaces, so they get shadows too.
- **H3b:** with glass in view (a lit transparent draw whose material has transmission), `addForward3DPass` splits: Forward 3D
  (unlit, sky), then the HDR image is copied with a mip chain of 2x2 averages (`ColorPyramid.slang`, "Refraction Source"), then
  **Forward Transparent** draws the transparent buckets. Glass bends the view ray by its IOR through the volume's thickness
  (`KHR_materials_volume` thickness x model scale; 0 = thin-walled, no bending), offsets the lookup by the exit point's screen
  offset, reads the mip `log2(size) * roughness * clamp(2 ior - 2)` (the Khronos viewer's rule), tints by the base color and
  takes out the Fresnel reflectance; it covers what is behind it by its alpha alone. Settings > **Glass Refraction**. Without
  glass in view the frame is unchanged. Not yet: volume absorption (attenuation color / distance are not imported), OIT (H4),
  denoising of the forward shadows (H5).
- **H3 (2026-10-18, untested):** in the Forward Transparent pass, smooth glass (roughness up to 0.08, fading into H3b by 0.2)
  traces what is behind it (`tracedRefraction` in `Material_TransparentLit_Mesh.slang`, flag `MESHLET_RT_REFRACTION`). Thin-walled
  glass (thickness 0) passes the view ray on unbent; glass with a volume thickness bends it in at the surface and out at the
  mesh's own far side, found with a ray that only accepts this instance (TLAS custom index = the GPU scene instance slot =
  `drawID`), with total internal reflection mirroring it; an open mesh falls back to the thickness slab. The ray then goes on
  through the scene past blended / transmissive surfaces (alpha-masked ones cut out) and the hit is shaded by `shadeRayHit`,
  the RT reflections' hit shading moved into `RayHitShading.slang` (base color, every light with a shadow ray, DDGI or
  irradiance, emission); a miss reads the environment. So glass shows what is off screen or hidden behind other objects, and
  solid glass bends like a lens. On in the High preset, Settings > **Ray Traced Glass** (under Glass Refraction).

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

### 5b. Built (2026-10-18, untested): S1, S2, S4 and the sun disc

- **Use:** Environment Light component -> **Source: Procedural Sky** is all it takes: without a Directional Light the sky's
  own sun (~54 degrees up, intensity 10 -- a daylight sky at exposure 1) is drawn and also submitted as the scene's
  directional light (direct light, shadows); **Add Sun Light** in the panel turns it into a real "Sun" entity. With a
  Directional Light, the first one is the sun (its rotation, color and intensity; the panel warns when it is weak -- a
  sun of intensity 1 gives a dark navy sky at exposure 1). The panel shows that light and its
  **Sun Elevation / Sun Azimuth** (dragging them rotates the light; not for a light with a parent), and warns when the sun
  is low (below 8 degrees: a sunset sky, dark above and orange at the horizon -- a new Directional Light with rotation 0
  shines horizontally, which is exactly that). Atmosphere: Air (Rayleigh), Haze (Mie), Haze Forward (Mie anisotropy), Ozone
  (scales of Earth's values, 1 = Earth), Ground Albedo, Altitude (km). Radiance Scale scales the sky's light as for an HDR
  map (not the sun's disc). Saved in the scene file, undoable.
- **`SkyAtmosphere.slang`**, after the paper's reference implementation: transmittance LUT (256 x 64, Bruneton's
  parametrization), multiple-scattering LUT (32 x 32, the paper's Psi_ms), then the sky raymarched into every mip of the
  environment cubemap (32 samples per ray on quadratically growing segments, planet shadow, ground bounce below the
  horizon). Earth constants: radii 6360 / 6460 km, Rayleigh (5.802, 13.558, 33.1)e-3 / km over 8 km, Mie 3.996e-3
  scattering + 0.444e-3 absorption over 1.2 km, ozone (0.650, 1.881, 0.085)e-3 in a tent at 25 +- 15 km.
  `SkyAtmosphereCommon.slang` holds what the skybox shares (planet, transmittance lookup).
- **One environment for everything:** the cubemap goes through the same irradiance / prefilter shaders as an HDR map, so
  skybox, raster IBL, reflections, DDGI and the path tracer's misses all see the same sky.
- **Sun disc** (`Skybox.slang`): the directional light's direction and angular diameter, dimmed and reddened by the
  transmittance in front of it, limb-darkened, antialiased by the skybox's 16 directions per pixel, hidden behind the
  planet. Its radiance is the illuminance over the disc's solid angle, capped at 64x the illuminance (the real ~15000x only
  saturates harder, and those pixels would skew auto exposure). Not in the IBL (the directional light already lights the
  scene; it would count twice) nor in the path tracer's view of the sky.
- **Updates inside the frame** (`Renderer::addProceduralSkyPass`, first pass of the frame graph): while the sky is the
  environment its textures stay -- environment 256, irradiance 32, pre-filtered 128 per face (an HDR map gets 512 / 64 /
  512; the sky has no sharp detail) -- and a frame whose settings changed rewrites them with compute passes: the LUTs only
  when the atmosphere changed, then the cubemap's mips, then irradiance and prefilter. Estimated ~1-2 ms in that frame, no
  CPU wait; full barriers before (frames in flight still read the textures) and after (the uniforms' IBL slots are not
  tracked by the graph). A dragged sun updates the sky every frame. The path tracer's environment importance map and
  accumulation restart after an update. Switching to the sky creates its textures and pipelines once (a short wait).
  **2026-10-18 fix:** the first version rebuilt an HDR-sized IBL set (512 prefilter at 1024 samples, ~550 M texture reads)
  with a GPU wait per step and pipelines created each time, debounced 150 ms after the last change: the ~1 s freeze after
  letting go of the sun. The IBL filter pipelines are now created once, for HDR maps too.
- **Aerial perspective (S5, built 2026-10-18, untested):** the paper's camera volume -- 32 x 32 froxels x 32 depth
  slices (a 2D array texture: rgb scattered light, a mean transmittance), slice z ending at 32 km * ((z + 1) / 32)^2,
  marched every frame from the camera with 2 samples per slice (`SkyAtmosphere.slang aerialPerspectiveMain`, the same
  medium, phase functions and LUTs as the sky). `DeferredLighting.slang` applies `color * T + scattered` at the end of the
  lit result, looked up by the pixel's distance. The pass is declared right before deferred lighting and imported into the
  graph, so it runs only when deferred lighting does (not in path-traced views) and its read is ordered by the graph.
  **Aerial Perspective** on the component multiplies distances (UE's distance scale): 1 = real air -- a grey object
  takes a light blue haze at 10 km (T 0.81) and heads for the horizon color at 30 km (T 0.55), barely anything within
  a few hundred meters -- so small scenes use 10-100 for a visible effect, 0 turns it off. Not applied yet to forward
  passes (unlit and transparent surfaces).
- **Volumetric clouds (built 2026-10-18, untested; Future.md item 13's clouds, plan V4)**, Schneider 2015 / Hillaire
  2016: a cumulus layer (Cloud Bottom 1.5 km, Thickness 2 km) raymarched into the environment cubemap's top mip with the
  sky, so skybox, IBL, reflections, DDGI and path tracer misses all see the same clouds. Tiling noise made once on the
  GPU when the sky is created (`CloudNoise.slang`): 64^3 Perlin-Worley + Worley FBMs for the shapes, 32^3 Worley FBMs
  eroding the edges (wispy below, billowy above), a 128^2 weather map spreading the coverage; 3D textures stored as 2D
  arrays and filtered between layers by hand. Per texel: 64 steps through the layer (jittered), six steps towards the
  sun per cloud sample, Hillaire's three multiple-scattering octaves, a two-lobe phase (silver lining towards the sun),
  ambient light from the sky's multiple-scattering LUT, the air in front of the clouds (distant clouds fade into the
  haze). The lower mips are now averaged from the top one (`downsampleCubeMain`) instead of raymarched (clouds would
  alias). Controls: Coverage (0.35), Cloud Density, Bottom, Thickness, Size (8 km noise tile), Offset (move it over time
  for wind; each change re-renders the sky). On by default. Previewed with a numpy port of the noise and the march
  through the engine's tonemap: scattered white cumulus at noon, grey undersides and a warm glow round a low sun.
  Cost: only when the sky updates; estimated a few ms more than the sky alone. Not yet: cloud shadows on the scene,
  per-frame animated clouds (wind needs a time-sliced update), clouds seen from above at high altitude.
- **Exponential height fog (built 2026-10-18, untested)**, UE's Exponential Height Fog, as a **Height Fog** section of
  the Environment Light (either source): density(y) = Fog Density * exp(-Height Falloff * (y - Fog Height)), its optical
  depth along the view ray in closed form (`HeightFog.slang`, one exp per pixel), Start Distance, Max Opacity; the fog is
  lit by the environment (its irradiance cubemap above the view direction, so it takes the sky's or the HDR map's
  color) tinted by Fog Color, or Fog Color alone; plus a glow towards the sun (Sun Glow x the sun's illuminance,
  pow(cos, Sun Glow Size)). Applied after the aerial perspective in deferred lighting and on the skybox at "infinity"
  (thick at the horizon, thin overhead). Defaults: 0.5 / km, falloff 5 / km (halves every ~140 m): ~10 % at the zenith,
  a soft band at the horizon, ~10 % on a surface 200 m away. Off by default; its parameters ride in the frame uniforms
  (read through the pointer: natural layout, the same as C++).
- **Applied** only from `Scene::SetRenderer` (main thread, every frame); the Submit Lights system, which may run on a
  worker, only reads the scale.
- **2026-10-18 fix 2, the wrong colors** ("light yellow above, brown-green below, a black cut"): push constants compile
  with **std430** rules in the engine (`SlangCompiler` sets GLSLForceScalarLayout, which does not reach push constants),
  so a `vec3` member starts on 16 bytes; the C++ structs were packed (scalar). From the first `vec3` on, the sky pass read
  every field shifted: the sun's color as its direction, the ground albedo as the sun's color, and the Rayleigh / Mie /
  ozone scales and altitude past the end of what C++ pushed (zero or garbage: no blue, the black regions); the skybox read
  the disc's angular radius from the altitude (cos = 0: a "disc" over half the sky, with a hard edge). Both structs now
  put each `vec3` on a 16-byte boundary followed by a float, so std430 and C++ agree, with `static_assert`s on the C++
  offsets; checked against the SPIR-V of the engine's own compile path (whole module, same options). Rule for new push
  constants: no `vec3` unless 16-byte aligned and followed by a 4-byte member.
- **Checked here:** every entry point compiles with slangc; push-constant offsets of the engine's compile path match C++;
  a float32 port of the passes renders the expected panoramas through the engine's tonemap (UC2, exposure 1): sun 10 at
  54 degrees a blue sky with a bright horizon and a glow round the sun, at 20 degrees deeper blue, at 4 degrees an orange
  band at the horizon; a numpy port of the passes gives a blue zenith at noon (blue / red ~4.9), a red-orange horizon towards the sun
  at 2 degrees elevation and a dark sky after sunset; 32 quadratic steps stay within ~6 % of 512 steps; the sun controls'
  rotation round-trips through the Euler angles the transform stores (error < 1e-5).
- **Brightness:** the sky is physically relative to the sun: at noon the zenith is ~0.03 x the sun's illuminance in blue
  (~0.013 in luminance), a sunlit white wall ~20x brighter, as outside. Auto exposure (camera) or Radiance Scale bring the
  sky up when it reads too dark next to an exposure tuned for a bright HDR map.
- **Still open:** time-slicing the update over a few frames (UE's real-time sky capture) if 1-2 ms per changing frame
  shows; aerial perspective on forward-rendered (unlit / transparent) surfaces; night (no moon / stars, the sky just goes dark); the viewer's altitude comes from the
  component, not the camera; the sun disc in path-traced views. Found while here: the component's **Rotation** is drawn
  in the UI but no shader applies it (HDR maps too); the fix is to rotate the direction in the resample pass.

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
