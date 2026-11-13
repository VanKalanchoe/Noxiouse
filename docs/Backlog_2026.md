# Backlog -- one list of everything that is open (kept up to date; started 2026-09-26)

> How to use: pick an item, say its number/name, and it is built the way the plans say (phase gates: the user builds and tests, nothing is
> built by me). When an item is done, move it to "Closed" with the date. Details live in the plan doc named in each line -- this file is
> only the index. Newest decisions win over older notes.

## Open

Nothing open: everything left is in "Ignore for now" below (2026-11-06).

### Ignore for now

| # | Item |
|---|---|
| B1 | ReSTIR DI broken, cause unknown (on hold: not needed now) |
| B2 | ReSTIR PT temporal artifact (ignore for now) |
| B3 | ReSTIR light indices across light changes (ignore for now) |
| B4 | Meshlet cone culling (ignore for now) |
| C4 | Step 8: **audio graph**, a second node-graph domain on the same core and editor panel. Planned 2026-11-06 with the whole audio system it needs: miniaudio + Steam Audio, MetaSounds-style graph, buses, spatial audio, phases A1-A5 (`Audio_Architecture_Plan_2026.md`). **A1 Core built 2026-11-06** (miniaudio, mixer + stream threads, clips with Load Types, Audio Source / Listener, 3D panning / attenuation / Doppler, buses, script API 16, Content Browser preview, packaging); tested by the user. **A2 Audio graph built 2026-11-06** (AudioGraph domain and runtime, ~60 nodes, streamed Wave Players, live preview, Audio Source Sound + parameter overrides, script API 17); tested. **A3 Mixer built 2026-11-07** (13 effects for buses and graphs, bus tree asset `.nmixer`, sends, ducking, sidechains, snapshots, Mixer panel with meters, Audio Volumes, script API 18); tested. **A4a Steam Audio built 2026-11-07** (HRTF / panning per voice, air absorption, occlusion and transmission against the level's static meshes with acoustic materials, script API 19); tested. **A4b real-time reflections built 2026-11-07** (reflection scene and thread, budget of the nearest voices, convolution through the reflection mixer, ambisonic decode into a bus, Mixer > Acoustics); tested. **A4c baked acoustics built 2026-11-12** (Acoustic Probe Volumes with a Bake button, `.nacoustics`, baked / real-time reverb at the listener through the Acoustic Reverb effect, pathing around corners); tested. **A5a voice management built 2026-11-12** (priority and concurrency on sound assets, real / virtual voices with a budget, audio stats, Impact Sound component on Jolt contact events, particle burst sounds); tested. **A5b built 2026-11-12** (physical surfaces, footstep notifies by surface, sequencer Audio track, graphs inside graphs, Curve node); waiting for the user's test |
| PP1 | **Post effects** (bloom, DOF, motion blur, grading / LUT, vignette / grain / CA / lens flares, CAS): built 2026-11-05, untested; not wanted for now (Settings > Post Effects; `Hybrid_Renderer_Plan_2026-10.md` §10) |
| L4 | **Level transforms** (a sub-level's Offset / Yaw in the Levels panel): built 2026-11-06, untested; later |
| N1 | **Game UI (UMG-like), proposed 2026-11-12:** a widget asset with a visual designer (canvas / horizontal / vertical / grid boxes, anchors and DPI scaling, text, image, button, slider, progress bar, list), bindings to script values, HUD and menus in the game, gamepad / keyboard focus navigation, animations of widget properties on the timeline |
| N2 | **AI navigation, proposed 2026-11-12:** Recast navmesh baked from the level's static geometry (Nav Mesh Bounds volume, agent radius / height / slope / step), Detour path queries and path following, crowd avoidance (DetourCrowd), nav links for jumps / ladders, then Behavior Trees + Blackboard as a third node-graph domain |
| N3 | **Landscape, proposed 2026-11-12:** heightfield terrain with sculpt (raise, smooth, flatten, noise) and paint tools, material layers blended by weight maps and tied to physical surfaces (footsteps, Jolt heightfield collider), LOD + streaming with world partition, foliage / grass scattering painted per layer |
| N4 | **Input actions + save games, proposed 2026-11-12:** Enhanced-Input-like Input Action and Mapping Context assets (keys, gamepad, modifiers and triggers such as hold / double tap, rebinding in the game, saved per user); save games: a SaveGame object with slots, serialized script fields and entity state, async save / load |
| R10 | **AVBOIT** (Adaptive Voxel-Based OIT; Drobot, Activision, SIGGRAPH 2025 Advances; Unreal's AVBOIT). **Built 2026-11-12, waiting for the user's test:** Settings > Order-Independent Transparency > Method (Weighted Blended / AVBOIT, default AVBOIT; project setting OIT 0 off / 1 weighted blended / 2 AVBOIT; the All presets pick AVBOIT). Per frame: occupancy of 1024 virtual log-depth slices at render size / 8, adaptive depth warp to 16-256 physical slices (Depth Slices, default 64), RGB extinction splatted (3 x (8+2) bits per atomic add, linear across two slices, overflow slice per channel), integrated front to back into an RGBA8 volume; the shading pass weights each fragment by the transmittance 2 slices in front of it; the revealage target is RGBA16F per channel, the resolve two draws (scene x colored revealage, + normalized light): tinted glass tints what is behind it in OIT with either method (`OITShared.slang`, `AVBOITBuild.slang`, `Passes/AVBOITPasses.cpp`) |
| N6 | **Networking, proposed 2026-11-12; NET0 (headless dedicated server, one tick for scripts and Jolt) built 2026-11-13, waiting for the user's test** (`Networking_Plan_2026-11.md`): authoritative server with state replication (Unreal's model; deterministic lockstep ruled out: the C# scripts, iteration order and async loading are not bit-deterministic), Valve GameNetworkingSockets transport (loopback / direct IP / Steam SDR backends), headless dedicated server (no window, GPU, audio; CPU-only assets), one network tick for scripts and Jolt, NetworkIdentity, property replication + RPCs from C# attributes, character prediction and reconciliation, interpolation, lag compensation, interest management on world partition cells, editor Play with N clients. Phases NET0-NET6 |
| N5 | **Template pictures** in the Project Manager, **built 2026-11-12, waiting for the user's test:** template tiles with pictures (`assets/Templates/*.png`, drawn by `scripts/make_template_pictures.py`), a new project shows its template's picture until its first screenshot, a `<ProjectName>.png` beside the `.nproj` overrides both (Unreal's project thumbnail) |
| R11 | **Startup time** (measured 2026-11-12 from the user's log: 7 s to the first frame; Streamline / NGX ~2.5 s, ~4 s creating ~214 pipelines one after the other). **Built 2026-11-12, waiting for the user's test:** shader-object pipelines are built on up to 8 worker threads; sources without a cached binary compile there too (2026-11-12 fix after the user saw first-time compiles get slower: the main-thread rebuilds ran in random order and kept recompiling the same big modules) with a Slang global session per thread (at most half the cores), each source once per start (others wait for it), cache files written atomically; `Device::finishPipelineBuilds` waits in creation order and draws the progress window meanwhile; the log shows "Pipelines: N built in parallel from the cache, M compiled in parallel ..." and "Renderer start-up: device X ms, pipelines and resources Y ms". Streamline / NGX start-up is NVIDIA's and stays |
| C9 | **Stair animations:** with a walk clip made for flat ground (UAL's step ~65 cm, a tread 30 cm) the feet land on every second step; AAA games play stairs up / down walks chosen when the floor under the feet is a stair. Needs a character with stair clips (none at hand): then an `OnStairs` parameter and a stair state |

## Closed (checked against the code 2026-10-30)

Built items many of which the user has not tested yet (marked "untested"); the plan docs hold the details.

- **2026-11-06 (tested by the user with the Quaternius UAL character):** C1 Jump / Fall states (At End of State transitions,
  VerticalSpeed; falling = not grounded and not rising), C6 IK Look At and Foot Placement (Two Bone IK still to try: C11), root
  motion (per-clip root travel blended like the pose; no teleport on Shift), blend space Smoothing Time and a constant turn rate.
  Fixed on the way: skinned import scale (inverse bind matrices), Animator parameters follow the graph's defaults unless overridden,
  parameters inside states, transition rule ID clash, Content Browser Duplicate and files moved outside the editor, Look At yaw / pitch
  limits with a fade behind, the character controller (no gravity while walking: no sliding down slopes; Unreal's floor test for
  IsGrounded: stairs down; stair smoothing; one jump per press), CameraFollow in OnLateUpdate. New editor features: state machine
  wires routed with right angles between pins, bone picker for bone-name properties, Active (Inspector box + Hierarchy eye, Unity's
  SetActive), physics collider overlay for every collider (inactive ones red, character capsules, mesh colliders, foot IK rays).
  C5 (Fox sizes) dropped: moot with the 1.8 m character. C11 Two Bone IK tested too (the hand reaches its target).
  P1 profiler extras tested (a capture saved and read back); the capture showed a ~26 ms "ImGui Build" spike every 2 s: the Content
  Browser walked the whole asset folder tree on the main thread every 2 s to see new folders -- now on an IO worker.
  C10 built: `Entity.Active` / `SetActive` / `ActiveInHierarchy` for scripts (script API 15).

- **2026-11-06 (tested by the user):** L1 sub-levels (Levels window: current level, visible, locked, Always Loaded / streamed, Move
  to Level, each level saved into its own file, Play copies them as edited; the scene's own primary camera stays the view; hierarchy
  Shift-click in display order). R11 more async compute (NRD shadows / reflections, DDGI blends, emissive light grid on the compute
  queue; Raster (All) 8.64 -> 8.16 ms GPU, 115 -> 121 fps); RS1-RS6 raster versions (shadow maps + cascade caching + local-light atlas,
  contact shadows, GTAO, SSR + reflection probes, relightable probe volume + SSGI, froxel fog); L2 a streamed level drawn once complete,
  L3 volumes use every streaming source (with the Play / Simulate crash fix: Jolt initialized before the collision group filter).
  H5 (denoising the transparent signals) dropped by the user.
  R10 (DLSS "WorldToScreenMatrix not invertible") dropped: reopened if it comes back. Raster colored translucent shadows: not wanted.
- **I2** `KHR_texture_transform`, normal / occlusion strength (2026-10-31); **M1** the missing material lobes (2026-10-31). Materials
  policy (decided 2026-10-31): Nox keeps its own glTF PBR model with OpenPBR as the reference for each lobe, no run-time material graphs

- **N4 World units and world tools:** U1-U6 built (cm world unit, grid, snap, ortho views, Measure tool, reference figure, import
  units, physics unit boundary) plus per-field unit display. U2 confirmed by the user; the rest untested. `Units_And_World_Tools_Plan_2026.md`
- **E1 Model size at import:** the import dialog shows Size W x H x D, a Source Unit combo, a wrong-unit warning and Fit to height (U4, untested)
- **E2 Rename more asset types:** built 2026-10-20 (textures, materials, meshes with companions, Reimport, Replace References)
- **F1-F14 (Future.md):** all built or answered. F2 content browser + material window, F3 animation player / sequencer, F5 four
  viewports, F7 runtime player + `RuntimeAssetManager`, F8 packaging (`.npak`, precompiled shaders), F10 basic shapes, F11 script
  mesh / jobs APIs, F13 particles + clouds, F14 stale registry cleanup: untested. F4 hybrid renderer: built except H5 (open above).
  F6 procedural sky: built, including night (moon, stars) and haze on forward passes. F9 stats overlay and F12 undo / redo: confirmed by the user.
- **Prefabs P1-P7:** built 2026-10-20 (P1, P5, P7 untested); P4 decided (Unity's rule), P6 closed by R2. `Prefab_Architecture_Plan_2026.md`
- **C2** script late / fixed update, **C7** sync markers, notifies, sockets, graph preview and the rest: built 2026-10-20 (untested)
- **C6** rename `.nmesh` -> SkeletalMesh: not done, by decision
- **I1 glTF features:** all handled (2026-10-31): `EXT_mesh_gpu_instancing`, `KHR_node_visibility`, vertex colors, morph targets
  (M1, M2), material variants, the default scene only, node extras -> Metadata, and `KHR_physics_rigid_bodies` complete: bodies,
  box / sphere / capsule (tapered) / cylinder (cone) / plane / mesh / convex colliders, materials, triggers, collision filters,
  joints (new Cylinder / Plane / Mesh Collider 3D, Physics Joint 3D, Collision Filter 3D components; compound bodies from child
  colliders). Tested by the user: vertex colors, morph targets, material variants; the rest untested. Mesh / material extras not
  imported by decision. `Engine_Architecture_Plan_2026.md` §5.8.2
- **C8** built 2026-10-31, untested:
  - **Per-entity material parameters** (Unreal's dynamic material instance parameters):
    - MaterialComponent `Parameters`: Base Color, Emissive Color / Strength, Metallic, Roughness, per slot or on all.
    - Such an entity draws with GPU material records of its own, refreshed every frame.
    - Edited in the Material section; saved with the scene.
  - **Material Parameter timeline tracks:** they write `SequencerParameters`, over the authored ones, kept after the timeline;
    previews restore them.
  - **Sub-Timeline tracks** (Unreal's Subsequence track): sections of another timeline (length, start offset, rate, loop), played on
    the track's target, nested up to depth 8, with the child's events.
  - **Variable node input pins** (Unreal's Add Pin): `NodeTypeDesc::VariableInput`, with + Pin / - Pin in the graph editor, and a new
    **Blend Poses by Int** node.
  - Offline tests in scratchpad `gstest/`: `subtimelinetest`, `varpintest` (the real compiler and evaluator), `matparamtest`.
  - **Not carried:** material parameters on a model instance's spawned nodes (not node overrides), and parameters in HLOD bakes.
- **I4** Import Into Level / static meshes of a file that also has a character: clips are now split by what they move (2026-10-31,
  untested). A clip that moves only the skins' joints and the armature above them stays with the skeletal asset; one that moves
  other nodes (a door, a fan) also goes to the static meshes and the level, which animate only those nodes. Before, any skeleton
  in the file kept every clip from the level. Offline test: scratchpad `gstest/clipsplittest`.
- **C3** Characters with several skins, or meshes under a joint, are one character entity (2026-10-31, untested). The cook merges
  the file's skins into one (Unreal's one skeleton per skeletal mesh): a joint shared with the same inverse bind matrix is one entry,
  and every skinned vertex's joint indices move into the merged list. An unskinned mesh under a joint (a sword in the hand) is bound
  to it rigidly (Unreal's Import Meshes in Bone Hierarchy): a model-space copy with weight 1 on that joint. `MeshImporter::UnifySkins`;
  offline test scratchpad `gstest/unifytest` (merged skinning equals each skin's, at rest and posed; the sword follows the hand).
- **I2** combining static meshes drops the node structure, lights, cameras and node animations: by design (Import Into Level keeps them)
- **I3** Import Into Level with skeletal models: built, untested (no glTF with two skinned meshes at hand)
- **R1** own TAA (2026-10-20), **R2** geometry streaming (2026-10-29, tested on Bistro), **R4** -> F9, **R5** camera-following DDGI
  cascades (2026-10-19)
- **R6 Streaming Virtual Texturing:** all phases built and measured 2026-10-30 (Bistro texture VRAM 680 -> 381 MB, same image,
  frame time unchanged). `Engine_Architecture_Plan_2026.md` §5.12.2
- **R7 Geometry compression:** 88 -> 36 bytes per vertex, cooked format v4, Bistro geometry 319 -> 161 MB; vertex indexing before
  clustering, Bistro re-imported (folder 4.21 -> 4.03 GB). §5.8.2
- **R8 Sparse geometry buffers:** Bistro 621 -> 319 MB committed (302 MB of VRAM back). §5.8.2
- **Tried and reverted:** R3 RT fallback LOD (holes in the path tracer); R9 Preserve Area for foliage (2026-10-30, no gain in the
  Bistro test, §5.7.2)
- **Rigid body interpolation:** tested by the user, works
- **E3 Balls roll for minutes (2026-10-30, untested in the editor):** not a damping problem. The 0.05 defaults are Jolt's own, in
  line with Unreal (0.01 / 0), Unity (0 / 0.05) and Godot (0.1 / 0.1); raising them slows bodies in the air too. What stops a real
  ball is rolling resistance, which Jolt lacks. Added the 3D one (as the 2D colliders have it from Box2D):
  - Sphere and capsule colliders have **Rolling Resistance** (default 0.05).
  - `JoltRollingResistance`, the physics system's contact listener, applies a torque of coefficient x radius x weight along the
    supporting contact normal against the rolling part of the angular velocity, never past zero.

  Offline with real Jolt (scratchpad `jolt/rolltest`), a ball at 5 m/s:
  - Before: still rolling after 180 s, 104 m away.
  - Default 0.05: stops in 10.7 s after 24.5 m; 0.01: 29.8 s, 57 m.
  - Matches theory (a = Crr g / 1.4) within 2% without damping.

  Other checks:
  - Spin in the air is unchanged.
  - On a 5 degree slope a ball rolls with 0.05 and holds with 0.1 (tan 5 degrees = 0.087).
  - A ball on a dynamic box settles.

## Done (recent)

- 2026-11-12 (waiting for the user's test): **Project Manager** (`NoxEditor/src/ProjectManager/`): the editor opens with it when no
  project is given -- recent projects (+ the Facerun sample), New Project (name, location, world units, render quality, C# scripts
  project with an example Rotator referencing the editor's Nox.ScriptCore.dll, .gitignore), Browse, Show in Folder, Remove from List,
  "Always open the last project at startup"; hands over to the editor in the same process; File > Project Manager... relaunches the
  editor into it (one project per editor process, as Unreal / Godot); scripts never built are built when a project opens. Offline test:
  project files made and read back, the generated C# project builds against Nox.ScriptCore. **Graph undo / redo** (Ctrl+Z / Ctrl+Y per
  graph window, Edit menu, drags as one step).

- 2026-11-12 (waiting for the user's test): node graph wires in every graph (audio, animation, a state's graph) routed orthogonally
  around the nodes like the state view's transitions (`NoxEditor/src/Panels/NodeGraph/WireRouter.*`: shortest path with corner cost over
  the lines along the node edges, then shared lines spread into lanes; offline test 58 wires / 42 nodes in 8.6 ms, all clear of nodes),
  wires selectable (Delete, right-click Break Link / Add Reroute Node / Reset Route), legs draggable (route saved per link as `Route`),
  output pins flush with the node's right edge and the header band across the whole node, Unreal's wire dropped on empty canvas ->
  filtered create menu with auto-connect, Alt+click to break links, moving nodes marks the graph unsaved.

- 2026-10-20 (all untested unless noted): the second audit's bugs (above), scripting (fixed / late update, per-project assembly, build on
  save, `[Expose]` metadata, API version, Wait for Debugger), packaging (`.npak` + VFS, precompiled shaders, texture cook, self-contained
  .NET, NOX_DIST, window modes, project renderer settings), renderer (OIT, TAA, DDGI relocation, shadow-ray rung, particle curves and
  motion vectors, path tracer glass and shadow-ray transmittance), editor (prefab P1 / P5 / P7, asset rename / move / reimport / replace
  references, prefab and model thumbnails, viewport show flags / resolution / realtime, outline and gizmo axes in extra views, reference
  figure at the cursor, GPU raycasts, wireframe, To Grid, streaming feedback from previews), animation (C7), text outline / background,
  Box2D surface materials and units, glTF instancing / visibility. Details: `Status_Audit_2026-10.md` §0.

- 2026-09-26: Hierarchy Create Prefab menu and clearing script references that point outside a new prefab (N3) -- tested by the user

- 2026-09-26: Apply to Outer for nested instances (N2) -- tested by the user

- 2026-09-26: Prefab Mode save prompt (N1): `*` in the toolbar when unsaved, Exit / opening a scene ask Save / Don't Save / Cancel -- tested by the user

- 2026-09-26: Prefabs P1-P6c (asset, spawn, create by dragging, inline rename, Prefab Mode, `Scene.Instantiate` + glowing ball test, property / structure overrides, nested prefabs, variants), rigid body interpolation, Import Into Level node animations, viewport input gate for game scripts, glowing-ball script API (mouse buttons, rigid body velocity/force, Destroy)
- 2026-09-25: outliner folders, hierarchy asset drops, one entity per dragged asset, Content Browser multi-select, Import Into Level (flat import, combine modes, per-mesh assets), jitter fix (camera precision), texture / geometry streaming performance fixes, Stage A/B of the animation restructure
- Earlier: animation graph Steps 1-7 (graph core, canvas editor, parameters, character controller, state machine)

- Ortho views: spheres (curved shaded surfaces) look broken while flat floors look fine (reported 2026-09-27, cause not looked at: suspects are the specular / reflection view vector, IBL or mesh LOD in the ortho path). Sky is flat in ortho too.
- 2026-09-27: Editor camera fly navigation (Godot/UE5-style RMB+WASD, wheel = fly speed), gizmo mode moved from Q/W/E/R to toolbar buttons, 3D physics collider wireframe visualization (Box/Sphere/Capsule; extensible for a future terrain/heightfield collider) -- all built, untested.
- 2026-09-27: Camera FOV slider switched to horizontal (Unreal's convention -- EditorCamera itself still works in vertical FOV internally, only the Settings slider converts through the aspect ratio); world grid LOD now keys off the editor camera's orbit distance instead of its raw height above the ground, so it no longer collapses to coarse cells just from tilting to look straight down/up. Both untested.
- 2026-09-27: Cursor locked/hidden while the editor camera is flying (SDL relative mouse mode via new Input::SetRelativeMouseMode/ConsumeMouseDelta), an FPS-style look instead of the cursor visibly drifting across the screen. Untested.
- 2026-09-27: The fly-camera cursor lock replaced: SDL relative mouse mode lost against ImGui re-showing/freeing the cursor every frame in ImGui_ImplSDL3_NewFrame, so the cursor kept drifting during flight and could reappear off screen on release. Now a manual hide + warp-to-window-centre every frame while flying (Input::SetCursorVisible/WarpMouseInWindow/GetWindowCentre), which doesn't depend on ImGui cooperating. Untested.
- 2026-09-27: Reverted the per-frame cursor warp-to-centre (it broke rotation entirely, likely by warping the OS cursor onto another docked panel outside the 3D viewport each frame, off the window's actual centre). Replaced with the simpler thing originally asked for: cursor hidden while flying, restored to the exact position it was hidden from on release (one warp, not continuous); the hide is re-asserted every frame from EditorLayer::OnImGuiRender (after ImGui's own NewFrame, which otherwise re-shows it every frame and wins). Untested.
- 2026-09-27: Found and fixed the actual cause of the flicker: Input::IsMouseButtonPressed checked the WHOLE mouse button bitmask for exact equality to one button's mask, so it read as "not held" the instant any other bit was also set that frame -- true for right-click generally, not just flying. Switched to a bitwise AND test. Untested.
- 2026-09-27: The cursor-hide during fly mode switched from a raw SDL call to ImGui's own mechanism (ImGui::SetMouseCursor(ImGuiMouseCursor_None), set every frame from EditorLayer::OnImGuiRender) -- our SDL_ShowCursor/HideCursor calls were racing ImGui_ImplSDL3_NewFrame's own per-frame cursor update no matter which side of it they ran on, which is what caused the flicker. Untested.
- 2026-09-27: Fixed the actual fly-camera bug (confirmed against UE5's own viewport-controls docs: RMB alone rotates the camera in place, WASD moves, scroll while held changes speed -- the scheme built was right, the rotation math wasn't): MouseFly changed Yaw/Pitch but left FocalPoint/Distance untouched, and since Position is derived from FocalPoint - Forward * Distance, that swung Position around FocalPoint on an arc -- an orbit, not an in-place look, seen as "rotating moves the scene". Now the pre-look position is captured, and FocalPoint is put back after the look (with the new forward direction) so Position only moves by the frame's WASD offset, never by the look itself. Also: the cursor now only hides once the mouse has actually moved after right-clicking (EditorCamera::ShouldHideCursor), matching UE5 -- clicking alone (e.g. about to open a context menu) no longer hides it. Untested.
- 2026-09-27: Physics collider wireframes now draw depth-test-off (new Renderer2D::DrawXRayLine, a third thin-line batch alongside the depth-tested one, restoring depth test after) -- they were depth tested before, so a collider roughly the mesh's own size was mostly hidden behind the opaque surface, which is why it was "hard to see". The 3-great-circles sphere shape itself is the standard technique (Godot's editor uses the same one); only the occlusion was the bug. Untested.
- 2026-09-27: Double-clicking an entity in the Hierarchy frames the editor camera on it (its world bounds, sized to fit in view with padding), like UE5's Outliner; F also frames the current selection (UE5's "Frame Selected"). New EditorCamera::Focus(centre, radius), works in both perspective (sets Distance from FOV) and ortho (sets the visible half-height). Tested by the user: works.
- 2026-09-27: Found and fixed the ortho sphere bug (screenshots showed a sphere almost entirely black except a small lit spot, while a flat cube nearby looked fine): DeferredLighting.slang computed the view vector as `normalize(cameraWorldPos - worldPos)`, which only reproduces a real per-pixel view direction for a genuine point (perspective) camera -- the editor's ortho views fake a point camera far back along a true parallel-ray projection, and that difference diverged enough across a sphere's curved surface to push NdotV negative (clamped near-zero, i.e. black) over most of the visible hemisphere; a flat surface facing the camera barely showed it. Fixed generally, not by special-casing ortho: the view vector is now the direction between the same pixel unprojected at two depths through the real projection matrix (like Skybox.slang's ray direction already does), which is correct for perspective and orthographic alike. Sky still flat in ortho (a separate, already-noted issue, not touched). Untested.
- 2026-09-27: Found the real cause of the broken raster PBR (the user's own diagnostic: path tracer looks correct, raster is wrong -- pointed straight at the G-Buffer resolve, the one piece raster has and the path tracer doesn't). GBufferMaterial.slang's visibility-buffer resolve reconstructed each pixel's world position (and so its interpolated normal) via a world-space ray/triangle-plane intersection (dividing by dot(triNorm, rayDir)), deliberately chosen over the usual 2D screen-space approach to avoid near-plane clipping singularities -- but that division loses precision fast for a triangle seen at a grazing angle to the view ray, which is everywhere on a curved surface (a sphere) and nowhere on a flat one (a cube), and affects perspective and ortho alike (the earlier ortho-only view-vector fix was real but not this bug). Fixed: the pixel's own depth -- already resolved exactly by hardware rasterization in the Visibility pass -- is now read back (PushConstantVisibilityDebug gained depthTextureIndex; Renderer::addGBufferPass reads and binds resources.Depth) and unprojected the same reliable way DeferredLighting.slang gets a world position from depth, then barycentric weights are solved for that known-correct point directly (new barycentricOfPoint, no ray/plane division at all). The old ray-based computeBarycentric3D is kept only for the texture-derivative (mip level) samples, where a little imprecision barely shows. Compiles with slangc. Untested.
- 2026-09-27: Found the "everything looks wet/shiny" raster-only bug (Bistro screenshot: cobblestones, fabric awnings, painted walls all showing a strong glossy sheen; path tracer unaffected): DeferredLighting.slang's Fresnel grazing-angle reflectance (F90) used `reflectance * 50.0`, saturating F90 to full white at half the reflectance value it should. Verified against the actual reference this formula is based on (Sascha Willems' glTF PBR shader, web-searched) and against this codebase's own other two copies of the same formula (Material_PBR_Mesh.slang, Material_TransparentLit_Mesh.slang), both of which correctly use `* 25.0` -- DeferredLighting.slang was the one outlier. Fixed to `* 25.0`, matching every other copy and the reference. This is why ordinary (non-metal, low-reflectance) surfaces everywhere in the raster pipeline showed an exaggerated Fresnel rim/edge highlight; the path tracer's separate, simpler Schlick Fresnel (no F90 term, PathTracer.slang) was never affected, which is what made the comparison possible. Confirmed pre-existing, unrelated to today's other two shader fixes (the black-half ortho sphere and the grazing-angle barycentric speckling). Compiles with slangc. Untested.
- 2026-09-27: Found the real "everything is shiny/disco" bug (the user's own test -- forcing Roughness to 1.0 and Metallic to 0.0 fixed it completely -- proved the BRDF math itself was fine and pointed straight at the material DATA, not the shader): MeshImporter.cpp only reads a glTF primitive's material inside `if (primitive.material >= 0 && ...)`; a primitive with none assigned (easy to miss in Blender, e.g. a quick test sphere with no material slot) is left at MaterialData's plain struct defaults in DataTypes.h, which had RoughnessFactor = 0.0 -- a perfect mirror. Any such primitive rendered as a mirror reflecting the environment (including Bistro's many small string lights) instead of a plain matte surface, which is exactly the "disco ball" look, everywhere it happened (both the test sphere and, presumably, whichever Bistro primitives have no material). Fixed the default to 1.0 (glTF's own spec default for roughnessFactor, and the sane fallback regardless). MetallicFactor's default (0.0, non-metal) was left alone -- already correct/sensible. This only affects future imports; already-cooked materials (the sphere, Bistro) keep their old 0.0 value baked in and need re-importing (delete the cooked .nmat + registry entry) to pick up the fix, or a manual Roughness fix in the Inspector like the user already found. This closes the "everything looks shiny" investigation -- three real, separate raster bugs found and fixed today (ortho black-half normals, grazing-angle barycentric speckling, this missing-material roughness default); the earlier Fresnel R90=50->25 fix was real but a no-op for ordinary materials, noted for correctness only. Untested.
