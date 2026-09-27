# Backlog -- one list of everything that is open (kept up to date; started 2026-09-26)

> How to use: pick an item, say its number/name, and it is built the way the plans say (phase gates: the user builds and tests, nothing is
> built by me). When an item is done, move it to "Done" with the date. Details live in the plan doc named in each line -- this file is
> only the index. Newest decisions win over older notes.

## Now (in progress)

| # | Item | Notes |
|---|---|---|
| N4 | **World units and world tools** (grid, snap, ortho views, measure, bounds readout, unit conversion at the boundaries, physics unit interface) | plan written 2026-09-26: `Units_And_World_Tools_Plan_2026.md`; decisions taken 2026-09-26 (cm, tools first); **U1 (WorldUnits) and the first part of U2 (Inspector size readout, snap toolbar) built, untested**; world grid (Godot port) + snap feedback line built, being tuned; world grid, snap feedback, Measure tool and reference figure built (U2); U1-U6 built, plus per-field unit display/editing (Transform Position, script [Expose] Unit) -- testing next. Replaces the old E1 idea of a bare "model size in the import dialog" (that is phase U4 of the plan) |

## Prefabs (plan: `Prefab_Architecture_Plan_2026.md`, section 8)

| # | Item |
|---|---|
| P1 | **Apply of a nested instance's change into the OUTER prefab** (today Apply on the inner instance writes into the inner prefab) |
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
