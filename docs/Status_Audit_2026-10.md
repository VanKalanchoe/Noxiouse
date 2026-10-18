# Status audit -- where the docs say we are, what the code says, what is missing (2026-10-18)

> Item 1 of `Future.md`: "check first all docs md files where we are currently, what's missing". Every `.md` in the repo
> was read and its claims checked against the code at commit `509dd11` ("before unleashing claude"). This file is the
> snapshot; `Backlog_2026.md` stays the living index (the Future.md items are now in it as F1-F14).

## 1. The docs, one by one

| Doc | Last touched | What it covers | State it describes | Stale or wrong (checked against code) |
|---|---|---|---|---|
| `README.md`, `PROJECT_CONTEXT.md`, `GEMINI.md`, `architecture.md` | 2026-09-11 | Overview, NRI rules, visibility-buffer migration record | Phases 0-7 of the VisBuffer migration done | **Stale overview.** Says Box2D is "the" physics engine and Taskflow is vendored but unused. Today: Jolt (3D) next to Box2D, Taskflow drives the frame (`JobSystem`), C# scripting through Coral, render graph, GPU scene. The NRI zero-leak and Slang rules are still current. |
| `Engine_Architecture_Plan_2026.md` | 2026-09-25 | Threading, render graph, GPU scene, memory, async IO, streaming, profiling | Phases 0-5 and 6a built; 6b-1 (cluster DAG + GPU cut) built, 6b streaming paused, RT fallback LOD reverted; 6c (world streaming) and 7 not started | §3b "current state" is still the *pre-plan* snapshot (lists "single-threaded main loop", "no profiler", which phases 0-2 removed). **D9 "Editor vs runtime asset managers"** is still open, and the code shows why: `RuntimeAssetManager` is an empty class (see F7). |
| `RayTracing_Architecture_Plan_2026.md` | 2026-09-12 | Lighting, GI, denoising, DLSS/RR, path tracer | Phase 5 Track B (ReSTIR GI) "next up", phases 6-8 not started | **Behind the code.** ReSTIR GI, ReSTIR DI and ReSTIR PT passes and shaders exist (`ReSTIRPasses.cpp`, `RTXDI/DI`, `RTXDI/GI`, `RTXDI/PT`), RTAO exists (`RTAO.slang`, phase 7 part 1) and SSAO was added. **Not in code:** LTC area lights, volumetric froxels, Reflex. The progress table (§6) needs a refresh. |
| `Animation_Graph_Architecture_Plan_2026.md` | 2026-09-26 | Node-graph core, animation graph, character controller, UE5-style import | Steps 1-7 built (7b/7c untested in Play), step 8 (audio graph) future; import stages A/B done, C became the prefab plan | Node types in code: `Clip`, `Blend2D`, `GetParameter`, `StateMachine`, `Output`. **Missing for F3:** no 1D blend space, no layered blend / per-bone mask, no additive, no sync groups or notifies, no root motion, no sequencer/timeline, no way to edit or author clips. "Step 8 audio graph" is the only planned phase left. |
| `Prefab_Architecture_Plan_2026.md` | 2026-09-26 | Prefab asset, instances, overrides, nested prefabs, variants | P1-P6c built and tested | Open items section 8 (P1, P4-P7) match the backlog. |
| `Units_And_World_Tools_Plan_2026.md` | 2026-09-28 | cm world, grid, snap, ortho views, measure, physics unit boundary | U1-U6 built; U2 confirmed by the user; U3-U6 untested | Consistent with code (`WorldUnitsPerMeter` default 100). Side effects worth knowing: the ortho camera sits a fixed 5000 units back (not unit-scaled), which matters for anything that places geometry by NDC depth (see F9). |
| `Scripting_Architecture_and_Status.md` | 2026-09-22 | C#/.NET through Coral, lifecycle, hot reload, exposed fields | Runtime integrated; "Next" list of 9 items | Next-list item 6 is half done: `AddForce` / `AddImpulse` / `SetLinearVelocity` exist (`InternalCalls.cs`), `FixedUpdate` does not. No job-system or chunk/voxel API is exposed (relevant to F11). |
| `Backlog_2026.md` | 2026-09-28 | Index of everything open | N4 (units) in progress; P/C/I/R/E items | Kept. R4 (stats overlay) is answered by this audit (F9). |
| `Future.md` | 2026-10-18 | The user's new wish list (13 items) | -- | Mapped below. |

## 2. Features that exist but no doc tracks

- **Editor:** world grid (Godot port), snap toolbar, measure tool, reference figure, ortho views, fly camera, frame
  selected (F / double-click), outliner folders, physics collider wireframes, prefab mode.
- **Rendering:** SSAO + blur, RTAO, auto exposure, NRD front end, the RTXPT-based path tracer split into
  `PathTracer/` modules, ReSTIR DI/GI/PT, compute skinning and animated skinned BLAS for shadows (commit `e6670d0`;
  the Engine plan §9 still lists the animated BLAS as deferred).
- **Assets:** texture streaming with GPU mip feedback, cluster-LOD cook, async upload manager.

## 3. Things nobody has (and the Future.md items that ask for them)

| Missing today | Code evidence | Future.md |
|---|---|---|
| Undo / redo | no command history anywhere in `NoxEditor/src` | 12 |
| Primitive shapes | the only cube/sphere are hand-imported glTFs in `Facerun` | 10 |
| More than one viewport | one `Viewport` panel, one `EditorCamera`; the renderer has one `CullView` ("A second real view" in Engine plan §9 waits for exactly this) | 5 |
| Procedural sky | `Skybox.slang` samples the HDR environment cubemap only; no atmosphere model in code | 6 |
| Runtime player and runtime asset manager | `RuntimeAssetManager` is an empty class; only `NoxEditor` is an executable; `Scene.Instantiate` resolves prefabs through the editor registry (P6) | 7 |
| Packaging / distribution build | no pack format, no cook-all step, no runtime target | 8 |
| Material editor window, mesh/material thumbnails | `ThumbnailCache` shows textures and a material's base-color texture only; materials are edited in the Properties panel | 2 |
| Content browser folder tree and search | flat grid with a breadcrumb and `<` back button | 2 |
| Animation authoring (sequencer, clip editing) | see the animation row above | 3 |
| VFX / particles / volumetric clouds | nothing in code | 13 |

## 4. Future.md items: where each one stands

| # | Item | Where it stands | Size | What happens with it now |
|---|---|---|---|---|
| F1 | Docs audit | this file | -- | done |
| F2 | Modern content browser | flat grid, no tree, no search, texture-only thumbnails | M (UI) + L (rendered thumbnails) | **first part built this round**: folder tree, clickable path, search, type filter, tile size, Material Editor window; rendered sphere / mesh thumbnails planned in `Editor_Features_Plan_2026-10.md` §2 |
| F3 | Animation graph: player node, sequencer, editing | five node types, no authoring | L | **first part built this round**: Clip node as a Sequence Player (Speed pin, Start Position), BlendSpace1D node; notifies, layered blend, montages, root motion, preview window and a Sequencer planned in `Editor_Features_Plan_2026-10.md` §3 |
| F4 | Hybrid renderer near path-tracer quality, RT-only fallbacks, DLSS edge jitter, robust transparency/transmission | hybrid renderer works; DLSS edge jitter cause found (below) | XL | plan in `Hybrid_Renderer_Plan_2026-10.md`; root cause of the jitter written down there |
| F5 | Up to 4 viewports | single viewport and camera | L (renderer needs per-view resources) | plan in `Editor_Features_Plan_2026-10.md` |
| F6 | Procedural sky (Hosek-Wilkie / Hillaire) | HDR IBL only | M | plan in `Hybrid_Renderer_Plan_2026-10.md` |
| F7 | Runtime asset manager + runtime project | empty class | L | plan in `Runtime_And_Packaging_Plan_2026-10.md` |
| F8 | How to package a distribution build | nothing | L | answered in `Runtime_And_Packaging_Plan_2026-10.md` |
| F9 | Stats overlay text broken | cause found | S | **fixed this round**: drawn in screen space (below) |
| F10 | Basic shapes | none | S | **built this round** |
| F11 | Minecraft clone: script or C++? | no voxel/job API exposed | -- | answered in `Runtime_And_Packaging_Plan_2026-10.md` §4 |
| F12 | Undo / redo | none | M | **built this round** |
| F13 | VFX, particles, clouds | none | L | answered and planned in `Hybrid_Renderer_Plan_2026-10.md` §6 |
| F14 | Unregister a mesh when its entity is deleted? (added 2026-10-18) | registry never shrinks; files deleted outside the editor leave entries behind | S | answered below; stale-entry clean-up **built** |

### F14 answer: deleting an entity should not unregister its mesh

The registry is the list of the project's **asset files**, not of what a scene uses -- like UE's Content Browser or
Unity's AssetDatabase. Unregistering a mesh because one entity using it was deleted would break:
- every other entity, scene and prefab that uses the same mesh (they reference it by handle),
- undo: Ctrl+Z brings the entity back with the same handle, which must still resolve,
- dragging the mesh into the scene again from the Content Browser.

The memory side is already handled: `EditorLayer::UnloadUnusedAssets` (mark and sweep) unloads meshes, materials and
textures no live scene references, and frees their GPU memory a few frames later. An unused asset costs one registry
line and its files on disk, nothing at run time.

What *is* pollution, and what to do about it:
1. **Entries whose files are gone** (deleted in the file explorer, leftovers of older imports): **built** -- when the
   registry loads, entries whose file and source file both no longer exist are removed (logged; the previous registry is
   kept as `.bak`; nothing is removed when more than half of the registry is missing, which means a moved asset folder).
2. **Deleting an asset on purpose**: Content Browser right-click > Delete, with a reference check (UE shows what uses the
   asset and offers Force Delete / Replace References): planned as B3 in `Editor_Features_Plan_2026-10.md`.
3. **Finding unused assets**: a report of registered assets no scene or prefab references (UE: the Reference Viewer /
   size map), so they can be deleted on purpose: planned with B3.

### F9 root cause (stats overlay)

The overlay was drawn as world-space quads on a plane placed at NDC depth 0.5 in front of the camera and then sent
through the camera's view-projection like any other 2D content. That ties its look to the camera:

1. **Ortho views (U3, 2026-09-27):** the ortho camera sits 5000 units back with near 0.1 / far 10000, so depth 0.5
   is ~5000 units away: the overlay plane lands at the focal point, *inside* the scene. Geometry in front of it
   hides parts of the panels and text, and the world grid (same depth range) runs through them.
2. **Float precision:** each glyph corner is a world position next to the camera (`camera + tiny offset`), built in
   float on the CPU and multiplied by `view` (which subtracts the camera position again) in the shader. The same
   cancellation the 2026-09-25 jitter fix removed from the sky shows up here: glyph corners snap to the float grid
   around the camera position, so text wobbles and distorts as the camera moves, worse the farther the camera is
   from the origin.
3. **Depth test against the scene:** anything closer than 2x near clip occluded the overlay.

Fix: `Renderer2D` has a screen-space batch (quads and text in output pixels, its own per-frame uniform buffer with
a pixel orthographic projection, depth 1.0 so the scene never occludes it) and the overlay draws into it. No camera
math is left in `StatsOverlay`.

### F4 finding (DLSS edge jitter)

`Entity/Depth Blit` (`UpscalePasses.cpp`) copies the *jittered* render-resolution entity-ID and depth buffers to
display resolution with a nearest blit. Everything that reads `EntityHi` / `DepthHi` therefore moves by the
sub-pixel camera jitter every frame: the selection outline (edge detection on `EntityHi`), picking, and the 2D
overlay's depth test (grid, gizmo lines, collider wireframes). With DLSS off there is no jitter, which is why it
only shows with DLSS on. The fix options are in `Hybrid_Renderer_Plan_2026-10.md` §3.

## 5. Recommended order after this round

1. Test what was built this round (all untested): F9, F10, F12, the content browser / Material Editor (F2) and the two
   animation nodes (F3). What to try is listed in `Editor_Features_Plan_2026-10.md` §1.
2. F4 DLSS outline jitter (small, self-contained, highest annoyance).
3. F7/F8 runtime player: it unblocks P6 (prefab lookup in exported games) and gives a clean place to measure
   performance without the editor.
4. F6 sky (self-contained renderer work).
5. F5 multi-viewport (needs the "second real view" renderer work from the Engine plan §9).
6. F3 animation authoring, then F13 VFX.
