# Editor features plan -- October 2026

> Future.md items 2 (modern content browser, material window), 3 (animation player node, sequencer, animation editing),
> 5 (up to four viewports), 10 (basic shapes) and 12 (undo / redo). Items 10 and 12 and the first parts of 2 and 3 were
> built in this round (untested: the user builds and tests); the rest is planned here. Same contract as the other plans.

## 1. Built this round (2026-10-18, untested)

### F12 Undo / redo -- `NoxEditor/src/EditorHistory.{h,cpp}`
- **Ctrl+Z** undo, **Ctrl+Y** / **Ctrl+Shift+Z** redo (both redo: Ctrl+Y is the Windows / UE / Visual Studio habit,
  Ctrl+Shift+Z the Blender / Photoshop / Godot one), and **Edit > Undo / Redo** with the step's name ("Undo Delete
  Entity", "Undo Edit Cube", "Undo Transform"). Text fields keep their own Ctrl+Z while typing. Letter shortcuts follow the
  keyboard layout (2026-10-18 fix: they read physical US positions, so on a German QWERTZ keyboard Z and Y were swapped).
- **How:** every step is the whole scene as its scene-file text (`SceneSerializer::SerializeToText`, new); undoing loads
  that text into a new scene (`SceneSerializer::DeserializeText`, new) and swaps it in like opening a scene. So anything
  the scene file holds is undoable without per-command code: gizmo drags, Properties edits, add / remove component,
  rename, reparent, folders, delete, duplicate, placed assets and shapes, prefab overrides. The selection is kept by UUID.
- **When a step is taken:** when an interaction ends (an ImGui item or the gizmo released, a shortcut key), a cheap
  fingerprint of the scene (ids, names, parents, folders, transforms, mesh/model/prefab kinds) and the text of the entity
  the Properties panel shows are compared with the current state; only on a difference is the whole scene serialized.
  A drag is one step. Things that happen on their own after a step (a placed model spawning / flattening once its asset
  loads) are folded into that step. Transforms driven by a playing animator are ignored.
- **Limits:** assets are not part of the scene history (material edits, prefab files, imports); Play / Simulate are not
  recorded (their scene is a throw-away copy); Prefab Mode has its own history and the level's comes back on exit;
  opening another scene starts a new history; at most 100 steps / 512 MB. Cost: one serialization per step (the
  "Undo Snapshot" scope in Nox Stats; a Bistro-sized level is ~1 MB of text), one scene load per undo ("Undo Restore").

### F10 Basic shapes -- `NoxCore/src/NoxCore/Asset/PrimitiveShapes.{h,cpp}`
- **Create > Shapes** in the main menu, and **Shapes** in the hierarchy's right-click menu (blank space or a folder):
  Cube, Sphere, Cylinder, Cone, Pyramid, Plane, Capsule. Placed where the editor camera looks, undoable.
- The first use of a shape writes `Assets/Shapes/<Name>.gltf` (+ `.bin`) and imports it as one static mesh (meters ->
  world units like any glTF); later placements reuse that asset, so its material can be edited like any other.
- Sizes like UE's BasicShapes: 1 m cube / cylinder / cone / pyramid / plane, 1 m sphere diameter, capsule 1 m wide and
  2 m tall. Checked by a standalone test: every closed shape is watertight with outward winding and the right volume,
  and the files parse with the engine's glTF reader.

### F2 Content browser, first part -- `ContentBrowserPanel`, `MaterialEditorPanel`
- **Layout:** folder tree on the left (click to open, drop entities from the hierarchy to make a prefab there),
  tiles on the right, a draggable splitter between them; toolbar with **+ Add** (C# script, animation graph), Import,
  up-one-folder, a **clickable path** (Assets > Models > Fox, each part a drop target too), **search** (this folder and
  everything below it, like UE), a **type filter** (meshes, materials, textures, prefabs, scenes, animation) and the tile
  size; a status line (items / selected).
- **Material Editor window:** double-click a material in the Content Browser, or **Edit** next to a material slot in the
  Properties panel. The Properties panel keeps the slots and a collapsed **Quick Edit** with the same controls
  (`MaterialEditorPanel::DrawProperties`, shared). Edits are live and saved to the `.nmat`.

### F3 Animation graph, first part -- `AnimationGraphNodes.cpp`
- **Clip node = UE's Sequence Player:** its **Speed** is now an input pin (wire a parameter in to drive the play rate;
  graphs saved before keep their Speed property, which the unconnected pin reads), plus a **Start Position** (seconds,
  applied again each time a state machine enters the state).
- **New BlendSpace1D node** (Blending): up to four clips along one axis with their positions, the **Value** pin picks the
  point (e.g. Speed -> idle / walk / run in one node); the two clips around the value play in step (shared normalized
  phase), so feet stay together while blending.
- **Graph editor fixes (2026-10-18):** node properties are drawn in the order the node type declares them, each in its own
  ImGui ID scope (BlendSpace1D's four empty clip buttons all read "None" and collided), a pin's literal is greyed out
  while a link feeds the pin, and the asset picker keeps same-named clips apart (hover shows the path).

## 2. F2, the rest: rendered thumbnails and material previews

| Phase | What |
|---|---|
| B1 | Thumbnail renderer: a preview view (camera, one directional light, the project's default environment) rendered offscreen at 256 x 256 into the thumbnail cache (`ThumbnailCache` already has a cache path). Meshes: framed by their bounds. Materials: on a sphere (the Sphere shape from F10). Prefabs: their entities. Rendered a few per frame in the background, saved as `.ntex` next to the cache so they survive restarts |
| B2 | The Material Editor shows a live preview sphere (same preview view, re-rendered on change) instead of the color swatch |
| B3 | Asset context menu: rename for every type (E2), **delete with a reference check** (what in the open scene, other scenes and prefabs uses it; Force Delete / Replace References like UE), show in explorer, reimport; drag assets onto folders to move them (registry paths updated); an **unused assets** report (registered, referenced by no scene or prefab). Deleting an entity never unregisters its assets (Future.md item 14, answered in `Status_Audit_2026-10.md`) |

**B3 built (2026-10-18, untested):**
- **Right-click on any asset:** Rename (F2), Create Variant (prefabs), Find References, Show in Explorer, Delete; with several
  selected it acts on the selection. **Del** deletes the selection.
- **Delete with a reference check:** a dialog lists the assets and, per asset, what uses it -- the open scene's current state
  (unsaved changes included) and the project's files -- then **Delete**, or **Force Delete** when something still does.
  Deleting removes the asset's file, its source when nothing else is made from it (a model's .gltf stays while any of its
  meshes, skeleton or clips remain), and its registry entry.
- **Who uses what** (`EditorAssetManager::FindReferences / FindUnusedAssets`, read from the files on demand): scenes, prefabs,
  animation graphs and the project name assets by handle (YAML numbers); materials and meshes name textures by path (the
  strings in their binary files, relative or absolute, any slashes, case ignored); scripts by path; a model's mesh owns the
  materials, skeleton and clips cooked from it by name; assets made from a source (meshes from a .gltf) use it.
- **Move:** drag assets onto a folder (a folder tile, the folder tree, a part of the path). Prefabs and animation graphs
  move (and rename); everything else is named by path somewhere or cooked with companion files, so it stays (logged).
- **Tools > Find Unused Assets:** every registered asset nothing uses (scenes are where use starts, never listed), with
  checkboxes and Delete Selected (through the same confirmation).
- Not yet: moving / renaming textures, materials and meshes (needs rewriting the paths that name them), redirectors.

B1 needs the renderer to draw more than one view per frame, the same work as F5's M2 (§4); do M2 first.

## 3. F3, the rest: animation

What is used in 3D, to answer the question in Future.md: **all of it.** The animation graph (blend spaces, state
machines) drives characters; a **sequencer / timeline** (UE's Sequencer, Unity's Timeline, Godot's AnimationPlayer --
which is used for 3D as much as 2D) keyframes properties of entities for doors, elevators, light flicker, cameras and
cutscenes. Authoring skeletal clips themselves (keyframing bones) stays in Blender / Maya in every engine; engines edit
clips' metadata (notifies, curves, trims) and retarget.

**Built 2026-10-18 (untested):** A2 notifies, A3, A4, A5, A6, and the start of A7 -- see below the table.

| Phase | What |
|---|---|
| A2 | **Notifies and sync markers** on clips (time-stamped names: "FootL", "Hit"), raised as script callbacks (`OnAnimationEvent(name)`) and used to sync blend-space clips by marker instead of normalized time |
| A3 | **Layered Blend per Bone** (upper body from a bone down: "spine_02") and **Additive** (aim offsets, breathing) nodes |
| A4 | **Montages / slots**: one-shot clips started from scripts (`Animator.PlayMontage(clip)`: attack, interact, reload) blended over the graph's output in a named slot node |
| A5 | **Root motion**: the root bone's motion taken out of the pose and handed to the character controller |
| A6 | **BlendSpace2D** (direction x speed, triangulated sample points) |
| A7 | **Animation preview window**: double-click a clip / skeletal mesh -> the model in an isolated preview view (B1's preview renderer), scrub, play rate, loop range, notify editing |
| A8 | **Sequencer**: `TimelineAsset` (tracks of keyframed entity properties: transform, light color / intensity, camera FOV, material parameters, events), a Timeline panel with keys and curves, a `TimelinePlayerComponent`, scripting `Timeline.Play()` |
| A9 | Audio graph (the plan's step 8), on the same node-graph core |

What was built:
- **A2 Notifies:** `AnimationSequence::Notifies` (time + name), kept beside the clip in `<clip>.nanim.notifies` (YAML, so a
  re-cook keeps them). Raised when a playhead crosses one -- Clip nodes, a blend space's leading clip, montages -- forward,
  backward and across the loop point; scripts receive `OnAnimationEvent(string name)` (new virtual on `EntityBehaviour`),
  after the animation update on the main thread. Sync markers (blend spaces syncing by marker) are not done.
- **A7 start, the Animation Clip window:** double-click a clip: its length and channels, a timeline (playhead + a marker per
  notify), add / edit / delete notifies; saved at once. No 3D preview (needs M2's second view).
- **A3:** **LayeredBlend** node (Base, Blend, Alpha; "Bone" = the branch root, e.g. spine_02; "BlendDepth" fades in over
  that many bones) and **ApplyAdditive** node (Base, Additive, Reference -- unconnected = rest pose -- and Alpha; rotation
  delta additive * inverse(reference) applied before the base rotation, translation offset, scale ratio).
- **A4 Montages:** **Slot** node ("SlotName", default DefaultSlot) passes its source through unless a montage plays there;
  scripts: `Animator.PlayMontage("path/to/clip.nanim", slot, blendIn, blendOut, rate)`, `StopMontage(slot, blendOut)`,
  `IsMontagePlaying(slot)`. Blends in and out, fires the clip's notifies, a new one replaces the slot's current one.
- **A5 Root motion** (graph mode): Animator **Root Motion** + **Root Bone** (empty = the skin's first joint). The root
  bone's horizontal movement (the skeleton's model space) is taken out of the pose and added to the entity's translation
  (rotated / scaled by the entity), its height and rotation stay animated; a looping clip's jump back to its start is
  detected (a large backward step) and replaced by the recent motion. Not routed into the character controller yet.
- **A6 BlendSpace2D** node: up to eight clips at (X, Y) points (a locomotion layout by default), "X" / "Y" pins, gradient
  band weights (exact at the samples, no triangulation), clips in step by normalized phase. Checked with a standalone
  test of the weights and of the notify crossing rules.

## 4. F5: up to four viewports

What the renderer has: per-view culling and draw resources exist (`CullView`, `ViewDrawResources`, parameterised passes),
but only one view is instantiated (Engine plan §9 "A second real view"), and everything temporal (DLSS / TAA history,
NRD, ReSTIR reservoirs, path tracer accumulation, Hi-Z) exists once, sized to the single viewport.

| Phase | What |
|---|---|
| M1 | **Editor layout:** up to four viewport panels, each with its own `EditorCamera`, view mode (perspective / top / front / side, already in `EditorViewMode`), gizmo, picking and toolbar. **Layout presets** that snap with the dock builder: 1, 2 side by side, 2 stacked, 3 (one big + two), 4 (2 x 2); panels can still be dragged and resized freely. The focused viewport is the "active" one (keyboard, gizmo shortcuts) |
| M2 | **Renderer views:** a `RenderView` (camera matrices, size, quality settings) with its own `ViewState` (all temporal history resources) per viewport, the frame graph built once per view; per-frame shared work (GPU scene update, skinning, TLAS, DDGI) stays once per frame. This is also what B1's thumbnails and A7's preview need |
| M3 | **Cost control:** the active viewport renders at full quality; the others raster-only (ortho views already are), optionally lower resolution, and "realtime off" (re-render only when the scene or their camera changes) like UE's viewport option |
| M4 | Per-viewport show flags: lit / unlit / wireframe / debug views, grid, colliders, stats overlay |

Exit test M1+M2: four viewports (perspective + top + front + side) each orbit / select / move independently; the
perspective one keeps DLSS and ray tracing while the others are raster.

**Built (2026-10-18, untested): M1, and M2 + M3 in their cheaper form.**

- Window > Viewports: presets 1, 2 side by side, 2 stacked, 3 (big left + two stacked), 4 (2 x 2), and Viewport 2 / 3 / 4
  on their own. The viewports dock inside a dockspace of their own (the "Viewports" area in the middle of the editor), so a
  preset rebuilds only that area; a viewport's tab can be dragged anywhere (also out of the area) and the splitters resized.
  Which viewports are open and their views are kept in imgui.ini (`[NoxViewports][State]`).
- Each viewport has its own `EditorCamera`. The **active** one (the last clicked, scrolled or dragged an asset over; framed
  in orange when several are open) is the full render with every tool: gizmo, picking, box select, placement, measure, grid,
  shortcuts, DLSS / ray tracing / path tracing. Activating another swaps its camera in (`EditorLayer::ActivateViewport`), the
  main render follows its size and ortho raster-only settings, and the temporal histories restart
  (`Renderer::ResetCameraHistory`). The activating click does nothing else (no selection change).
- The other viewports are **extra views** (`Renderer::SubmitExtraView`, `Passes/ExtraViewPasses.cpp`): their own uniforms
  and `CullView`, frustum culling (no Hi-Z), one forward pass (opaque / masked surfaces lit by `Material_TransparentLit_Mesh`
  with the new `MESHLET_SHADE_OPAQUE` flag, unlit, sky, transparent), tonemapping with the main view's exposure, and the 2D
  overlay's world-space batch with their own grid (`Renderer2D::FlushWorld` / `MakeGrid`). No G-buffer, shadows, RT, DLSS
  or history: cheap, and the active view is untouched. A resize keeps the old image on screen until the new size is drawn.
- Camera input: the viewport a mouse button went down in keeps the mouse until all buttons are up
  (`EditorCamera::OnUpdate(ts, acceptInput)`), so a right-click in another panel no longer flies the camera.

Open: shadows in extra views (comes with item 4's ray-query lighting in the forward shader, H2), selection outline and
gizmo in extra views, per-viewport show flags (M4), lower resolution / "realtime off" (rest of M3), the main camera
itself is not saved between sessions.

## 5. Order

F12 / F10 / F2 / F3 first parts -> user tests -> M1 -> M2 -> B1 / B2 -> A2 -> A7 -> A4 / A5 -> A3 / A6 -> A8 -> M3 / M4 -> B3.
