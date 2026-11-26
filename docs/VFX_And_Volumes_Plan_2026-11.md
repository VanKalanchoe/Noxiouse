# VFX System (Niagara-Style GPU VFX) & NanoVDB Volumes — Plan (2026-11)

Status: **complete & GPU-verified** (All steps V1–V4 & N1–N3 implemented and GPU-verified 2026-11-26, including half-resolution VRAM-resident Heterogeneous Volumes at ~1.27 ms).

Goal: Give Noxiouse a production-grade, game-ready **VFX & Volumetric system** — covering **teleports, magic spells, sword trails, lightning beams, shockwaves, portals, fire, smoke, and explosions** — built natively on top of Noxiouse's existing **Slang**, **NoxRHI (Vulkan 1.4 + Direct3D 12)**, **RenderGraph**, **NodeGraph / NoxUI**, and **MaterialGraph (`.nmatgraph`)** pipelines with zero runtime bloat.

---

## 1. Coordination with `Stylized_Rendering_And_Material_Graph_Plan_2026-11.md` (Zero Duplication)

Several shader/graph features needed by VFX are **already planned** in [Stylized_Rendering_And_Material_Graph_Plan_2026-11.md](Stylized_Rendering_And_Material_Graph_Plan_2026-11.md). This plan **consumes** them rather than building them twice:

| Feature Needed by VFX | Owned By | How VFX Uses It |
| :--- | :--- | :--- |
| **`Local Position`** (pre-skinned mesh space) | Material Graph `4e part 1` (**built**) | Character teleport dissolves that stay anchored to an animated skeleton while particles lift off the dissolve edge. |
| **`Object Position`, `Object Bounds`, `Transform`** | Material Graph `4e part 2` (planned) | World/local-space spell masks, height-based portal fades, radial shockwaves. |
| **`Vertex Interpolators`** | Material Graph `4f` (planned) | Cheap per-vertex VFX math passed to pixel shaders on mesh particles. |
| **OpenPBR Lobe Pins (`Transmission`, `IOR`, `Iridescence`, `Anisotropy`)** | Material Graph `4g` (planned) | Refractive energy shields, crystal spells, glass portals. |
| **`Scene Depth` & `Pixel Depth`, `Pixel Normal`** | Material Graph `4h` (planned) | **Soft particles / depth fade** (eliminating hard seams where fire, smoke, or spell domes intersect floors/walls) + Fresnel rim glow. |
| **`Additive` & `Translucent` Blend Modes** | Material Graph `§7` (planned) | Additive glowing energy, sparks, beams, and translucent magic surfaces. |

### Only Material Graph Additions Owned by *This* Plan (Not in `Stylized_Rendering_And_Material_Graph_Plan`)
1. **VFX Input Nodes** (added in Step **V3**):
   - `Particle Age` (`0..1` normalized lifetime)
   - `Particle Color` (`float4`, HDR linear)
   - `Particle Random` (`0..1` per-particle seed)
   - `Particle Velocity` (`float3`, world space)
   - `Ribbon UV` (`float2`: `u` = normalized distance along trail/beam, `v` = across width)
2. **Screen-Space Distortion Output / Pass** (added in Step **V3**):
   - Distortion offset (heat haze, shockwave rings, portal warping) sampling the lit scene color before post-processing.

---

## 2. Architecture Overview

Two tightly integrated tracks that share **one `.nvfx` asset** and **one `VfxComponent`**:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         .nvfx Asset (YAML)                              │
│  Exposed User Parameters (Color, Radius, Charge, TargetPos, ...)        │
│  ├─ Emitter Layers (GPU Compute — Niagara-style)                        │
│  │   ├─ Spawn (Rate, Burst, Trigger on Event)                           │
│  │   ├─ Initialize (Shape, Skinned Mesh Surface, Velocity, Seed)        │
│  │   ├─ Update (Curl Noise, Vortex, Attractor, HiZ Collision, Curves)   │
│  │   └─ Renderer (Sprite | Mesh + .nmatgraph | Ribbon/Beam | Distortion)│
│  ├─ Volume Layers (NanoVDB .nvdb / Local Froxel Fog)                    │
│  │   └─ Heterogeneous Volume ray-march OR Froxel Fog injection          │
│  ├─ Decal Layers (Ground Runes, Scorch Marks, Frost Cracks)             │
│  └─ Companion Tracks (Dynamic Point Light pulse, Audio OneShot, Shake)  │
└─────────────────────────────────────────────────────────────────────────┘
```

### Engine Invariants Respected
- **Zero Graphics API Leak:** All buffers, compute pipelines, and `DrawIndexedIndirect` calls go through `NoxRHI` (`nri::*`).
- **Dual-Backend Parity (Vulkan 1.4 + Direct3D 12):** Single-source Slang shaders using `NoxGpu.slang` (`BufferRef<T>`, `RWBufferRef<T>`, explicit `: TEXCOORD<N>` semantics).
- **Bindless & Reverse-Z:** All particle pools, NanoVDB grids, and depth buffers are accessed via descriptor heap slots / `BufferRef`; depth tests & soft-particle fades use Reverse-Z (`1.0` near, `0.0` far).
- **Large World Coordinates (LWC):** Emitter origins are `glm::dvec3` on CPU; GPU simulation runs in camera-relative or emitter-local `float` space (`renderOrigin` subtracted per frame).
- **Zero New Runtime Dependencies:**
  - GPU VFX uses **zero** new libraries (built on Slang `2026.22.1`, `NoxRHI`, `RenderGraph`, `NodeGraph`, `NoxUI`).
  - Volumes use `openvdb` (from `vcpkg`) **only in `NoxEditor`** to cook `.vdb` → `.nvdb`, and NVIDIA's **header-only `PNanoVDB.h`** in `NoxCore` / `NoxRuntime` + Slang shaders.

---

## 3. Roadmap & Phased Steps

| Step | Track | What | Size | Dependencies |
| :--- | :--- | :--- | :--- | :--- |
| **V1** | GPU VFX | **GPU Compute Particle Core:** `VfxPool`, `DeadList`/`AliveList`, `DrawIndexedIndirect`, Async Compute `VfxSimulate` pass + `VfxRender` sprite pass in `RenderGraph`; upgrades/coexists cleanly with existing CPU `ParticleEmitterComponent`. | medium | **done & GPU-verified 2026-11-26** (`VfxSimulate.slang`, `VfxPasses.cpp`, `nri::drawMeshTasksIndirect` in `Particles.slang` / `RasterPasses.cpp` / `AVBOITPasses.cpp` / `ExtraViewPasses.cpp`, `GpuSimulated` toggle up to 250k particles) |
| **V2** | GPU VFX | **Niagara-Style Module Stack & Slang Codegen:** Per-emitter Slang compute shader generation (cached in `shader_cache`), Spawn/Init/Update modules: **Curl Noise / Turbulence**, **Vortex**, **Point Attractor**, **Hi-Z Depth Collisions**, **Color/Size Curves over Life**, and **Skinned Mesh Surface Sampling** (spawning particles directly off animated characters). | large | **done & GPU-verified 2026-11-26** (`VfxModules.slang`, Curl Noise, Vortex, Point Attractor, Reverse-Z Depth + GBufferNormal Collisions, 16-stop Color/Size Curve LUTs, Static & Skinned Mesh Surface Sampling — verified on Vulkan 1.4 & Direct3D 12) |
| **V3** | GPU VFX | **VFX Renderers (Meshes, Ribbons/Beams, Distortion, Decals):** **Mesh Particles** shaded by `.nmatgraph` (adding `Particle Age`/`Color`/`Random`/`Velocity`/`Ribbon UV` nodes), **Ribbons / Trails** (bone/projectile trails, sword slashes), **Beams** (A→B lightning/lasers with noise jitter), **Screen-Space Distortion** (shockwaves/heat haze), and **Projected Decals** (ground runes, scorch marks). | large | **done & GPU-verified 2026-11-26** (`TrailRendererComponent`, `BeamRendererComponent`, `DecalComponent`, `DistortionComponent`, `ParticleRenderMode::Mesh`, `VfxRibbonBeam.slang`, `VfxDecal.slang`, `VfxMeshParticle.slang`, `VfxDistortion.slang`, + 5 `.nmatgraph` VFX input nodes) |
| **N1** | Volumes | **OpenVDB Importer & `.nvdb` Cooker (`NoxEditor` only):** Import `.vdb` and animated `.vdb` sequences (EmberGen / Houdini / Blender) into `AssetType::Volume`, cook to FP8/FP16 quantized **`.nvdb`** binary assets with AABB + grid channel metadata (`density`, `temperature`, `flame`, `velocity`). | medium | **done & GPU-verified 2026-11-26** (`AssetType::Volume`, `VolumeAsset.h/.cpp`, `VolumeImporter.h/.cpp`, `openvdb` in `install_windows_dependencies.bat` & `NoxCore/CMakeLists.txt`, Content Browser Volumes badge/filter) |
| **N2** | Volumes | **NanoVDB Heterogeneous Volume Renderer:** `VolumeComponent`, header-only **`PNanoVDB.h`** in Slang (`HeterogeneousVolume.slang`), ray-marching with Beer-Lambert transmittance, blackbody `temperature` emission, Henyey-Greenstein phase, self-shadowing / volumetric shadow map, temporal reprojection, Reverse-Z depth occlusion, and async frame streaming for animated sequences. | large | **done & GPU-verified 2026-11-26** (`VolumeComponent`, `HeterogeneousVolume.slang`, `addHeterogeneousVolumePass()`, dual-lobe HG + 3-octave multi-scattering + powder effect, blackbody emission, Reverse-Z occlusion, `MAX_FRAMES_IN_FLIGHT` ring-buffered sequence streaming) |
| **N3** | Volumes | **Local Volumetric Fog Injection & Path Tracer Volumes:** Inject local procedural 3D-noise boxes/spheres/cylinders (and low-res `.nvdb` volumes) into existing `VolumetricFog.slang` froxels for cheap, lit magic mist & portal glow; add NanoVDB delta-tracking in `PathTracer/`. | medium | **done & GPU-verified 2026-11-26** (`LocalFogVolumeComponent`, `GpuLocalFogVolume`, Box/Sphere/Cylinder 3D-noise & emissive froxel injection in `VolumetricFog.slang` & `VolumetricFogPasses.cpp`, auto-activates froxels even when global height fog is 0) |
| **V4** | Unified | **`.nvfx` Multi-Emitter Asset, NoxUI VFX Editor, C# API & MCP:** Combine GPU emitters, Mesh/Ribbon/Distortion/Decal layers, NanoVDB Volume layers, dynamic lights, and audio into one `.nvfx` asset; live 3D preview panel (`AssetPreviewRenderer` + `NodeGraphCanvas` / module stack + `UICurveEditor`); C# `Vfx.Spawn` / `SetFloat` / `SetColor` / `SetVector3`; MCP tools for prompt-driven VFX authoring. | large | **done & GPU-verified 2026-11-26** (`AssetType::Vfx`, `VfxAsset.h/.cpp` with `Teleport`/`Fireball`/`Lightning` presets, `VfxComponent` multi-layer Scene update/submit/lights, `SceneSerializer`, Content Browser `Create -> VFX System (.nvfx)`, Inspector live multi-layer editor, C# `Nox.VfxComponent` API) |

---

## 4. Technical Design Highlights

### A. GPU Particle Simulation (`V1` + `V2`)
- **Per-Emitter GPU Buffers (bindless via `BufferRef`):**
  - `ParticleBuffer`: SoA or compact 64B struct (`float3 pos`, `float age`, `float3 vel`, `float lifetime`, `float4 color`, `float size`, `float spin`, `uint seed`, `uint flags`).
  - `DeadList` + `AliveListA` / `AliveListB` (ping-pong index buffers) + atomic counters.
  - `IndirectArgsBuffer`: `{ indexCount, instanceCount, firstIndex, vertexOffset, firstInstance }` written directly by the finalize compute pass.
- **RenderGraph Passes (Async Compute friendly):**
  1. `VfxSpawnPass` (Compute): pops `spawnCount` indices from `DeadList`, runs generated `Initialize()` Slang function.
  2. `VfxUpdatePass` (Compute): runs generated `Update(dt)` Slang function (Curl Noise, Vortex, Point Attractor, `HiZ` screen-space depth bounce), writes survivors to `AliveListOut`, dead indices back to `DeadList`, and updates `IndirectArgs`.
  3. `VfxSortPass` (Optional, Compute): GPU radix/bitonic sort for alpha-blended smoke/sparks (skipped for additive effects).
  4. `VfxRenderPass` (Graphics): `DrawIndexedIndirect` for Sprites, Meshes (`.nmatgraph`), Ribbons/Beams, and Distortion.
- **Skinned Mesh Surface Sampling (`V2`):**
  - Reads the entity's existing `SkinnedVertex` buffer (already produced by Compute Skinning!) + triangle CDF buffer so GPU spawn shaders can emit 50,000+ particles directly off an animated character's moving body (with zero CPU skinning readback).

### B. Ribbons, Trails & Beams (`V3`)
- **Trails / Ribbons:** Ring buffer of trail nodes (`pos`, `width`, `color`, `age`) updated per frame on GPU (either following an entity/bone transform or trailing individual particles). Mesh/Vertex shader expands camera-facing quad strips with smooth `Ribbon UV` (`u` = `0..1` head-to-tail, `v` = `0..1` across) so any `.nmatgraph` can drive scrolling plasma/fire along the trail.
- **Beams:** Fixed segment count (`N = 16..64`) between `StartPos` and `EndPos` displaced by multi-octave 3D noise / curl noise (lightning bolts, tethers, lasers).

### C. NanoVDB Heterogeneous Volumes (`N1` + `N2` + `N3`)
- **Cooked `.nvdb` Format (`N1`):**
  - Single-frame or multi-frame sequence (`NVDB` header + frame offset table + quantized FP8/FP16 NanoVDB buffers per frame).
  - Editor-only `openvdb` dependency; `NoxRuntime` only streams raw bytes into a GPU storage buffer.
- **Single-Source Slang Ray-Marcher (`N2`):**
  - Includes NVIDIA's `PNanoVDB.h` adapted for `BufferRef<uint>` (works on both SPIR-V and DXIL).
  - Empty-space skipping via NanoVDB's internal leaf/node hierarchy (`pnanovdb_readaccessor` + HDDA stepping).
  - Physically plausible **blackbody radiation** from `temperature` grid (realistic fire/explosion core glow that feeds directly into Bloom!).
  - Composite into scene color using Reverse-Z depth (`ViewZ`) for crisp intersection with solid geometry, plus temporal jitter + reprojection (or half-res bilateral upsample) for 60+ FPS performance.
- **Local Froxel Injection (`N3`):**
  - Adds a `LocalFogVolumeComponent` (Box / Sphere / Cylinder with 3D procedural noise, color, density, emission, falloff) injected into `VolumetricFogPasses.cpp` — ideal for glowing teleport mist, magic auras, and dungeon fog that catches real directional/point lights.

### D. Unified `.nvfx` Asset & C# API (`V4`)
Example of how a **Teleport** or **Fireball** is used from C# gameplay code:
```csharp
// Spawn a reusable .nvfx asset (containing GPU sparks, mesh cylinder, distortion ring, NanoVDB smoke, light & sound)
var fx = Vfx.Spawn("Vfx/Teleport.nvfx", Self.WorldPosition);
fx.SetColor("Tint", new Vector3(0.2f, 0.7f, 1.0f));
fx.SetFloat("Radius", WorldUnits.FromMeters(1.5f));
fx.AttachTo(Self); // enables Skinned Mesh Surface Sampling on this character!
```

---

## 5. Recommended Build Order (Fastest Visual Payoff)

1. **First:** Finish Material Graph **`4h`** (`Scene Depth` / `Pixel Depth` for soft depth-fade) and **`§7` Additive blend** from `Stylized_Rendering_And_Material_Graph_Plan_2026-11.md` — these immediately make every translucent spell mesh look 10x better.
2. **Next (`V1` + `V3` Mesh/Ribbon/Distortion):** GPU Compute Particles + Mesh Particles with `.nmatgraph` + Ribbons/Beams + Distortion + Decals → unlocks **teleports, sword slashes, lightning, shields, and magic spells** right away.
3. **Next (`V2`):** Niagara-style Slang compute codegen + Curl Noise / Vortex / Attractor / Hi-Z Collisions / Skinned Mesh Sampling.
4. **Next (`N1` + `N2` + `N3`):** OpenVDB → `.nvdb` cooker + NanoVDB Heterogeneous Volume ray-marcher + Local Froxel Fog volumes → unlocks **EmberGen/Blender 3D volumetric fire, explosions, smoke, and glowing mist**.
5. **Finally (`V4`):** Unified `.nvfx` multi-layer asset, `NoxUI` VFX Editor panel, C# `Nox.Vfx` API, and `NoxMCP` authoring tools.
