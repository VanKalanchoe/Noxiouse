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
| Low (fallback) | RT + SIGMA (sun), a ray per light (others) | IBL | SSAO | screen probes, 24 px (§2c) | loop, every light shadowed | Performance |
| Medium | RT + SIGMA | RT + REBLUR | SSAO | screen probes, 16 px | loop, every light shadowed | Balanced |
| High (near the path tracer) | RT + SIGMA | RT + REBLUR | RTAO | screen probes, 16 px | loop, every light shadowed | Quality |

(Until 2026-10-19 the GI column was DDGI / DDGI / ReSTIR GI + REBLUR (§2c), and Medium / High used ReSTIR DI + REBLUR for
direct light -- the user found ReSTIR DI broken; it stays selectable. The loop already traces a shadow ray per light in range
per pixel, not only for light 0 as older notes said.) Glass refraction is on in all three. Changing one setting afterwards shows Custom for the session. Not yet: half-resolution
reflections and a roughness cut as their own rungs.

### 2b. DDGI fixed and made camera-following (2026-10-19, untested)

The user reported GI as "broken or slow". DDGI (the Low / Medium rungs, and what Auto picks on an 8 GB card) was broken in any
project not in meters: the probe grid was a fixed box at the origin in raw world units (22 x 10 x 14 probes 1.8 units apart --
40 cm wide in a centimeter project like Facerun's Bistro), and the distance moments clamped ray lengths at 100 world units (1 m),
so the visibility test rejected most probes and surfaces went dark. Now:

- **The volume follows the camera** (Settings > DDGI > Follow Camera, on): first 20 x 10 x 20 probes 2 x 1.5 x 2 m apart (about
  38 x 13.5 x 38 m around the camera; now cascades, below), spacing in meters scaled by the project's world unit, snapped to whole cells and scrolled with
  **toroidal addressing** (a probe's atlas slot is its world grid coordinate modulo the counts: `DDGICommon.slang`
  `ddgi_storage_index` / `ddgi_local_coord`), so probes keep their history while the camera moves; the probes that come in on a
  side start over (`ddgi_probe_entered`, from the per-frame shift in the uniforms). Off: the old fixed grid, now in world units.
- **The volume's faces blend into the environment's irradiance** over one cell, instead of the edge probes stretching over
  everything outside.
- **Unit-aware limits**: the distance moments clamp at 1.5 cells and are stored in meters (the atlas is 16-bit float: squared
  centimeters would overflow it), the Chebyshev variance floor is (2 cm)^2, the debug spheres' radius is in meters.
- **Probe classification**: rays that hit the back of single-sided geometry are marked (negative distance); a probe that sees more
  than a quarter back faces is inside something and is switched off (irradiance alpha 0, skipped when shading; dark red in the
  probe-sphere debug view), so walls no longer have dark probes behind them bleeding in.
- **Update budget** (Probe Updates: every probe / half / a quarter a frame, in turn; probes that scrolled in always update):
  half by default, a quarter on Low -- a quarter of the rays of tracing every probe each frame on Low.
- **Energy**: the probes hold irradiance / pi (as the lighting reads them); the second bounce inside DDGI and the ambient term of
  ray-hit shading (RT reflections, ray traced glass) divided by pi once more, which made multi-bounce light about three times too
  dark. Fixed.

- **Cascades (2026-10-19, after the user's test: "across the street in Bistro is already not lit by GI"):** the single
  camera-following volume (~38 x 13.5 x 38 m) was too small. Now Cascades (default 3, 2 on Low) nested volumes of 20 x 12 x 20
  probes, each Cascade Scale (3x) wider than the one inside: 2 m cells over ~38 x 22 x 38 m, 6 m over ~114 x 66 x 114 m, 18 m over
  ~342 x 198 x 342 m. Probes are stored cascade after cascade in the same atlases (`UniformBufferObject::ddgiCascades`, one
  `DDGICascade` per cascade: origin, spacing, toroidal scroll, shift). Shading uses the finest cascade around the point and
  hands over to the next coarser one over its last cell, then to the environment. Each further cascade updates half as often
  as the one inside it (so three cascades cost about 1.75x the probe updates of one, not 3x); debug spheres scale with their
  cascade.
- **Cheaper probe rays in many-light scenes:** a probe ray's hit used to trace a shadow ray to every light (Bistro has many).
  Up to 4 lights still get one each; with more, every light's unshadowed contribution is summed (no rays) and two lights are
  picked in proportion to it (weighted reservoir sampling) for the only two shadow rays -- unbiased, the noise averaged by the
  probes' temporal blend.

- **Smooth walls, no flicker (2026-10-19, after the user's test: "wiggly, not smooth across the board compared to the path
  tracer; some flickering on window edges and curtains"):** the shading now weights probes the way RTXGI does --
  - a flat **geometric normal** (rebuilt from the depth buffer in DeferredLighting, `ddgi_geometric_normal`: nearer-side
    neighbours, so silhouettes do not bend it) for the sample offset and the backface term; the normal-mapped normal only
    picks the irradiance direction. Weights that followed the normal map made the probes' light wiggle with every bump;
  - a **smooth backface term** ((cos + 1) / 2)^2 + 0.2 instead of dropping probes behind the tangent plane at once;
  - the sample point offset mostly **towards the viewer** (0.8 view + 0.2 normal, Normal Bias x 1.5 cells);
  - RTXGI's **gentler Chebyshev** (cubed, at least 0.05) instead of a remapped fourth power, and its **weight crushing**
    below 0.2.
  - **Classification blended over time** (the probe's alpha follows the same hysteresis as its light): probes near the
    back-face threshold (thin curtains, window frames) fade instead of flipping on and off -- the flicker.
  - **The sun always gets its own shadow ray** in probe-ray shading; only local lights are picked by reservoir sampling
    (a picked sun flickered).

Not done: probe relocation (moving probes out of geometry instead of switching them off) and a faster ReSTIR GI (half
resolution / checkerboard) for the High rung.

### 2c. Screen-probe GI: the default diffuse GI (2026-10-19, untested)

After the DDGI fixes the user still saw DDGI as broken ("make a modern solution, rewrite if needed, close to Lumen / Godot").
A probe grid in world space is the wrong tool for the final gather: its cells cut through walls, so a wall's light is an
interpolation of probes on both sides of it (leaks, wiggles, classification flicker), whatever the weighting. Lumen's answer is
to gather from probes **on the visible surfaces** and keep a world-space cache only for what those probes' rays hit. That is
now **Diffuse GI mode 3, "Screen Probes"**, the GI of every quality preset (DDGI and ReSTIR GI stay selectable):

| Pass (`ScreenProbe*.slang`, `Renderer::addScreenProbeGIPasses`, after DDGI) | Output |
|---|---|
| **Place**: one probe per tile (16 x 16 px at render resolution; 24 on Low, whose DLSS Performance renders at half size) on a pixel of the tile that is not sky, at an offset that moves every frame (R2 sequence), with the surface's flat normal from the depth buffer | probe positions (RGBA32F) and normals |
| **Trace**: 8 x 8 rays per probe, uniform over its hemisphere, stratified and rotated per frame. Hits: lights (sun always with its shadow ray, every light when there are up to 4, else two picked by weighted reservoir sampling), emission, and **the DDGI cascades as the world radiance cache** (second bounce and beyond). Misses: the sky. Glass and clear blended surfaces let rays through (light through windows), alpha masks cut out, the back of single-sided geometry is black | ray atlas (RGBA16F, radiance + hit distance) |
| **Irradiance**: per probe a world-space octahedral irradiance map (6 x 6 + border, like DDGI's), the clamped cosine of each direction over the rays: E/pi = 2/N sum L max(0, d.w) | irradiance atlas |
| **Filter** (x2 by default): each texel averaged with the same direction of the 3 x 3 neighbouring probes that lie on the same surface (plane distance relative to the view distance, normal agreement) | irradiance atlas (ping-pong) |
| **Integrate**: per pixel, the 4 probes around it (bilinear over the jittered grid) weighted by plane distance and normal, read towards the pixel's normal-mapped normal; none on its surface: the 4 x 4 around; still none: DDGI / environment. Then **temporal accumulation**: last frame's result reprojected with the motion vectors, accepted per tap against last frame's depth and normal, blended by 1 / frames (up to 24); a jump of 8x shortens the history | per-pixel irradiance / pi (history, ping-pong) |

The deferred lighting reads it like DDGI (ambient diffuse = it x diffuse color; the sky's specular is occluded by it). The
probes start on the surface they light, so no light comes through walls; every pixel reads the same few probes as its
neighbours on the same surface, so walls are smooth; the jittered placement and the temporal accumulation average over the
tile, so small objects and corners converge to their own light. DDGI keeps running underneath (a quarter of its probes a frame
in this mode) as the cache for the rays' hits, its artifacts averaged away by 64 rays x the filters x the history.

Settings > Diffuse Global Illumination > Screen Probes: Screen Probe Spacing (8 / 12 / 16 / 24 / 32 px), Spatial Filter Passes
(0-3), Temporal Frames (1-64), Reset Screen Probe History; the DDGI settings below it are the cache's. Debug view 16 (Indirect
Diffuse GI Only) shows the result. Cost at 1080p render resolution, 16 px: 8160 probes, ~520k rays + their shadow rays a frame.

Not done: Lumen's importance sampling of the probe rays (by last frame's light and the BRDF), its adaptive extra probes where
the interpolation fails (here the 4 x 4 search and the DDGI fallback), and a screen-space trace before the ray trace.

**Everything else lit by the same GI (2026-10-19, after the user's test: "decals have a light white background compared to the
path tracer; looking through windows the inside loses its color, with GI off it comes back"):**
- **The cause of the pale veil was transparency, not GI**: the forward shader (Material_TransparentLit_Mesh.slang) scaled a blended
  surface's diffuse light by its alpha but added its reflection (sky, sun highlights) whole, so a decal's clear parts and
  low-alpha window glass laid the sky's reflection over what is behind them. Alpha is coverage now, for all of it, as the path
  tracer takes it (it keeps a blended surface with probability alpha). With GI on the inside of the Bistro is darker (the
  sky's light no longer reaches it unoccluded), so the constant veil over the windows hid its colors -- hence "GI off brings
  the color back".
- **One GI for every shaded point** (`SceneGI.slang`): the screen-probe pass also writes each pixel's distance from the camera;
  a point shaded elsewhere -- a decal or glass in the forward pass, an RT reflection or refraction hit -- that is the surface the
  camera sees at its pixel takes that pixel's screen-probe GI (a decal gets exactly its wall's light, a reflected or refracted
  object on screen the light it has when seen directly); other points take DDGI, then the environment. RT reflections now run
  after the GI for that. Forward surfaces also get the deferred lighting's specular occlusion.
- **Thin glass is no longer traced** on the High rung: an unbent view ray only finds the surface right behind the pane, which
  the screen-space refraction source already holds fully lit (GI, reflections, shadows); the trace shaded it more simply. Glass
  with a volume (thickness) is still traced.
- **Rough reflections from the GI** (screen probes): the environment's reflection hands over to the GI times the BRDF's
  specular albedo from roughness 0.35 to 0.8 -- a rough metal indoors reflects the room, not the sky.

Ray tracing resolution: every RT pass runs at the render resolution, which the DLSS mode sets (DLAA 100 %, Quality 67 %,
Balanced 58 %, Performance 50 %, Ultra Performance 33 % per axis; Settings shows "Rendering W x H -> W x H"). Performance traces a
quarter of DLAA's rays for every effect. Per effect: Screen Probe Spacing (24 px: about half the GI rays of 16 px), DDGI Probe
Updates, RT reflections off on Low.

**Ray Tracing Resolution, independent of DLSS (2026-10-19, untested;** the user: "DLSS lowers the textures too, I need the ray
traced effects alone lower"): Settings > Enable Hybrid Ray Tracing > Ray Tracing Resolution, Full / Half. Half: RT reflections,
the RT sun shadow mask and RTAO render into a half-width, half-height target, each texel tracing one pixel of its 2 x 2 block
(`rt_trace_pixel`, shaderIO.h), a different one every frame (`UniformBufferObject::rtSubsample` / `rtSubsampleOffset`, the four
over four frames). `RTUpsample.slang` brings each back to the render resolution before its denoiser: every pixel takes the traced
samples of the 3 x 3 blocks around that lie on its surface (depth within a few percent, normal agreeing), the nearest weighing
most -- blended for reflections and AO (the hit distance from the best match), the best match alone for the shadow mask (its
penumbra distance does not average). NRD's temporal accumulation then sees every pixel over the frames. A quarter of those
passes' rays; the GI keeps its own setting, the deferred lighting's shadow rays for local lights stay per pixel.

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
| H4 | **Order-independent transparency**: weighted blended OIT for many overlapping layers (foliage cards, particles), sorting by object as today otherwise | **built 2026-10-20, untested:** `Renderer::setOITEnabled` (Settings: Order-Independent Transparency, off by default); transparent meshes and alpha-blended particles into an accumulation (RGBA16F) and a revealage (R16F) target, `OITComposite.slang` resolves them over the HDR scene; additive particles add after it; glass still refracts the scene copy |
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
  mesh's own far side, found with a ray that accepts this instance or a solid surface inside the volume (TLAS custom index =
  the GPU scene instance slot = `drawID`; other glass is passed), with total internal reflection mirroring it; an open mesh
  falls back to the thickness slab. A solid surface inside (a bulb's filament) is shaded and seen through the front surface with
  the volume's absorption over the distance (2026-10-31: it used to be passed, so an emissive filament stayed dark). The ray then goes on
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
- **Exposure (2026-10-31, Unreal's model):** auto exposure meters the frame's log-average luminance L as a reflected-light meter
  (EV100 = log2(L x 100 / 12.5)) and shows it at 1 / (1.2 x 2^EV100): L maps to 0.104 (`AutoExposureKey`). RTXPT's key, 0.042, put
  the average 2.3 stops lower: through the Hable UC2 tonemapper an average surface came out at sRGB ~49/255, daylight looked dim
  and a frame of sky alone (the sky's ~0.13 at noon is its average then) dark. Without auto exposure the camera's **Manual EV100**
  applies (default 2, the default sun's daylight; the old fixed exposure 1.0 was EV100 -0.3, blown out); the editor's slider is a
  compensation in stops on top. Asset previews and unlit views keep their own exposure.
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

### 6b. Built (2026-10-18, untested): V1 and V2 (without OIT)

- **Particle Emitter** component (Add Component > Particle Emitter; `Components.h`): emission (rate, burst per loop, duration,
  looping, max particles), shape (point, sphere, box, cone with angle), motion (lifetime range, velocity + random per axis in the
  emitter's space, gravity in g, drag, spin range, local or world space), look (size and color + opacity over life, size random,
  intensity for HDR glow, additive or alpha blend, lit, soft with a distance, a flipbook texture of columns x rows at a frame rate
  or over each life). Lengths in meters, scaled by the project's world unit. Saved in the scene; Restart button and live count in
  the panel.
- **Simulation on the CPU** (`Scene/ParticleSystem.{h,cpp}`, the "Particles" update system after transform propagation): spawn,
  integrate, retire with swap-remove, xorshift random per emitter. It runs whenever the scene steps -- in Edit too, so effects
  preview while placed -- and Play / Simulate restart every emitter. Chosen over a GPU simulation for now: a few thousand
  particles per emitter cost little, the state is plain data scripts drive, and nothing reads back from the GPU; the GPU
  simulation (and GPU sorting) is the next step when effects need hundreds of thousands.
- **Rendering** (`Particles.slang`, `Renderer::SubmitParticles` / `addParticlePass`): the "Submit Particles" system hands each
  emitter's particles (position, size, color, rotation, flipbook frame) to the renderer, which sorts alpha-blended ones back to
  front, uploads all into a host-visible buffer per frame in flight, and draws them after the forward pass over the HDR scene
  (before DLSS, like transparent meshes): a mesh shader makes 32 camera-facing quads per workgroup, emitters far to near,
  additive or premultiplied-alpha blending per emitter. No depth attachment: each fragment reads the scene depth, hides behind
  geometry and, soft, fades over the last Soft Distance. **Lit** particles are shaded once per particle in the mesh stage: DDGI
  (or the irradiance cubemap) for the environment, the first 8 lights as a diffuse puff, the first directional light behind a
  shadow ray (ray query) while RT shadows are on.
- **Scripts:** `ParticleEmitterComponent.Play(restart)`, `Stop()`, `Emit(count)`, `IsEmitting`, `ParticleCount`.

Not yet: (OIT built 2026-10-20, H4: with it on, alpha-blended particles and transparent meshes blend in any order), GPU simulation,
collisions, mesh particles, color / size curves beyond start-end, the module editor (V3), particles in the editor's extra
viewports and in the path tracer, motion vectors for DLSS (fast particles can smear).

## 7. Order

H1 outline (built) -> H1 overlay depth -> S1-S4 (sky) -> H2/H3b (transparency) -> preset ladder (§2) -> H3 -> V1/V2 -> S5 + V4 -> V3, V5.

## 8. Lights (2026-10-19, untested)

After the user's night-time test ("the GI flashes with every light, rainbowy; fix all punctual lights; add rect area lights"):

**Punctual lights fixed** (one model for every shader: `shaderIO.h` `light_distance_attenuation`, `light_spot_attenuation`,
`sample_light_point`):
- **Unit-correct falloff.** The inverse square law ran in world units: in a centimeter project (Facerun) a light was 10 000
  times dimmer than the same light in a meter project -- a default point light (Intensity 5, 3 m away) gave 0.00006. The
  renderer now multiplies point / spot intensities by (units per meter)^2 (`Renderer::SubmitLight`), so the falloff is the one
  in meters everywhere, the shaders unchanged in form. **Lights in centimeter projects are much brighter now: lower intensities
  that were raised to compensate.**
- **Range follows the entity's scale**: a glTF light's range is in the file's meters while its node is scaled into the
  project's units -- it reached a hundredth as far in centimeters.
- **Held at the source radius** (at least 1 cm): exact for a sphere light, no blow-up on a lamp's shade right next to the bulb.
  The range window is Karis's smooth (1 - (d/r)^4)^2 everywhere (one shader had another).
- **The sun keeps its soft shadow.** The RT shadow mask (NRD SIGMA) took light 0, but the light buffer puts point / spot lights
  first (ReSTIR DI's regions): with any local light in the scene the sun got a hard shadow, a lamp the soft one, and SIGMA a zero
  light direction. It now takes the first directional light (`UniformBufferObject::shadowMaskLightIndex`); without one the pass
  is skipped and every light traces its own rays.
- **Every consumer the same**: deferred, transparent, particles, ray hits (reflections, refraction, screen probes, DDGI), both
  path tracers and ReSTIR DI / GI / PT (their six copies of the light sampler replaced by `sample_light_point`). ReSTIR DI / GI's
  shadow-ray offsets were 0.02 world units -- 0.2 mm in centimeters, shadow acne; now 2 cm in any unit.

**Night-time GI flashing** (`LightSelection.slang`): a probe ray's hit traced shadow rays for two lights picked at random among
all of them when there were more than four -- with dozens of colored street lamps every ray saw a different lamp, the colors
flickered. Now the strongest four at the hit (and every directional light) are traced exactly and one more, picked in
proportion to its light from the rest, stands for them: exact where a few lights dominate. GI rays take point / spot lights at
most at 10 cm (a ray grazing a lamp's own fixture is no firefly), and the screen-probe history no longer drops to 4 frames on a
large jump (one bright frame made it flash). Used by the screen probes, DDGI and RT reflection / refraction hits.

**Emissive light samples near a small lamp** (2026-10-31, `ScreenProbeTrace.slang` `screen_probe_emissive_sample`): the probes'
emissive triangle samples used the same 10 cm rule (a lamp nearer counted as 10 cm away). Inside AnisotropyBarnLamp's shade,
3-7 cm from the filament, that dropped 70% of its light (measured on the mesh: 29.5% kept) and the shade's white inside came out
gray in hybrid while raster and the path tracer showed it white. Now a sample credits its triangle with at most a hemisphere
(2 pi sr; `EmissiveLights::Sample::solidAngle`, area x cos / d^2): no triangle subtends more, so the bound only acts closer than
the triangle's own size -- where the 1/d^2 spike was -- and keeps 100% of the filament's light on the shade.

**Rect area lights** (Add Component > Rect Light: color, intensity = the radiance of its surface, width / height along the
entity's X / Y, range; it shines along -Z; an outline and direction stroke in the editor viewport):
- Deferred and transparent shading (`PBRLighting.slang` `evaluateRectLight`): **diffuse exact** (Lambert's polygon formula
  over the rectangle clipped to the surface's horizon), **specular from the representative point** (the rectangle's point
  nearest the reflected ray, energy normalized for the lobe the light's size widens -- Karis 2013). Not LTC (it needs fitted
  tables), close for diffuse, an approximation for sharp reflections of large lights.
- **Soft ray traced shadows**: 4 shadow rays per pixel to jittered points of the rectangle (deferred; one in the transparent
  pass), the grain smoothed by the temporal upscale.
- GI / reflection hits: the exact diffuse with a shadow ray to a random point; path tracers and ReSTIR: area sampling
  (radiance x area x cos / distance^2). Rect lights are not geometry: they are not seen directly or in mirrors (an emissive
  plane in front of one shows it).
