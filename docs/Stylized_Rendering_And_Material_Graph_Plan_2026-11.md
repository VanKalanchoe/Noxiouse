# Stylized rendering and the material graph -- plan (2026-11-14)

Status (2026-11-14): **step 1 (Toon shading model) built and tested by the user; step 2 (outlines, Post Process Volume) built, waiting for the user's test**; steps 3-7 planned. Backlog item R13.

Goal: Nox is not tied to realistic lighting. Materials can be lit physically (the default), not at all (Unlit), with a built-in toon
model, or -- later -- with lighting of their own; and artists build surfaces from nodes (a material graph) without writing shaders.
The Unreal way: data-verified, every step usable on its own, nothing half-built in the shipping path.

## 1. What a material graph is (and is not)

A material graph turns a node network into shader code at edit time (Unreal's Material Editor, Unity's Shader Graph, Godot's Visual
Shader -- three names for the same idea). Every material ends up as GPU shaders; the question is only how much of the shader the
engine lets you write:

| | Surface (colour, roughness, normal, glow, opacity, vertex offset) | Lighting (how light hits the surface) |
| :--- | :--- | :--- |
| Unreal 5 | Material Editor | Fixed list of shading models (Default Lit, Subsurface, Cloth, Hair, Unlit ...); a new one is an engine change |
| Unity (URP / HDRP) | Shader Graph, hand-written HLSL | Hand-written shaders; in Shader Graph via a Custom Function node in an Unlit graph |
| Godot 4 | Visual Shader, its shader language | `light()` per material; toon built in (`diffuse_toon`, `specular_toon`) |

The pattern: **deferred renderers fix the lighting models** (all pixels are lit in one pass), **forward renderers allow custom
lighting** (each material lights itself). Nox is deferred (visibility buffer + deferred lighting) **and** has a forward path
(transparent, unlit) -- realistic materials stay deferred, custom-lit ones draw forward.

Compute is never part of a material in any of the three: GPU particles are Niagara (Unreal) / VFX Graph (Unity), compute shaders are
code. Nox keeps compute separate too (its particles have their own system; a node-based particle system would be its own project).

A material makes two independent choices: **the surface** (what the graph builds) and **the shading model** (how it is lit). A
dissolve, triplanar, layered moss, panning water or wind on leaves is a surface effect on a *Lit* material -- realistic lighting
stays. Only "change how light behaves" needs Custom Lit.

## 2. Why deferred + visibility buffer stays (no Forward+ renderer)

Screen-probe GI, SSR, GTAO, SSGI, the NRD denoisers and DLSS Ray Reconstruction read per-pixel normals, roughness and depth, and the
hybrid ray tracing starts from them: a forward renderer would write most of a G-buffer anyway. The visibility buffer makes dense,
streamed geometry cheap. Unreal 5 made the same choice.

What it costs for custom materials: one system -- **material tile classification** in the visibility buffer (Unreal 5 does it for
Nanite) when graph materials arrive (step 4); custom lighting goes through a **forward pass inside the deferred frame** (step 3),
which Nox's transparent path already proves (`Material_TransparentLit_Mesh.slang` lights with the same `evaluateDirectLight`).
A full second Forward+ renderer would double the work of every screen effect for no gain over that pass: not planned.

## 3. Settings: nothing to switch globally

| Where | Setting | Chosen by |
| :--- | :--- | :--- |
| Material Editor, per material | **Shading Model:** Default Lit / Unlit / Toon / later Custom Lit; Toon's own fields | the user |
| Post-process settings, per scene | **Outlines** (on, thickness, colour, which surfaces) | the user |
| Project Settings, once (optional) | **Rendering style:** Physical (default) / Stylized -- Stylized hides the path tracer presets | the user |
| Deferred or forward | none: Lit / Unlit / Toon are deferred, Custom Lit goes forward | the engine |
| Ray tracing / path tracer | the quality preset as today; non-physical materials use their fallback, the editor warns | preset: user; fallback: automatic |

Realistic and stylized materials mix in one scene. Every step only adds options; existing materials stay Default Lit and look the
same.

## 4. Ray tracing and the path tracer with non-physical materials

- **Hybrid stays.** Toon / custom lighting is computed on screen by raster; the ray-traced effects only need what the surface looks
  like in a reflection or as bounced light. Each non-physical material has a **ray-tracing fallback** (its base colour and emissive,
  or later a graph branch -- Unreal's Ray Tracing Quality Switch). Ray-traced shadows need only the geometry.
- **Path tracer:** it computes everything physically, so toon surfaces appear there as their fallback (Default Lit with their base
  colour). The editor will list such materials; the optional *Rendering style: Stylized* hides the path tracer presets.

## 5. Naming

The Material Editor uses glTF's names today (Alpha Mode Opaque / Mask / Blend, Alpha Cutoff, Double Sided, Occlusion). The labels the
user sees move to Unreal's (Blend Mode Opaque / Masked / Translucent, Opacity Mask Clip Value, Two Sided, Ambient Occlusion,
Shading Model) when the Material Editor gets its Shading Model control (step 1) -- labels only: the glTF importer, the `.nmat`
fields and the GPU structs keep their names (no file or import change).

## 6. Parameters and instances (with the graph)

One system, two layers, as in all three engines: the graph declares **parameters** (Scalar / Vector / Texture Parameter nodes;
Static Switch for compile-time choices); a **material instance** (`.nmat` with a parent graph) overrides their values with no
recompile; scripts change them per entity at run time (C# `SetFloat` / `SetColor` / `SetTexture`, Unreal's Dynamic Material
Instance). Today's `.nmat` already acts as an instance of one built-in "Standard" material (like Godot's `StandardMaterial3D`).

## 7. The output node (with the graph)

Pins map to what `GpuMaterial` already holds, so lighting, RT and the path tracer read graph materials like `.nmat` ones: Base
Color, Metallic, Roughness, Normal, Emissive, Ambient Occlusion, **Opacity**, **Opacity Mask**; Clear Coat (+ Roughness, Normal),
Sheen, Specular, Transmission, IOR, Thickness / Attenuation, Diffuse Transmission, Iridescence, Anisotropy, Dispersion; later World
Position Offset. Material settings: Blend Mode (Opaque / Masked / Dithered / Translucent / Additive), Opacity Mask Clip Value, Two
Sided, Shading Model. (Godot's ALPHA_ANTIALIASING_EDGE / alpha-to-coverage needs MSAA, which Nox does not use: not planned.)

## 8. Steps

| Step | What | Size | Status |
| :--- | :--- | :--- | :--- |
| 1 | **Toon shading model**: Shading Model per material (Default Lit / Unlit / Toon), toon bands or ramp texture, sharp highlight, rim light; deferred (G-buffer flag + toon parameters in the lobe target) and forward transparent; RT / PT fallback = Default Lit; Material Editor labels to Unreal's names | small | built, tested by the user |
| 2 | **Outlines** post effect (depth / normal edges, thickness, colour, all surfaces or toon only), in a **Post Process Volume** (Unreal's; saved with the scene -- the post effects were editor-only before) | small | built (waiting for the user's test) |
| 3 | **Forward pass for opaque custom-lit materials** inside the deferred frame (writes depth, normals, velocity and a "custom lit" flag; deferred lighting skips those pixels); a light grid if many lights | medium | planned |
| 4 | **Material graph** (surface): Material domain in the NodeGraph framework, node library, Slang code generation into per-pass templates, material tile classification in the visibility buffer, RT fallback then per-material hit groups, parameters and instances, C# parameter API, packaging of every variant | large | planned |
| 5 | **Custom Lit** in the graph (a per-light function; forward pass from step 3) | medium | planned |
| 6 | **Hand-written material shaders** (a Slang file supplying the function the graph would generate) | medium | planned |
| 7 | **Rendering style** project setting (Physical / Stylized) and the editor's list of non-physical materials | small | planned |
| -- | Scriptable render passes (Unity SRP style) | -- | not planned |
| -- | Vertex animation (World Position Offset): shadows, streaming bounds, BLAS refits | -- | after step 4 |

## 9. Step 1 design (Toon shading model)

- `MaterialData::ShadingModel` (`MaterialShadingModel`: DefaultLit 0, Unlit 1, Toon 2) replaces `bool Unlit`: it is the same byte
  in the `.nmat` / cooked layouts (0 / 1 read as before), so no cooked mesh is made again. glTF's KHR_materials_unlit sets Unlit.
- Toon fields (`MaterialToonSettings`): Bands (1-8; 1 = two tones), Band Softness, an optional Ramp texture (sampled along its width
  by the light amount: replaces the bands), Specular Size / Softness / Intensity, Rim Color / Width / Intensity. Stored after the
  other fields in `.nmat` files ("NMA4"; "NMA3" files read with the defaults). A cooked mesh's embedded glTF materials do not carry
  them (glTF has no toon model).
- GPU: `GpuMaterial::shadingModel` (was `unlit`) and the toon fields. The G-buffer flag byte's free bit 0x40 marks toon pixels;
  their lobe target (RGBA32_UINT, unused by a toon pixel: lobes are Default Lit only) carries the toon parameters
  (`gbuffer_encode_toon` / `gbuffer_decode_toon`), as Unreal keeps shading-model data in its CustomData target.
- Lighting (`ToonShading.slang`), for directional / point / spot lights: diffuse = albedo x light x ramp(N.L x shadow) -- shadows
  are banded with the light, no soft gradient; highlight = a sharp N.H step of the given size and softness (white for dielectrics,
  base colour for metals), shadowed; rim = a band at grazing angles (1 - N.V) lit by the light's side, in the rim colour. Ambient
  (GI, sky) and reflections as for Default Lit (roughness 1 gives none). Rect / disc lights light toon surfaces as Default Lit.
  ReSTIR DI's resampled lighting is physical: toon pixels take the direct light loop instead.
- Forward transparent: the same functions. RT hit shading and the path tracer: the material as Default Lit (the fallback).
- Material Editor: Shading Model combo + Toon section; labels to Unreal's names (section 5). MCP material JSON: `ShadingModel` and
  the `Toon*` fields (`Unlit` still read).

## 9b. Step 2 as built (outlines, Post Process Volume)

The post effects lived only in the editor's Renderer Settings (not saved, not in a packaged game), so the outlines come with Unreal's
Post Process Volume: `PostProcessVolumeComponent` (Unbound or a box, Priority) holds a `PostProcessSettings`; `Scene::SubmitPostProcess`
hands the applying one to the renderer each frame (none: the Renderer Settings). Outlines: `SceneOutline.slang`, a full-screen raster
draw into the lit scene before TAA / DLSS with Zero / SrcColor blending (the scene times the outline colour) -- depth edges
(silhouettes, relative to the distance) and normal edges (creases) on a ring `thickness` pixels out, seen from the nearer side;
toon-only by the G-buffer's toon bit; distance fade. Not with the path tracer or debug views; lines show through glass in front of an
edge. Not yet: blending between volumes, a C# API for the volume, the LUT file packaged with the game.

## 10. Tests

Step 1: the `.nmat` round trip of the shading model and toon fields, NMA3 files read with defaults (NoxToolsTest); shader compile
checks; the user's look test on the UAL1 character in `AnimationTest2.nox` (the files live only on the user's machine).
