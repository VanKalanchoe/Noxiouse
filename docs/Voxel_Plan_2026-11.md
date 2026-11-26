# Voxels -- Plan (2026-11)

Status: **planned (2026-11-26), nothing built.** Backlog item **VX** (`Backlog_2026.md`).

Goal: tiny voxels (10 cm by default) in Noxiouse, two ways at once:

- **Voxel meshes** -- MagicaVoxel models (`.vox`) imported as ordinary meshes: props, furniture, a workbench, decorative buildings.
  Static as wholes (placed, moved, removed), drawn by everything the renderer already has (visibility buffer, meshlets, LOD, RT, GI).
  Nivalis (ION LANDS, Unity) works this way: MagicaVoxel art turned into meshes, no voxel engine.
- **The voxel world** -- a sparse **brick map** for what the player digs, builds, destroys and what is generated without end (terrain,
  caves, player-built houses): drawn by a voxel pass that steps through the voxels in the pixel shader (DDA, Teardown's way) and feeds
  the same G-buffer, depth and motion vectors as meshes, so lighting, shadows, GI, TAA / DLSS, NRD, fog, post effects and later ray
  tracing apply unchanged.

The rule for content: **should the player dig, break or build it voxel by voxel?** Yes: the brick map. No: a voxel mesh. Both write the
same depth buffer and G-buffer and mix freely (a character in voxel terrain, a meshed workbench on a voxel floor).

Order (the user's decision): **raster first, at 10 cm, starting with loading a `.vox` model**; ray tracing after the raster preset works.

---

## 1. Decisions

| Topic | Decision | Why |
|---|---|---|
| Voxel size | **10 cm** world voxels (project setting; 5 cm possible); later per-object grids at 2.5-5 cm for props and debris (§9 VX8) | The proven tiny-voxel size (Teardown). At 2560x1440 / 90 degrees a pixel covers ~8 mm at 10 m and ~8 cm at 100 m: smaller voxels turn sub-pixel at play distances and cost memory (4x per halving: surfaces scale with area) for nothing visible |
| World structure | Chunks of **64^3 voxels (6.4 m)**, each 8x8x8 **bricks of 8^3** | Brick = 512 voxels: 512 bytes at one palette index each, plus a 64-byte occupancy mask; a chunk is the unit of generation, streaming, saving and collision |
| Voxel value | 1 byte: index into the chunk's palette (0 = air), up to 255 materials per chunk | Minecraft's sections store a palette + indices the same way (`PalettedContainer`); compresses very well |
| Rendering of the world | Raster the boxes of the visible non-empty bricks, DDA inside the brick in the pixel shader, conservative depth (`SV_DepthGreaterEqual` with reverse-Z), output `GBufferOutput` | Cost per pixel, not per voxel; edits only write bytes (no remesh); LOD from brick mips; the G-buffer keeps every later pass unchanged |
| Rendering of `.vox` props | Meshes: visible faces merged (greedy, per palette entry) into a `StaticMesh`; colours and materials from palette textures | Static art needs no voxel pass; the mesh path already has everything |
| Collision of the world | Greedy-meshed, simplified (meshoptimizer) collision mesh per chunk, Jolt `MeshShape`, rebuilt off the main thread after edits | Where greedy meshing is the right tool; a custom Jolt voxel shape only if destruction at scale needs it (VX8) |
| Edits | **Brush operations** (shape, size, add / subtract / paint / smooth, noise on the edge), applied by the authority, sent over the network as operations | Fun and fast to dig (§2), small to send, the same result everywhere |
| Chunk and brick index | **ankerl::unordered_dense** maps (chunk position packed into 64 bits -> chunk; dirty sets), from the first line of code | Millions of lookups per second in generation, edits, streaming and meshing; far faster than `std::unordered_map`; no rework later |
| World generation in multiplayer | **Every machine generates; the server sends only edits** -- FastNoise2 capped at **SSE4.1** on every machine (§3a), a hash per generated chunk checked against the server's, a full chunk sent only on a mismatch or for chunks clients cannot generate | Sending every chunk costs megabytes per second per player at 10 cm (§3a); generating everywhere costs a little CPU |
| Ray tracing (later) | Bricks as **AABBs** in a BLAS per chunk; the shared ray-query code handles procedural candidates with the brick DDA; secondary rays trace coarser mips | Every RT effect (shadows, AO, reflections, GI, DDGI, ReSTIR, path tracer) then sees voxels without per-pass rewrites. NoxRHI's BLAS takes triangles only today (`NriAccelerationStructureTrianglesDesc`): it needs AABB geometry (Vulkan `VK_GEOMETRY_TYPE_AABBS_KHR`, D3D12 procedural primitive AABBs) |
| Targets | Raster preset **120 fps**, RT preset **90 fps** at 1440p on the user's GPU | Estimates from how such engines work, to be measured from VX2 on (§8) |

What is **not** planned: NanoVDB / OpenVDB as the runtime world (read-only, or CPU-heavy: kept for import and smoke), meshing the world
for rendering (triangle counts explode at 10 cm with per-voxel colours; every edit a remesh and a BLAS rebuild), a third-party voxel
engine (none fits: PolyVox unmaintained, Voxel Plugin Unreal-only, GVDB CUDA-only).

## 2. Digging and building (gameplay defaults)

Neither 1 m (it throws the detail away) nor single voxels (1 m^3 = 1,000 voxels: a chore). **Tools carve shapes; the voxel size gives
the detail of the result, the tool gives the speed** (Teardown, Enshrouded, Deep Rock Galactic, Astroneer, each their own way).

| Tool | Default | Notes |
|---|---|---|
| Hand tool (shovel / pickaxe) | Removes a lumpy blob of **30-50 cm** per hit | Edge roughened with noise: holes look dug, not like perfect spheres; per-material hardness scales the size or the hits |
| Fine tool (chisel) | **10-20 cm** | Detail work: a doorway, a window, a carved sign |
| Continuous tool (drill, terrain sculptor) | A cylinder / sphere while held | Tunnels, levelling ground |
| Explosives | **1-3 m** | Debris VFX in the removed voxels' colours |
| Building | Shapes on the **10 cm grid**: size selector 10 / 50 / 100 cm, cube / slope / cylinder / sphere, fill and paint | Snapping to the grid and to existing faces |

Removed voxels count per material; the inventory shows friendly units (1 unit = 0.1 m^3 = 100 voxels). Later (VX8): loose parts that
lost their connection fall as physics bodies (Teardown).

The engine side is the same for every tool: a **brush** = shape (sphere, box, cylinder, capsule, mesh SDF) + transform + size +
operation (subtract, add with a palette index, paint, smooth) + edge noise (seed, amplitude). Games define tools as brush presets
(script API); the editor uses the same brushes for level design.

## 3. Libraries

| Library | License | Use |
|---|---|---|
| **FastNoise2** | MIT | World generation on the CPU: SIMD node trees filled into whole 3D grids (`GenUniformGrid3D`), designed in its NoiseTool and stored as the encoded tree string in a generator asset. Created with `maxFeatureSet = SSE41` on every machine (§3a) |
| **zstd** (LZ4 if load speed wins) | BSD | Chunks on disk, streamed cells, saved edit deltas (palette-indexed bricks compress well) |
| ankerl **unordered_dense** | MIT | The CPU chunk / brick index and dirty sets (position packed into 64 bits -> slot), used from the start |

Already in the engine: **OffsetAllocator** (the GPU brick pool), **Jolt** (collision), **meshoptimizer** (simplifying collision /
prop meshes), **tinygltf** + editor **OpenVDB** (glTF -> voxels, VX9), **xxhash** (content hashes), **NoxTasks / taskflow**
(generation, meshing, compression in parallel), **Tracy**. Written here (the user's decision): the **`.vox` reader** (not ogt_vox),
the brick map, the DDA shaders, greedy meshing.

## 3a. Determinism and what multiplayer sends (measured 2026-11-26)

**FastNoise2 across instruction sets** (FastNoise2 c35d604, GCC, a CPU with AVX-512; Simplex, SuperSimplex, Perlin, Value, Cellular,
5-6 octave FBm and Ridged, domain-warped FBm; 64^3 grids at the origin, at 640 voxels and at one million voxels out, compared bit for
bit): **SSE2 and SSE4.1 give identical output; AVX2 and AVX-512 differ from them in almost every sample** (by 1e-7 to 1e-4, fused
multiply-add rounding; Cellular differs between AVX2 and AVX-512 too). FastNoise2 picks the best level of the CPU at run time, so by
default two players' machines generate slightly different worlds: a voxel whose density lies within 1e-4 of the surface threshold can
be solid on one and air on the other. **Capped at SSE4.1** (`FastNoise::New<T>(FastSIMD::FeatureSet::SSE41)`,
`NewFromEncodedNodeTree(tree, SSE41)`) every x86-64 machine runs the same code. Cost, one 64^3 chunk of domain-warped 5-octave simplex
FBm on one core: SSE2 17.2 ms, **SSE4.1 13.1 ms**, AVX2 7.4 ms, AVX-512 3.9 ms -- about 600 chunks a second on 8 worker threads,
and chunks fully above or below the surface skip most of it. Still to verify (VX1 test): the same chunk hashes on a Windows MSVC
build and a Linux GCC / Clang build (no `-ffast-math` / `/fp:fast` in the generator), and ARM (NEON) machines differ by design: they
take chunks from the server.

**What Minecraft sends:** the server generates (single player runs an integrated server too) and sends whole chunks -- each 16^3
section as a block palette + bit-packed indices, plus heightmaps, block entities and light, zlib-compressed above a size threshold;
afterwards only block changes. Clients never generate terrain. Hytale's internals are not documented publicly; it is
server-authoritative with chunks streamed by the server in the same family. At 1 m blocks this is cheap. **At 10 cm it is not:** a
64^3 chunk is 262 KB raw, roughly 2-20 KB after palette packing and zstd; a player running at 10 m/s with a 256 m view distance meets
on the order of hundreds of new near-surface chunks a second -- megabytes per second per player before LOD.

**The plan's model:** the world is the seed + the generator asset + the edits. Every machine generates chunks itself (capped at
SSE4.1); the server is authoritative for edits, which travel as brush operations (§2) and are kept per chunk as an edit log / brick
deltas (also what a save game and a late joiner receive); after generating a chunk a client compares its hash (xxhash of the
generated bricks) with the server's and asks for the chunk when they differ (an ARM client, a driver or compiler surprise) -- the
server then sends it compressed. Far chunks can come at a coarser LOD when they must be sent. Late joiners get the edit logs of the
chunks around them, not the chunks.

## 4. `.vox` voxel meshes (VX0)

- **Reader** (`NoxCore/src/NoxCore/Voxel/VoxReader.*`): RIFF-style chunks -- `MAIN`, `PACK`, `SIZE` + `XYZI` (models), `RGBA`
  (palette; the default palette when absent), `MATL` (`_type` diffuse / metal / glass / emit / blend, `_rough`, `_metal`, `_ior`,
  `_emit`, `_flux`, `_trans`), scene graph `nTRN` (translation, rotation as the packed byte, frames), `nGRP`, `nSHP`, `LAYR`
  (hidden layers skipped); unknown chunks skipped; every size checked against the file (a broken file is an error, never a crash).
- **Import** (Content Browser drop / Import, like models): one `StaticMesh` per model -- visible faces only, greedy-merged per palette
  entry, scaled by the project's voxel size (default 10 cm); a multi-model scene becomes a **prefab** with the scene graph's
  transforms. Pivot: the model's centre or its bottom centre (import option).
- **Materials:** one `.nmat` per model with **palette textures** (256 x 1: base colour; a second for roughness / metallic / emission
  strength from `MATL`); each face's UVs point at its palette texel (point sampling). Glass entries (`_type glass`) a submesh with a
  transmission material. Emissive entries feed emission (and so the emissive light grid / GI).
- **LOD:** meshoptimizer simplification as for other meshes; the voxel look is kept by simplifying only coplanar merges.
- **Done when:** the user's MagicaVoxel models import, place and render in raster and RT presets; a `.vox` with several models
  becomes a prefab with the right transforms.

## 5. The brick map (VX1-VX2)

**CPU side** (`NoxCore/src/NoxCore/Voxel/`):

- `VoxelChunk`: 64^3, an array of 512 brick slots (empty / uniform with one index / full brick of 512 bytes), the chunk palette
  (index -> material entry), a revision. Encoded for disk: brick masks + palette indices bit-packed to the palette's size, then zstd.
- `VoxelWorld` (a scene component / world-level system): the chunk map (unordered_dense), generation requests, edit application,
  dirty tracking, saving; world partition cells own the chunks inside them.
- `VoxelGenerator` asset: a FastNoise2 tree (encoded string) + layers (surface material by height / slope, caves, ores) evaluated
  per chunk on NoxTasks; deterministic from the world seed.
- Palette: a project-wide **voxel material table** (name, base colour, roughness, metallic, emission, hardness, footstep surface,
  physical material); chunk palettes index into it.

**GPU side** (`Renderer`, pass in `Passes/VoxelPasses.cpp`, shader `Voxel.slang`):

- **Brick pool:** one large buffer of 512-byte bricks + occupancy masks, sub-allocated with OffsetAllocator; an upload budget per
  frame (MB), uploads from the dirty list.
- **Chunk table:** per resident chunk its origin, LOD, 512 brick pointers (pool offset or uniform value), palette offset.
- **Brick list:** per frame, a compute pass culls the resident non-empty bricks that have at least one exposed face against the
  frustum and the previous frame's Hi-Z and writes an indirect draw of their boxes (front to back by chunk).
- **Voxel G-buffer pass:** draws the brick boxes (back faces culled; the camera inside a brick handled by drawing that brick's box
  inside out), the pixel shader starts the ray at the box entry point (or the camera) and steps the 8^3 voxels with a DDA using the
  occupancy mask (64-bit words: whole rows skipped), at the first solid voxel writes `GBufferOutput` (base colour, roughness, metallic,
  emission from the material table; the hit face's normal; entity ID for picking: the world's entity; motion vectors from the camera
  for the static world) and conservative depth; misses `discard`. It runs with the visibility buffer's depth so meshes and voxels
  occlude each other (order: meshes' visibility buffer + resolve, then voxels depth-tested against them, or the reverse -- measured).
- **Ambient occlusion per voxel** (optional, cheap and very "voxel"): corner AO from the neighbours, as Minecraft-style games do, on top
  of the renderer's AO.

## 6. Raster preset coverage (VX3)

The voxel draw (box list + DDA, depth only) in every pass that draws geometry itself: sun shadow cascades (cascade caching invalidated
by edits inside a cascade's bounds), the local-light shadow atlas, the irradiance volume and reflection probe captures, extra views
(ortho, previews), editor picking and the selection outline. Screen-space passes, deferred lighting, TAA / DLSS / NRD, fog, outlines
and post effects need nothing.

## 7. Phases

| Phase | What | Done when |
|---|---|---|
| **VX0** | `.vox` reader + import as meshes / prefabs, palette materials (§4) | The user's models in the editor, both presets |
| **VX1** | Brick map CPU side: chunks, bricks, palette, voxel material table, encoding + zstd, FastNoise2 generator asset, a `.vox` model stamped into the world | A generated 10 cm terrain + a stamped `.vox` in memory; offline tests |
| **VX2** | GPU brick pool + brick culling + voxel G-buffer pass (§5), raster lighting | Terrain and the stamped model lit in the raster preset; frame time measured at 1440p |
| **VX3** | Raster preset coverage (§6) | Voxel shadows (sun + local lights), probes, extra views, picking |
| **VX4** | Brushes (§2): edits, dirty bricks, uploads, debris VFX, C# API (`VoxelWorld.Apply(brush)`, queries: raycast to a voxel, material at a point), edit deltas saved per cell, brush ops over the network | Digging and building in Play, saved and loaded, in multiplayer |
| **VX5** | Streaming + LOD: chunks by distance through world partition, brick mips (2x2x2: the most common non-air index), LOD per distance, upload budget, an endless generated world | Flying across an endless world at the frame-rate target |
| **VX6** | Collision: greedy-meshed simplified chunk meshes, Jolt `MeshShape`, async rebuild after edits; character controller on voxels; footsteps from the voxel material | Walking, jumping and physics objects on dug terrain |
| **VX7** | RT preset: NoxRHI AABB BLAS (Vulkan, D3D12), a BLAS per chunk (refit on edits), procedural-candidate handling in the shared ray-query code (one brick-hit function), the path tracer; secondary rays on coarser mips | RT shadows, AO, reflections, GI, DDGI, ReSTIR and the path tracer see voxels; RT preset frame time measured |
| VX8 | Later: per-object voxel grids (dynamic, 2.5-5 cm), connectivity -> falling pieces, a custom Jolt voxel shape if destruction needs it | -- |
| VX9 | Later: glTF -> voxels at import (OpenVDB mesh-to-volume for the inside, colours from the glTF textures matched to the palette) | -- |

## 8. Tests

- Offline (NoxToolsTest): the `.vox` reader on hand-built files (every chunk type, broken sizes, the scene graph's rotations); chunk
  encode / decode round trips; brush application against a CPU reference; a CPU DDA reference against the shader's rules (same voxels
  hit for a grid of rays); greedy meshing (watertight, no T-junction gaps in collision); generator determinism (same seed, same
  chunk; the same chunk hash on Windows and Linux builds with the SSE4.1 cap).
- GPU (the user): each phase's "done when", frame times in UIDemo-style summaries (voxel pass, shadows, uploads), validation clean on
  Vulkan and D3D12.

## 9. Open questions

- Mesh first, voxels second in the depth order, or the reverse (VX2 measures both).
- Chunk size 64^3 vs 32^3 (finer streaming and edits vs fewer draws and tables).
- FastNoise2 capped at SSE4.1: identical chunk hashes between a Windows (MSVC) and a Linux (GCC / Clang) build (§3a; measured
  identical across SSE2 / SSE4.1 on one machine, AVX2 / AVX-512 differ).
- How far the per-voxel AO and the renderer's AO overlap (VX2 look test).
