# Backlog -- one list of everything that is open (kept up to date; started 2026-09-26)

> How to use: pick an item, say its number/name, and it is built the way the plans say (phase gates: the user builds and tests, nothing is
> built by me). When an item is done, move it to "Done" with the date. Details live in the plan doc named in each line -- this file is
> only the index. Newest decisions win over older notes.

## Now (in progress)

| # | Item | Notes |
|---|---|---|
| N3 | Prefab leftovers: hierarchy **Create Prefab** context menu (P2) and clearing script references that point outside the created prefab (P3) | **built 2026-09-26, untested**: right-click an entity -> Create Prefab (this entity, or the selection it is part of) into the folder the Content Browser shows, name in rename mode; script entity references that point outside the prefab are cleared in the file (logged), the scene keeps them |

## Prefabs (plan: `Prefab_Architecture_Plan_2026.md`, section 8)

| # | Item |
|---|---|
| P1 | **Apply of a nested instance's change into the OUTER prefab** (today Apply on the inner instance writes into the inner prefab) |
| P2 | Hierarchy right-click **Create Prefab...** menu (dragging into the Content Browser is the way today) |
| P3 | Clear script entity references that point outside the dragged set when a prefab is created (they keep their UUID) |
| P4 | Re-parenting a spawned entity is not tracked as an override; entities attached directly below an instance root also show as `+ Entity` |
| P5 | Overrides on a deleted instance entity are dropped with it (only its name is remembered) |
| P6 | Exported games need their own prefab lookup for `Scene.Instantiate` (it resolves paths through the editor asset registry) |
| P7 | Unpack of a nested instance / Unpack All; prefab thumbnails in the Content Browser; variants tinted differently |

## Characters, animation and gameplay (plan: `Animation_Graph_Architecture_Plan_2026.md`)

| # | Item |
|---|---|
| C1 | **Jump / Fall states** in the locomotion graph -- needs jump/fall clips (the Fox has only Survey, Walk, Run); `VerticalSpeed` parameter was tried and reverted, add it back with the states |
| C2 | Script **late update** (`OnLateUpdate`, after physics / animation / propagation) -- was built for the camera-follow investigation and reverted; the real cause there was missing anti-aliasing, so only add it if a script needs it |
| C3 | Skeletal meshes with **several skins** or unskinned meshes next to the skin still spawn as nodes (not the single-entity path) |
| C4 | Animation graph phases the first commit message calls missing (see the plan doc, section 8) |
| C5 | Fox_Locomotion `Speed` thresholds assume walk 2 / run 5 m/s (retune if the speeds change); movement defaults are for a ~1.8 m character -- the Fox capsule / speeds need sizes for a ~0.8 m animal |
| C6 | Optional rename `.nmesh` -> SkeletalMesh (deferred by decision) |

## Import / glTF (plan: `Animation_Graph_Architecture_Plan_2026.md` section 8, "known limits of Import Into Level")

| # | Item |
|---|---|
| I1 | **Not-yet-handled glTF features:** vertex colors (COLOR_0), morph targets, `EXT_mesh_gpu_instancing`, `KHR_node_visibility`, `KHR_materials_variants`, extras, multiple scenes, physics extensions. Vertex colors first (most common) |
| I2 | Combining static meshes drops the file's node structure, lights, cameras and node animations by design (Import Into Level keeps them) |
| I3 | Import Into Level with skeletal models: the group logic (static + skinned jobs, one level) is **untested** -- no glTF with two skinned meshes at hand (the sample assets have none) |
| I4 | Import Into Level: animations of a file that also has a skeletal model (the skeletal asset owns those clips) |

## Rendering (plan: `Engine_Architecture_Plan_2026.md`)

| # | Item |
|---|---|
| R1 | **Own TAA** (UE4/Karis style: variance clipping in YCoCg, Catmull-Rom history, closest-depth motion vectors) -- decided 2026-09-26 to keep **DLSS DLAA (no RR)** for now; edge stepping / crawling with DLSS off is the missing AA, not a bug. DLSS costs measured: DLAA/Balanced ~1.1-1.2 ms, with Ray Reconstruction 4.3 ms (raster) / 11.6 ms (path tracing, replaces NRD) |
| R2 | Phase 6b **geometry streaming** (cluster pages): paused on purpose, returns with a scene that does not fit in memory or with cluster acceleration structures (D8) |
| R3 | Phase 6b-2 RT fallback LOD: tried and reverted (holes in the path tracer) -- see the plan |
| R4 | **Stats overlay text** looked broken after the viewport-resize fix (possibly large Peak values; Shift+F3 resets) -- never confirmed |
| R5 | Editor/DDGI: the probe grid is a fixed 1.8 m grid at the Bistro scale (a moving character crossing it can show stepping in the lighting) |

## Editor and tools

| # | Item |
|---|---|
| E1 | Scene sizes: the test scene's floor collider must match the mesh (`Cube` mesh is 2 m: half extents 1,1,1; `Sphere` mesh radius 1). Check any new scene against the mesh bounds (the import dialog does not show model size yet: idea -- show bounds and a "fit to height" Import Scale) |
| E2 | Content Browser rename for more asset types than prefabs (meshes have cooked companions named after them) |
| E3 | Rigid body **default damping** (0.05 rolls a ball for over a minute; ~0.5 angular damping is more usual) |

## Untested (built, waiting for the user)

- Import Into Level: animations (tested, works), skinned-only level and skeletal Do Not Combine placement (untested, I3)
- Rigid body interpolation (built 2026-09-26, tested by the user: works)

## Done (recent)

- 2026-09-26: Apply to Outer for nested instances (N2) -- tested by the user

- 2026-09-26: Prefab Mode save prompt (N1): `*` in the toolbar when unsaved, Exit / opening a scene ask Save / Don't Save / Cancel -- tested by the user

- 2026-09-26: Prefabs P1-P6c (asset, spawn, create by dragging, inline rename, Prefab Mode, `Scene.Instantiate` + glowing ball test, property / structure overrides, nested prefabs, variants), rigid body interpolation, Import Into Level node animations, viewport input gate for game scripts, glowing-ball script API (mouse buttons, rigid body velocity/force, Destroy)
- 2026-09-25: outliner folders, hierarchy asset drops, one entity per dragged asset, Content Browser multi-select, Import Into Level (flat import, combine modes, per-mesh assets), jitter fix (camera precision), texture / geometry streaming performance fixes, Stage A/B of the animation restructure
- Earlier: animation graph Steps 1-7 (graph core, canvas editor, parameters, character controller, state machine)
