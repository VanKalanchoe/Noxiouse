# Voxels -- Plan (2026-11)

Status: **VX0 built and user-tested (2026-11-27): `.vox` models import as meshes** (§4; room, cars and the knight look right); VX1
onward planned. 2026-11-28: binary greedy meshing v2 chosen for world chunks (collision; the block preset's mesh candidate).
**VX1 built (2026-11-28, offline only -- NoxToolsTest, 16 checks): the voxel world's CPU side** (§5 Built); chunk size decided: 64^3,
binary greedy meshing on 32^3 parts. VX2 next. Backlog item **VX** (`Backlog_2026.md`).

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

**Opt-in, nothing changes for other projects:** the voxel system runs only in a scene that has a voxel world (a `VoxelWorld`
component). Without one no voxel pass is recorded, no brick pool is allocated and no generation thread starts -- a realistic project
(Cyberpunk-like: meshes, materials, RT / GI) renders exactly as today, and both kinds of project live side by side. Voxel meshes from
`.vox` are ordinary meshes and need nothing either. The renderer changes are additions (a voxel pass, its use in the shadow / probe /
view passes, later AABBs in NoxRHI's BLAS and a brick-hit function in the ray-query code) that do nothing when a scene has no voxels.

---

## 1. Decisions

| Topic | Decision | Why |
|---|---|---|
| Voxel size | **A project setting, 5 cm to 1 m; 10 cm the default** (later per-object grids at 2.5-5 cm for props and debris, VX8). At **50 cm - 1 m** (Minecraft / Hytale-style blocks) the same brick map and voxel pass apply, and a material may have **block textures** (top / side / bottom, sampled at the hit point inside the voxel face) instead of one colour; chunk size is tuned with it (§9) | 10 cm: the proven tiny-voxel size (Teardown). At 2560x1440 / 90 degrees a pixel covers ~8 mm at 10 m and ~8 cm at 100 m: smaller voxels turn sub-pixel at play distances and cost memory (4x per halving: surfaces scale with area) for nothing visible |
| World structure | Chunks of **64^3 voxels (6.4 m)**, each 8x8x8 **bricks of 8^3** | Brick = 512 voxels: 512 bytes at one palette index each, plus a 64-byte occupancy mask; a chunk is the unit of generation, streaming, saving and collision |
| Voxel value | 1 byte: index into the chunk's palette (0 = air), up to 255 materials per chunk; a chunk needing more (edits, a colourful stamped `.vox`) first compacts its palette (unused entries dropped), then switches to 2-byte indices for that chunk only | Minecraft's sections store a palette + indices the same way (`PalettedContainer`, which also widens its indices); compresses very well |
| Vertical extent and column summaries | The world is endless horizontally and **-512 m to +512 m** vertically (project setting); per 64 x 64 voxel column a **summary**: lowest / highest surface voxel and the sky-visibility height | Chunks fully above or below the surface skip generation, sky visibility (below) is a lookup, far LOD starts from the column; cheap to keep up to date on edits |
| **Underground darkness in the raster preset** | **Sky visibility per voxel brick** (Minecraft's sky light, coarse): flood-filled from the open sky through air, updated around edits on worker threads, sampled by the voxel pass and multiplied into the ambient / irradiance-volume light of voxel and mesh pixels in voxel worlds; block lights (torches) are ordinary point lights | The irradiance volume and probes were captured before the player dug: without it a fresh cave is lit like the outdoors. In the RT preset GI darkens caves itself; sky visibility then only guards probe-lit passes |
| Rendering of the world | **Two candidates prototyped in VX2, the faster at 1440p kept:** (a) raster the boxes of the visible non-empty bricks (two-phase occlusion culling as the meshlets), DDA inside the brick in the pixel shader, conservative depth (`SV_DepthGreaterEqual` with reverse-Z); (b) one compute primary ray per pixel through chunk -> brick -> voxel, stopping at the first hit, depth-tested against the meshes. Both output `GBufferOutput` | Cost per pixel, not per voxel; edits only write bytes (no remesh); LOD from brick mips; the G-buffer keeps every later pass unchanged. (a) can overdraw at grazing angles (a pixel stepping through several mostly empty boxes, each discarded); (b) has no overdraw but no hardware depth rejection -- measure, do not guess |
| Rendering of `.vox` props | Meshes: visible faces merged (greedy, per palette entry) into a `StaticMesh`; colours and materials from palette textures | Static art needs no voxel pass; the mesh path already has everything |
| Collision of the world | **Binary greedy meshing (v2)** per chunk -- solid columns as 64-bit masks, face culling and merging with bit operations (cgerikj's algorithm, MIT, written here) -- made **watertight** (a vertex inserted wherever a neighbouring rectangle's corner lies on an edge: no T-junction gaps), Jolt `MeshShape`, rebuilt off the main thread after every edit | Rebuilt after each dig or build, so meshing speed is the budget: the bitwise mesher meshes a chunk in microseconds where a cell-by-cell greedy mesher takes milliseconds. T-junction gaps catch characters and leak rays. A custom Jolt voxel shape only if destruction at scale needs it (VX8) |
| **Block-style worlds (50 cm - 1 m)** | The same brick map, generator, edits and collision; for drawing, a **third candidate measured in VX2 for the block preset: chunk meshes** from the same binary greedy mesher (merged only across faces of one block type and light level; block textures tiled over a merged face from a texture array), drawn as ordinary meshes | At block size a chunk's mesh is small (Minecraft, Hytale, Vintage Story mesh their chunks), and meshes get everything the renderer has -- visibility buffer, meshlets, shadow maps, and triangle BLASes for ray tracing without VX7's AABB work. At 10 cm with per-voxel colours meshing stays out (triangle counts explode); the tiny-voxel preset keeps the DDA pass |
| Edits | **Brush operations** (shape, size, add / subtract / paint / smooth, noise on the edge), applied by the authority, sent over the network as operations; **evaluated in integer voxel coordinates** (shape tests in integers / fixed point, edge noise from strict-FP FastNoise2) so the client's prediction and the server give the same voxels; after an edit the touched bricks' hash is compared and the server sends those bricks on a mismatch; a chunk's edit log is **compacted** into its changed bricks once it grows larger than them | Fun and fast to dig (§2), small to send, the same result everywhere |
| Generator versions | A world stores the generator asset's **version and hash**; an edited generator never changes existing worlds silently (they keep generating with the version they were made with, or an explicit Regenerate migrates them) | Edits are stored relative to the generated terrain: a changed generator would leave them floating or buried |
| Chunk and brick index | **ankerl::unordered_dense** maps (chunk position packed into 64 bits -> chunk; dirty sets), from the first line of code | Millions of lookups per second in generation, edits, streaming and meshing; far faster than `std::unordered_map`; no rework later |
| World generation in multiplayer | **Every machine generates; the server sends only edits** -- FastNoise2 built with **`FASTNOISE2_STRICT_FP=ON`** (every SIMD level bit-identical, each CPU keeps its fastest level; §3a), a hash per generated chunk checked against the server's, a full chunk sent only on a mismatch or for chunks clients cannot generate | Sending every chunk costs megabytes per second per player at 10 cm (§3a); generating everywhere costs a little CPU |
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
| **FastNoise2** | MIT | World generation on the CPU: SIMD node trees filled into whole 3D grids (`GenUniformGrid3D`), designed in its NoiseTool and stored as the encoded tree string in a generator asset. Built with `FASTNOISE2_STRICT_FP=ON` (§3a) |
| **zstd** (LZ4 if load speed wins) | BSD | Chunks on disk, streamed cells, saved edit deltas (palette-indexed bricks compress well) |
| ankerl **unordered_dense** | MIT | The CPU chunk / brick index and dirty sets (position packed into 64 bits -> slot), used from the start |

Added as submodules for VX1 (2026-11-28; nothing downloaded at configure time): FastNoise2 (pinned to the commit §3a measured) with its
**FastSIMD** (CPM's `CPM_FastSIMD_SOURCE` points at `NoxCore/vendors/FastSIMD`), zstd 1.5.7 (static, no programs), unordered_dense 5.3.3.

Already in the engine: **OffsetAllocator** (the GPU brick pool), **Jolt** (collision), **meshoptimizer** (simplifying collision /
prop meshes), **tinygltf** + editor **OpenVDB** (glTF -> voxels, VX9), **xxhash** (content hashes), **NoxTasks / taskflow**
(generation, meshing, compression in parallel), **Tracy**. Written here (the user's decision): the **`.vox` reader** (not ogt_vox),
the brick map, the DDA shaders, the meshers: plain greedy for `.vox` props at import (VX0, built) and **binary greedy meshing v2** for
world chunks (VX4; cgerikj/binary-greedy-meshing's algorithm, MIT, reimplemented on the chunk format).

## 3a. Determinism and what multiplayer sends (measured 2026-11-26)

**FastNoise2 across instruction sets** (FastNoise2 c35d604, GCC, a CPU with AVX-512; Simplex, SuperSimplex, Perlin, Value, Cellular,
5-6 octave FBm and Ridged, domain-warped FBm; 64^3 grids at the origin, at 640 voxels and at one million voxels out, compared bit for
bit): **SSE2 and SSE4.1 give identical output; AVX2 and AVX-512 differ from them in almost every sample** (by 1e-7 to 1e-4, fused
multiply-add rounding; Cellular differs between AVX2 and AVX-512 too). FastNoise2 picks the best level of the CPU at run time, so by
default two players' machines generate slightly different worlds: a voxel whose density lies within 1e-4 of the surface threshold can
be solid on one and air on the other (FastNoise2's FAQ says the same). Its two remedies, measured:

| One 64^3 chunk, domain-warped 5-octave simplex FBm, one core | SSE2 | SSE4.1 | AVX2 | AVX-512 | Identical across levels |
|---|---|---|---|---|---|
| Default build | 17.2 ms | 13.1 ms | 7.4 ms | 3.9 ms | No (AVX2 / AVX-512 differ) |
| Default build, every node capped at SSE4.1 | -- | 13.1 ms | (13.1) | (13.1) | Yes |
| **`FASTNOISE2_STRICT_FP=ON`** | 16.9 ms | 14.5 ms | **8.1 ms** | **5.5 ms** | **Yes: all eight trees, all three offsets** |

**Chosen: `FASTNOISE2_STRICT_FP=ON`** -- every CPU keeps its fastest level and the output is bit-identical anyway; it costs ~9 %
on AVX2 and ~40 % on AVX-512 against the default build, and is still 1.6x (AVX2) to 2.4x (AVX-512) faster than capping at SSE4.1.
About 1,000 chunks a second on 8 AVX2 threads; chunks fully above or below the surface skip most of it. Still to verify (VX1 test):
the same chunk hashes on a Windows MSVC build and a Linux GCC / Clang build (no `-ffast-math` / `/fp:fast` around the generator),
and on ARM (NEON) -- a machine whose hash differs takes chunks from the server.

**What Minecraft sends:** the server generates (single player runs an integrated server too) and sends whole chunks -- each 16^3
section as a block palette + bit-packed indices, plus heightmaps, block entities and light, zlib-compressed above a size threshold;
afterwards only block changes. Clients never generate terrain. Hytale's internals are not documented publicly; it is
server-authoritative with chunks streamed by the server in the same family. At 1 m blocks this is cheap. **At 10 cm it is not:** a
64^3 chunk is 262 KB raw, roughly 2-20 KB after palette packing and zstd; a player running at 10 m/s with a 256 m view distance meets
on the order of hundreds of new near-surface chunks a second -- megabytes per second per player before LOD.

**The plan's model:** the world is the seed + the generator asset + the edits. Every machine generates chunks itself (strict-FP
FastNoise2); the server is authoritative for edits, which travel as brush operations (§2) and are kept per chunk as an edit log / brick
deltas (also what a save game and a late joiner receive); after generating a chunk a client compares its hash (xxhash of the
generated bricks) with the server's and asks for the chunk when they differ (an ARM client, a driver or compiler surprise) -- the
server then sends it compressed. Far chunks can come at a coarser LOD when they must be sent. Late joiners get the edit logs of the
chunks around them, not the chunks.

## 3b. Cheating: a public and a secret layer

Whatever a client generates, a hacked client can draw (X-ray, ESP): with client-side generation it would know every ore, cave and
hidden room, and seed crackers recover a world's seed from terrain a player has seen, so a secret seed protects nothing. (Server-sent
chunks have the same problem: Minecraft servers answer it with anti-xray, e.g. Paper's, replacing hidden ores with stone until they
are exposed -- impossible for content clients generate themselves.) The game state itself stays safe as long as the server is
authoritative: movement is checked against the server's own world (NoxNet: server authority, prediction, reconciliation), edits are
brush operations the server validates (tool, reach, rate, cost) and resources are counted from the voxels the server removed; the
chunk hash is a consistency check, never trusted; other players behind walls are held back by NoxNet's relevancy.

**Two layers in the generator asset:**

- **Public layer** (every machine): terrain shape, surface and bulk materials (rock, dirt, sand) -- the megabytes not worth sending;
  seeing it through walls gives nothing. Its hash is the chunk hash (§3a).
- **Secret layer** (server only): ores and valuable materials, loot, hidden caves and rooms, buried structures. The client holds the
  public layer there (secret caves are solid rock to it, so no hole, light or sound gives them away); the server sends **overrides**
  (voxel index replacements, air included) for secret voxels that are **exposed** (a neighbour turned to air) and **within reach**.

| Concern | Handling |
|---|---|
| Delay on reveal (one round trip, 50-150 ms: ore popping in late) | The server reveals ahead: secret voxels within ~1-2 m of exposed surfaces near the player -- digging feels instant, and that thin shell is all an X-ray shows (Paper's anti-xray reveals a radius the same way) |
| Exposed ore across a big cave | Exposed secret voxels revealed only within ~32-64 m of the player (a small pop when coming closer) |
| Ores following public clues (depth, rock type) | Player knowledge, not a hack: only exact positions are hidden |
| Server bookkeeping | Per player, the set of secret bricks already sent (unordered_dense); edit logs and saves cover both layers |
| Single player / co-op | The local authority reveals at once: no delay, no cost |
| Server setting **Hide underground content** | On by default in multiplayer; off (a friends' server): everything generated publicly, nothing delayed |

The voxel data, rendering and physics do not change: a client brick is the public voxels plus the overrides it received.

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
- **LOD:** by **downsampling the voxels** (2x and 4x coarser models, each meshed the same way), not by triangle simplification, which
  melts the blocky look.
- **Done when:** the user's MagicaVoxel models import, place and render in raster and RT presets; a `.vox` with several models
  becomes a prefab with the right transforms.

**As built (VX0, 2026-11-26):**

- Reader `NoxCore/src/NoxCore/Voxel/VoxReader.*` (all chunks above plus 0.98 `MATT`; models up to 1024 per axis; an animated shape's
  frame 0; files without a scene graph place their first model -- the others are animation frames), mesher `VoxMesher.*` (greedy per
  palette entry and plane, the glass rule: a face towards an empty cell, or from an opaque voxel towards a transparent one), importer
  `NoxCore/src/NoxCore/Asset/VoxImporter.*`: a `.vox` is a `MeshSource` cooked by `MeshImporter::CookMesh` / `CookSplitMeshes`,
  which take the glTF parser's output from it -- so hashing, hot reload, cluster LOD and packaging are the glTF path's.
- **Multi-model scenes: Combine / Do Not Combine instead of a prefab** -- the glTF import's own choice (one mesh placed as in
  MagicaVoxel, or a mesh per model with its file layout); a root node moves the scene's pivot to the origin.
- **Pivot** option Bottom Center (default) / Center / File Origin; **Merge Faces** option; **Voxel Size** per import (`ImportScale` =
  world units per voxel, default 10 cm) until the project setting exists (VX1).
- **Palette textures** 256 x 1, one mip, uncompressed (Color sRGB; Surface R transparency / G roughness / B metallic; Emissive sRGB
  colour x strength / the brightest's, strength `_emit` x 2^(`_flux` + 1) x Emission Scale), rewritten only when changed; materials `_Palette` /
  `_PaletteGlass` written once (factors 1; glass Transmission 1 x the surface R, mean IOR) except Emissive / Emissive Strength (each cook). Glass, blend, media and cloud entries go to the glass submesh.
- **LOD: the engine's cluster LOD DAG, not voxel downsampling (yet)** -- it protects UV seams (= colour borders) and hard normal edges and
  has no sloppy fallback, so flat areas of one colour simplify losslessly; voxel downsampling waits until the cluster LOD is seen to
  melt the look.
- Found on the way and fixed: `ReimportAsset` deleted a `.ntex` that is its own source (palette textures, generated textures); importing
  a model again through the dialog made a second registry entry and kept the old cook (now the same asset is cooked again with the new
  settings, glTF too); importing a `.vox` from outside the project copied its whole folder (now only the file); NoxToolsTest's
  `fresnel` declared twice (did not compile).
- **After the user's first test (2026-11-26):** dragging a `.vox` onto the Content Browser did nothing -- a file dropped from the
  system (and one picked in the file dialog) arrives between frames, when no NoxUI context is current, and the import dialog returned
  silently (glTF drops too, since the NoxUI editor). It now opens in the UI layer's context, as the editor's other event dialogs.
- **Second test:** the knight's folder moved into `Models/` in Explorer kept its material and textures but lost its mesh (not listed):
  the move detection (`ScanAndRegisterNewAssets`, the Content Browser's Refresh) adopted only self-contained files. It now adopts a
  moved cooked mesh too and moves its source (`.vox` / `.gltf`) and material base path by the same step -- glTF model folders too.
- **Third test:** palette edits did not show (the user had saved over another copy, but two real gaps were behind it as well): the
  palette textures were renamed over, which the watcher reports as the `.tmp` only -- now written in place; and `ReimportAsset`
  swapped a texture without telling the renderer, whose slot cache kept the old image -- now swapped as the streamer swaps one.
  Emission was weak (room.vox lamps 4): now `_emit` x 2^(`_flux` + 1) (the Blender MagicaVoxel add-on's mapping; 8 for those lamps)
  x a new **Emission Scale** import option; the materials' Emissive / Emissive Strength follow the palette on every cook (0 without
  Emit entries, so meshes without lamps are no emissive lights). The black path tracer is not explained by the import (the emissive
  sampling is guarded against zero-power instances) -- isolation asked of the user.
- Tests: `Tests/VoxTests.inl` (NoxToolsTest, 31 checks, also compiled standalone with ASan / UBSan here); the 44 files of
  github.com/ephtracy/voxel-model and the 15 sample files of MagicaVoxel 0.99.7 read and meshed with closed surfaces. Not run here:
  the cook inside the engine, the editor dialog, rendering (no GPU in the cloud container) -- the user's test.

## 5. The brick map (VX1-VX2)

**CPU side** (`NoxCore/src/NoxCore/Voxel/`):

- `VoxelChunk`: 64^3, an array of 512 brick slots (empty / uniform with one index / full brick of 512 bytes), the chunk palette
  (index -> material entry), a revision. Encoded for disk: brick masks + palette indices bit-packed to the palette's size, then zstd.
- `VoxelWorld` (a scene component / world-level system): the chunk map (unordered_dense), generation requests, edit application,
  dirty tracking, saving; world partition cells own the chunks inside them.
- `VoxelGenerator` asset: FastNoise2 trees (encoded strings) in two layers (§3b) -- **public** (shape, surface material by height /
  slope, bulk materials) and **secret** (ores, loot, hidden caves and rooms, buried structures; server only in multiplayer) --
  evaluated per chunk on NoxTasks; deterministic from the world seed.
- Palette: a project-wide **voxel material table** (name, base colour or block textures, roughness, metallic, emission, hardness,
  footstep surface, physical material); chunk palettes index into it. Stamping a `.vox` into the world maps each of its palette
  entries to the nearest material of the table, or adds entries (an import option).

**GPU side** (`Renderer`, pass in `Passes/VoxelPasses.cpp`, shader `Voxel.slang`):

- **Brick pool:** one large buffer of 512-byte bricks + occupancy masks, sub-allocated with OffsetAllocator; an upload budget per
  frame (MB), uploads from the dirty list.
- **Chunk table:** per resident chunk its origin, LOD, 512 brick pointers (pool offset or uniform value), palette offset.
- **Brick list (candidate a):** per frame, a compute pass culls the resident non-empty bricks that have at least one exposed face
  against the frustum and the Hi-Z in two phases (last frame's visible bricks first, then the rest against the new Hi-Z, as the
  meshlets are culled: nothing pops) and writes an indirect draw of their boxes (front to back by chunk).
- **Primary rays (candidate b):** a compute pass, one ray per pixel from the camera through the chunk table, the brick pointers and the
  occupancy masks (empty bricks and chunks skipped whole), depth-tested against the meshes' depth, writing the same outputs.
- **Voxel G-buffer pass:** draws the brick boxes (back faces culled; the camera inside a brick handled by drawing that brick's box
  inside out), the pixel shader starts the ray at the box entry point (or the camera) and steps the 8^3 voxels with a DDA using the
  occupancy mask (64-bit words: whole rows skipped), at the first solid voxel writes `GBufferOutput` (base colour, roughness, metallic,
  emission from the material table; the hit face's normal; entity ID for picking: the world's entity; motion vectors from the camera
  for the static world) and conservative depth; misses `discard`. It runs with the visibility buffer's depth so meshes and voxels
  occlude each other (order: meshes' visibility buffer + resolve, then voxels depth-tested against them, or the reverse -- measured).
- **Ambient occlusion per voxel** (optional, cheap and very "voxel"): corner AO from the neighbours, as Minecraft-style games do, on top
  of the renderer's AO.

**Built (VX1, 2026-11-28; offline, NoxToolsTest):** `NoxCore/src/NoxCore/Voxel/` -- `VoxelTypes.h` (64^3 chunks of 8^3 bricks, 16-bit
material ids, 64-bit chunk keys, `MeshPartSize` 32), `VoxelChunk` (empty / uniform / full bricks; palette with compaction and the 2-byte
fallback; occupancy masks word z / bit y * 8 + x; XXH3 hashes over material ids per brick and chunk; encoded as brick kinds + palette +
bit-packed indices + zstd: a 10 cm terrain chunk ~15 KB, an empty one 33 bytes; `Decode` checks everything), `VoxelMaterialTable` (YAML
with fixed ids; colour, roughness, metallic, emission, hardness, footstep surface, block textures), `VoxelGenerator` (public: 2D height
tree, 3D cave tree below the surface, surface / subsurface / bulk by depth; secret: ore pockets in the bulk; metres as noise input;
settings with version + binary hash; `MakeDefaultTerrain`), `VoxelWorld` (unordered_dense chunk index, columns' heights shared,
`LoadRegion` on worker threads: 576 chunks = 51 x 51 m of 10 cm terrain in ~0.4 s, ~37 MB; vertical limits), `VoxelStamp` (a `.vox`
scene into the world, MagicaVoxel placement, materials added or nearest). Measured and fixed on the way: the default cave threshold
0.45 carved 20% of the underground (the cave noise measured over +-5 km), now 0.65 (~10%); depths in metres now round to voxels (3 m /
0.1 m is 29.99999 in floating point: a 29-voxel subsurface), heights still floor; FastSIMD builds SSE2 / SSE4.1 / AVX2 / AVX-512 on x86 --
no scalar -- and the four give bit-identical chunks (tested; §3a). Real files stamped exactly (every voxel its own cell): room.vox
82,536 voxels in 11 ms, monu1.vox 156,942 in 20 ms, cars, the knight. Generation runs on `std::thread` workers for now; VX6 moves it onto
the job system with streaming.

## 6. Raster preset coverage (VX3)

The voxel draw (box list + DDA, depth only) in every pass that draws geometry itself: sun shadow cascades (cascade caching invalidated
by edits inside a cascade's bounds), the local-light shadow atlas, the irradiance volume and reflection probe captures, extra views
(ortho, previews), editor picking and the selection outline. Screen-space passes, deferred lighting, TAA / DLSS / NRD, fog, outlines
and post effects need nothing.

## 7. Phases

| Phase | What | Done when |
|---|---|---|
| **VX0** | `.vox` reader + import as meshes (Combine / Do Not Combine), palette materials (§4) -- **built, user-tested 2026-11-27** | The user's models in the editor, both presets |
| **VX1** | Brick map CPU side: chunks, bricks, palette (compaction, 2-byte fallback), voxel material table, encoding + zstd, FastNoise2 generator asset (public / secret layers, version + hash), a `.vox` model stamped into the world -- **built 2026-11-28** (§5 Built) | A generated 10 cm terrain + a stamped `.vox` in memory; offline tests |
| **VX2** | GPU brick pool + voxel G-buffer output, **both primary-visibility candidates** (brick boxes with two-phase culling; compute primary rays), raster lighting; for the block preset also the third candidate, chunk meshes from binary greedy meshing (§1) | Terrain and the stamped model lit in the raster preset; both candidates measured at 1440p, one kept |
| **VX3** | Raster preset coverage (§6), sky visibility and column summaries (underground darkness) | Voxel shadows (sun + local lights), probes, extra views, picking; a cave dark in the raster preset |
| **VX4** | Collision: chunk meshes from binary greedy meshing v2 (coplanar merges only, watertight: no T-junctions, no simplification that opens gaps), Jolt `MeshShape`, async rebuild after edits; character controller on voxels; footsteps from the voxel material | Walking, jumping and physics objects on the terrain (needed before digging can be tried in Play) |
| **VX5** | Brushes (§2): integer-space edits, dirty bricks, uploads, collision rebuilds, debris VFX, C# API (`VoxelWorld.Apply(brush)`, queries: raycast to a voxel, material at a point), edit logs with compaction saved per cell, generator version in the world, brush ops over the network validated by the server with brick-hash checks; the secret layer and its reveals (§3b) | Digging and building in Play, saved and loaded, in multiplayer; an X-ray client sees only revealed ores |
| **VX6** | Streaming + LOD: chunks by distance through world partition, brick mips (2x2x2: the most common non-air index), LOD per distance, upload budget, an endless generated world | Flying across an endless world at the frame-rate target |
| **VX7** | RT preset: NoxRHI AABB BLAS (Vulkan, D3D12), a BLAS per chunk (refit on edits), procedural-candidate handling in the shared ray-query code (one brick-hit function), the path tracer; secondary rays on coarser mips | RT shadows, AO, reflections, GI, DDGI, ReSTIR and the path tracer see voxels; RT preset frame time measured |
| VX8 | Later: per-object voxel grids (dynamic, 2.5-5 cm), connectivity -> falling pieces, a custom Jolt voxel shape if destruction needs it | -- |
| VX9 | Later: glTF -> voxels at import (OpenVDB mesh-to-volume for the inside, colours from the glTF textures matched to the palette) | -- |

Not planned now: water and other fluids, falling sand / gravity voxels (later, if the game wants them).

## 8. Tests

- Offline (NoxToolsTest): the `.vox` reader on hand-built files (every chunk type, broken sizes, the scene graph's rotations); chunk
  encode / decode round trips; brush application against a CPU reference (integer evaluation: the same voxels from two different orders of operations on the
  same brush list); palette compaction and the 2-byte fallback; sky visibility after carving a cave; a CPU DDA reference against the shader's rules (same voxels
  hit for a grid of rays); binary greedy meshing against the plain greedy mesher on random chunks (same visible faces and area) and
  watertight (every directed edge matched by its reverse: no T-junction gaps); generator determinism (same seed, same
  chunk; the same chunk hash on Windows and Linux builds with strict FP).
- GPU (the user): each phase's "done when", frame times in UIDemo-style summaries (voxel pass, shadows, uploads), validation clean on
  Vulkan and D3D12.

## 9. Open questions

- Mesh first, voxels second in the depth order, or the reverse (VX2 measures both).
- ~~Chunk size~~ -- decided 2026-11-28 (the user): **64^3 chunks** (8^3 bricks of 8^3, as planned), binary greedy meshing on **32^3
  parts** (32 + 2 border = 34 bits in a 64-bit column word; 8 parts per chunk, rebuilt only where an edit lands). 62^3 was ruled out
  (it does not divide into 8^3 bricks); 32^3 chunks would mean 8x the chunks, tables and draws.
- The block preset (50 cm - 1 m): chunk meshes or the DDA pass (VX2 measures; §1 Block-style worlds).
- `.vox` props (VX0): the same T-junction removal for their merged faces, so ray-traced effects see them watertight (no visible
  problem reported on room / cars / knight).
- Primary visibility: brick boxes or compute primary rays (VX2 measures both).
- Strict-FP FastNoise2: identical chunk hashes between a Windows (MSVC) and a Linux (GCC / Clang) build, and on ARM (§3a; measured
  identical across SSE2 / SSE4.1 / AVX2 / AVX-512 on one machine).
- How far the per-voxel AO and the renderer's AO overlap (VX2 look test).
