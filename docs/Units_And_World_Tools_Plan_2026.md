# World Units and World Tools Plan -- September 2026

> Status: **PLAN, nothing built.** Written 2026-09-26 with the user, after the Fox (authored in centimeters, declared as meters by its glTF)
> came in 100x too big and cost a long debugging session. Same working rules as the other plan docs: the user builds and tests, phase gates,
> no compatibility code (the user deletes cooked files and re-imports; scenes are converted once or rebuilt), class layout rules, Vulkan never
> leaks outside NRI. Index: `Backlog_2026.md`.

---

## 1. The problem

- Sizes are only known by looking. The engine's world unit is implicit (meters, because glTF and Jolt are), the importer takes a bare
  "Import Scale" number, and nothing in the editor shows how big something is. A model authored in centimeters arrives 100x too big.
- There is no editor tooling to check a size: no ortho views, no ruler, no grid, no snapping.
- Physics (Jolt) is tuned for meters. If the world unit ever changes, or another physics backend is added, the boundary must convert.

Goals: (1) a **single, explicit world unit** everything derives from; (2) **conversion at the boundaries** (import, physics, denoiser, shaders), never
scattered magic numbers; (3) **tools that let you verify sizes** (grid, snap, ortho views, measure, bounds readout, a reference human);
(4) a physics interface that **knows its backend's unit** so other engines can be added.

---

## 2. What the others do (researched 2026-09-26)

| | Unit | Notes |
|---|---|---|
| **Unreal Engine 5** | **1 unit = 1 cm** | glTF is meters: import with an **Import Uniform Scale of 100** ([UE glTF import docs](https://dev.epicgames.com/documentation/unreal-engine/importing-gltf-files-into-unreal-engine?lang=en-US), [UE5 export guide](https://sarahhyperdense.substack.com/p/blender-to-ue5-the-complete-export)). Its physics (Chaos) works in the same units. Viewport snapping: a **grid snap** with a dropdown of sizes (1, 5, 10, 50, 100, ... in the viewport toolbar), a **rotation snap** (5, 10, 15, 30, 45, 60, 90, 120 degrees) and a **scale snap**, all configurable in Editor Preferences > Viewports > Snap ([UE snapping docs](https://dev.epicgames.com/documentation/unreal-engine/actor-snapping-in-unreal-engine), [viewport toolbar](https://dev.epicgames.com/documentation/unreal-engine/viewport-toolbar)). Orthographic viewports (top / side / front) are standard; I did not verify UE's exact measuring tool, so this plan designs its own. |
| **Unity / Godot / Blender / glTF** | 1 unit = 1 m | glTF's spec is meters; Jolt, PhysX, Unity and Godot are tuned for meters. |
| **USD / Omniverse** | per-stage `metersPerUnit` | The unit is data on the scene, not a hard-coded constant ([units in OpenUSD](https://docs.nvidia.com/learn-openusd/latest/beyond-basics/units.html)). Both cm and m are common. |
| **Jolt Physics** | **SI: meters, kg, seconds** | No unit switch. It works best with dynamic objects of 0.1-10 m, speeds up to 500 m/s, gravity up to ~10 m/s^2, static objects 0.1-2000 m; penetration slop, speculative contact distance and sleep thresholds are in meters ([Jolt docs](https://jrouwe.github.io/JoltPhysics/)). The right answer for another unit is to **scale at the boundary**, not to retune Jolt. |
| **NRD** (in this repo, `NRDSettings.h`) | meters by default | "**if unit is not meter, all default values must be converted from meters to units**": every distance in its settings (denoising range, thresholds, blur radius...) must be scaled if the world is in cm. |

Consequence: with a cm world, the boundaries that need a x100 / x0.01 are **import, physics, NRD, and every distance constant in shaders and
C++ code** (section 5). The unit choice does not change numeric precision (float precision is relative, 1 km costs the same in cm or m).

---

## 3. Decisions to confirm (with recommendations)

1. **The internal world unit.** You asked for **1 unit = 1 cm** (UE convention: integer-friendly sizes, UE's snap list and tutorial values apply).
   Recommendation: **yes, cm, but as ONE project setting** (`WorldUnits::PerMeter`, default 100) that all engine code and a shader constant read,
   never as scattered `* 100`. Then the whole engine is unit-agnostic; switching to meters later is changing one number plus re-running the audit.
   (1 m as the default would be equally valid; the cost of cm is the audit in section 5, paid once.)
2. **Build order.** Recommendation: **tools first, unit switch last** (section 6): the grid, snap, measure tool, bounds readout and ortho views
   work in either unit and are exactly what verifies the switch afterwards.
3. **Ortho rendering scope.** Recommendation: orthographic views use the raster path only (no ray/path tracing, no DLSS) at native resolution.
4. **Grid sizes** (in cm): 1, 5, 10, 50, 100, 500, 1000, 5000, 10000 (UE-style). Rotation snap 5, 10, 15, 30, 45, 60, 90, 120; scale snap 0.1, 0.25, 0.5, 1.
5. **Physics interface.** The backend declares its native unit; the scene converts at one place (section 4.2).
6. **Existing content.** No compatibility code: cooked meshes and registry entries are deleted and re-imported; scenes / prefabs are converted once
   by a small script (translations, collider sizes, light ranges, camera clip planes x100) or rebuilt by hand.

---

## 4. Architecture

### 4.1 `WorldUnits` (engine, single source of truth)

```cpp
namespace Nox::WorldUnits
{
    float PerMeter();            // project setting, default 100 (1 unit = 1 cm); read once at project load
    float FromMeters(float);     // 1.8 m -> 180 units
    float ToMeters(float);
    // Display: "180 cm", "1.8 m", "12.5 km" in the unit the editor shows (Settings: cm / m / auto).
    std::string Format(float worldLength);
    enum class SourceUnit { Meters, Centimeters, Millimeters, Inches, Feet };
    float PerSourceUnit(SourceUnit);   // world units per one source unit (importer)
}
```
- `PerMeter` is a **project setting** (saved in the project file), also uploaded to the frame constants (`ubo->unitsPerMeter`) for shaders.
- Nothing else in the engine names centimeters or meters; code that needs a physical length says `FromMeters(0.34f)` (a 34 cm character radius).

### 4.2 Physics unit boundary

- `IPhysics3DScene` gets `virtual float NativeUnitsPerMeter() const = 0;` (Jolt: 1) and the factory takes the world's `PerMeter`.
- A small `PhysicsUnits` helper (owned by the backend base) holds `scale = NativeUnitsPerMeter / WorldUnits::PerMeter()` (0.01 for a cm world on
  Jolt) with `ToNative(length)`, `ToWorld(length)` for positions, velocities, forces/impulses (they scale with length; mass is unchanged) and
  distances; angles are never scaled.
- The **backend** applies it where lengths cross the boundary: body / shape creation (positions, box half extents, radii, offsets), character
  controller (radius, height, step height, floor step-down, velocities), gravity, ray / shape casts (origin, distance, hit points), overlap queries,
  forces / impulses / velocities set and read, and the pose sync back into transforms. Jolt's own tolerances stay in meters, untouched.
- Adding a backend = implement the interface in ITS unit and declare `NativeUnitsPerMeter`; nothing above the interface changes.

### 4.3 Import units

The import dialog's bare **Import Scale** becomes:
- **Source Unit** dropdown (Meters -- the glTF default --, Centimeters, Millimeters, Inches, Feet, Custom); the factor to world units is `PerSourceUnit`.
- **Model size readout** (from the glTF's accessor min/max and node transforms, before importing): width x height x depth in the display unit, and a
  sanity hint ("2.4 m wide, 80 m tall: probably authored in cm").
- **Fit to height**: type the wanted height (e.g. 80 cm) and the extra multiplier is worked out.
- The old **Import Scale** stays as an extra multiplier after the source unit. Content Browser mesh details show the same bounds.

### 4.4 Editor tools

| Tool | Behavior |
|---|---|
| **Grid** | A procedural infinite grid on the ground plane in perspective, on the view plane in ortho; minor / major lines from the chosen snap size (adaptive: never denser than ~8 px), axis colors, distance fade. Drawn in the forward pass with depth test; sizes from `WorldUnits`. |
| **Snap** | Toolbar toggles + dropdowns for **location** (1, 5, 10, 50, 100, 500, 1000, 5000, 10000 cm), **rotation** (5..120 deg) and **scale** (0.1..1). Feeds the gizmo (ImGuizmo takes snap values), the drop point of dragged assets and prefabs, and "move to grid". |
| **Ortho views** | View menu in the viewport header: Perspective, Top, Bottom, Front, Back, Left, Right (numpad shortcuts). Pan (MMB / Alt+drag), zoom at the cursor, orthographic projection in the editor camera; the gizmo constrains to the view plane. Raster only, no RT / DLSS (decision 3). |
| **Measure** | Click point A, click point B: a line with the distance and the X / Y / Z deltas in the display unit. Points come from the depth buffer under the cursor (world position readback like the entity pick) or, with nothing under it, from the grid plane; snapped to the grid when snap is on. Drawn as an overlay (ImGui draw list) so it needs no renderer work. Esc clears. |
| **Snap to floor** | Select and press **End** (UE): the entity drops straight down (world -Y) until the bottom of its world bounds touches the first surface below it (a physics ray / depth pick from the bounds' bottom center); works on a multi-selection (each drops on its own or as a group by the lowest one). Uses the mesh bounds already computed for the size readout. Toolbar / menu entry too. |
| **Bounds readout** | The Inspector shows the selected entity's world bounding-box size ("45 x 180 x 30 cm") next to its transform. |
| **Reference figure** | Toggle: a 180 cm wire figure (line renderer) at the origin / under the cursor, visible in every view, to compare sizes at a glance. |
| **Editor camera** | Fly speed and clip planes in world units via `WorldUnits`. |

### 4.5 Shaders

`ubo->unitsPerMeter` replaces every literal that is a distance in meters (ray offsets, TMin, blur / bias radii, DDGI spacing...). The audit (section 5)
lists them; each becomes `x * ubo->unitsPerMeter` or a named constant derived from it.

---

## 5. The audit (what must be converted for a cm world)

Found in the code as of 2026-09-26; each is checked, converted through `WorldUnits`, and named in the log of its phase:

| Area | Items |
|---|---|
| **Physics** | gravity -9.81, `mStickToFloorStepDown` 0.5, character defaults (radius 0.34, height 1.08, step 0.45, accel 20.48, braking 20), rigid body damping unaffected, `CreateBody` shapes / offsets, ray / overlap distances, script API velocities / forces |
| **Scripts** | `CharacterMovement` defaults (walk 2, run 5, jump 7 m/s), `CameraFollow` Distance / Height, `Shooter` speed / spawn distance |
| **Renderer** | camera near / far (0.001 / 1000 in the scene camera), editor camera clip planes and speeds, DDGI grid origin and spacing (1.8 / 1.4 / 1.7), light ranges and radii, path tracer / shadow ray offsets and TMin (0.005 / 0.02 in shaders), RTXDI / ReSTIR distance and depth thresholds, LOD error thresholds (pixel based: fine), NRD settings (denoising range, thresholds, blur radius: **all** "meters" defaults), DLSS / Streamline near / far, sky dome radius |
| **Editor** | Import Scale defaults and buttons (m / cm / in), gizmo sizes, the viewport grid, placement plane distance, the Nox Stats world-size text |
| **Assets** | cooked meshes (vertex positions are in the unit they were imported into): re-import; materials carry no lengths except IOR / thickness (thickness is a length: scale it) |
| **Scenes / prefabs** | Translation, collider half extents / radii / offsets, light Range / Radius, camera clip planes, script float fields that are lengths (by hand) |

A helper script greps the sources for float literals near the words range / distance / radius / bias / epsilon / near / far to keep the list honest.

---

## 6. Phases (each ends with a user test)

| Phase | Deliverable | User test |
|---|---|---|
| **U1 -- Foundations** | `WorldUnits` (project setting `PerMeter`, currently 1 so nothing changes; `FromMeters`, `Format`, `SourceUnit`), display-unit setting, `ubo->unitsPerMeter` uploaded (unused yet). | Builds; nothing looks different. |
| **U2 -- Tools in the current units** | Grid + snap (location / rotation / scale, toolbar) in perspective, snap on drag-drop placement, Inspector **bounds readout**, **Measure** tool (depth pick + grid plane, overlay), reference figure. | Measure the Fox and a cube: sizes read correctly in m or cm; grid follows the snap size; drag an asset and it lands on the grid. |
| **U3 -- Ortho views** | Editor camera orthographic modes (top / bottom / front / back / left / right), navigation, grid on the view plane, gizmo constrained to the plane, raster-only rendering path for ortho. | Switch to Front: the Fox and the reference figure side by side, measure heights. |
| **U4 -- Import units** | Source Unit dropdown, model size readout and hint in the import dialog, Fit to height, Content Browser bounds. | Import the Fox: the dialog says ~80 (cm) tall; pick Centimeters and it comes in at the right size. |
| **U5 -- Physics unit boundary** | `NativeUnitsPerMeter`, `PhysicsUnits`, Jolt converts at the boundary (scale still 1: no behavior change). | Balls, character and shooter behave exactly as before. |
| **U6 -- The switch to cm** | `PerMeter` = 100; the audit converted item by item (physics scale 0.01, NRD, shaders, DDGI, camera, lights, character / script defaults); one-off conversion of scenes and prefabs; assets re-imported. | The Fox, the floor, the balls and the glowing ball look and behave as before; measure and ortho views agree with the sizes; denoising and GI unchanged. |

**Why this order:** U1-U4 are additive and low risk and give the measuring tools the user asked for; U5 restructures physics without changing
behavior; U6 is the one risky switch, done last with everything needed to verify it already in place.

---

## 7. Risks

- **U6 is a wide change made blind** (no compiler here): the audit list is the safeguard, and each converted constant is reported.
- **Ortho in a perspective-assuming renderer**: lighting uses a per-pixel view vector, the sky uses view rays, motion vectors assume perspective;
  the ortho path must turn those off / use the constant view direction (raster only), and the temporal effects reset when the mode changes.
- **NRD and ReSTIR distance thresholds** are easy to miss and show up as denoiser artifacts, not errors.
- **Scripts with lengths in float fields** cannot be converted automatically.

## 8. Status log

- 2026-09-26: plan written.
- 2026-09-26: **decisions by the user:** (1) world unit = **1 cm, like UE5**, as the default project setting; (2) build order: whichever tests best first (recommended and taken: tools first, the unit switch last); (3) ortho views raster-only (no RT / DLSS) accepted -- reason: both assume a perspective camera (parallel rays in ortho), and UE's ortho views also use simpler shaded / wireframe modes; (4-6) as recommended.
- 2026-09-26: **the measure tool works like UE's (user's description):** in an ortho view click a first point, a line follows the cursor until the next click, the length is written next to the line in cm; with grid snap on the points snap to the grid. Kept on screen until the next click; also works in perspective views (depth pick).
- Until U6 the project setting `PerMeter` is **1** (the engine really is in meters): the tools convert through it, so sizes read in cm today (1.8 units = "180 cm") and nothing changes when U6 sets it to 100.
- 2026-09-26: **U1 built (untested):** `WorldUnits` (`Core/WorldUnits.h/.cpp`: `PerMeter` project setting `WorldUnitsPerMeter`, saved in the project file, default 1 until U6; `FromMeters` / `ToMeters` / `FromCentimeters` / `ToCentimeters`, `Format` with a display unit cm / m / auto, `SourceUnit` + `PerSourceUnit`); set when the project loads.
- 2026-09-26: **U2 first part built (untested):** mesh bounds (`MeshHandle::boundsMin/Max`, from the vertices at upload, not saved) and `ComputeWorldBounds` (an entity's meshes and everything below it, world axes) -> the Inspector's Transform shows **Size: W x H x D** in the display unit; **snapping**: a toolbar at the top left of the viewport (Snap switch, location step 1 / 5 / 10 / 50 / 100 / 500 / 1000 / 5000 / 10000 cm, rotation step 5..120 deg, scale step 0.1..1, display unit cm / m / auto), Ctrl inverts it while held, the gizmo uses the chosen steps, and dragged assets / prefabs land on the grid when snap is on. Still to do in U2: the world grid drawing, the Measure tool (perspective, depth pick), the reference figure.
- 2026-09-26: user checked the U2 snap toolbar and size readout: snapping (gizmo and prefab / asset placement) works. Added **Snap to floor (End)** to the plan (section 4.4); the grid, the Measure tool and the reference figure are the rest of U2. The grid and the reference figure are drawn by one small editor-only pass after the scene (depth tested); the measure line is an ImGui overlay (no pass).
- 2026-09-26: **U2 grid + Snap to Floor built (untested).** *Grid*: `Renderer2D::DrawThinLine` (a batch of 1-pixel lines drawn by the same LineMesh pipeline after the thick ones, from the same storage buffer through an address offset; the buffer is now bounds-checked) and `EditorLayer::DrawWorldGrid` in `OnOverlayRender` (not in Play): cells of the snap step, x10 while a cell would be under ~12 px (sparser far from the ground), every tenth line stronger, X axis red / Z axis blue, faded with distance in 12 pieces per line, depth tested by the 2D overlay pass; the centre follows the camera in whole major cells. Toolbar: a **Grid** switch. *Snap to Floor*: **End** key or the toolbar's **To Floor**: each selected top entity drops until its world bounds' bottom rests on the top of the highest other mesh whose bounds overlap it in the ground plane and lie below its middle (bounding boxes), else on y = 0 (`ComputeWorldBounds(..., includeChildren)`). Still to do in U2: the Measure tool, the reference figure.
- 2026-09-26: **U2 grid replaced by a port of Godot's editor grid** (after the CPU thin-line grid hurt and several own shader grids looked wrong, user: "fully copy Godot"): `Renderer2D::DrawGrid(camera, cell)` works out Godot's `_init_grid` numbers (8 steps, levels 0..2, bias -0.2; the smallest cell is max(snap step, 1 m) so a 10 cm snap does not shrink the grid) and `GridMesh.slang` draws 2 x 401 real 1-pixel lines (one mesh shader group each, positions and colors made in the shader; the axes X red / Z blue) with Godot's angle and distance fade. Colors are brighter than Godot's (0.78 / 0.62, alpha .75 / .55) and secondary lines keep 40 % strength through the cross fade, both on purpose (lit floors). `DrawThinLine` stays in Renderer2D (used by later tools). The translation snap feedback is an ImGui measuring line from the drag start to the object with end bars and the distance in the middle (`DrawTranslationSnapFeedback`). The grid and Godot's numbers are still to be confirmed by the user.
- 2026-09-26: **U2 Measure tool built (untested).** Renderer: the pick readback pass also copies the depth of the picked pixel (`DepthHi` -> `PickerDepthStaging`, 1x1; `TextureVK::copyImageToBuffer` uses the depth aspect for depth textures) and `Renderer::getPickedDepth(x, y, depth)` returns it (reverse-Z infinite projection: depth = near / view depth; 0 = nothing drawn). Editor: **M** or the toolbar's **Measure** switch, click the first point, a yellow line with end bars follows the cursor (the distance in the middle, in the display unit), click the second point, it stays until the next click; **Esc** drops the line, then leaves the tool. Points: the surface under the cursor (`PickWorldPoint`: pixel ray x near / depth / cos to the camera forward), else the y = 0 plane; snapped like the gizmo (Snap switch, Ctrl inverts). Left clicks do not select while the tool is on. Still to do in U2: the reference 180 cm figure.
- 2026-09-27: **Measure tool tuned with the user:** the world point is unprojected from the picked depth with the renderer's own projection * view and the renderer's output size (the pixel -> NDC mapping used the ImGui panel size before, which was the bug: lengths varied with angle and zoom); the pick copies a 9 x 9 depth block and takes the nearest drawn pixel within 4 px, so a cursor just off an object's silhouette measures the object, not the floor behind it (farther off: the y = 0 plane, like Godot's fallback); debug text removed. **Reference figure built (untested):** toolbar "Figure", a 180 cm blocky wire figure at the origin (thin lines, sizes in cm through WorldUnits). U2 is done once the user confirms it; next: **U3 ortho views**.
- 2026-09-27: **U2 confirmed by the user** (reference figure and Measure tool agree with the sizes; measured 20 cm difference from the figure to a cube top as expected). **U3 ortho views built (untested):** `EditorCamera` has `EditorViewMode` (Perspective, Top, Bottom, Front, Back, Left, Right): the orientation is fixed by the mode, the camera sits 5000 units from the focal point along the view axis (the shaders' camera position then gives a nearly parallel view vector; no separate ortho lighting), the projection is a reverse-Z orthographic matrix (near 0.1, far 10000 from the camera; ImGuizmo gets `glm::ortho`), pan moves 1:1 with the mouse, scroll / RMB-drag zoom the visible height (about 11 % per notch, around the centre), Alt+LMB rotation is off in ortho. Renderer: `m_orthographic` (set in `BeginScene(EditorCamera)`): no camera jitter (it offsets perspective terms) and the mesh LOD cut is forced to full detail; the editor turns DLSS, ray tracing and path tracing off when it enters an ortho view and restores them when it leaves (`EditorLayer::SetViewMode`); Play always starts from the perspective view. UI: a view combo in the viewport toolbar and numpad keys (7 top, 1 front, 3 right, Ctrl = the opposite side, 5 = perspective). The Godot grid port now works on any plane (`PushConstantGrid::planeAxis`: Top / Bottom y = 0, Front / Back z = 0, Left / Right x = 0; its level follows the ortho size like Godot's); axis lines are colored by world axis (X red, Y green, Z blue). The Measure tool picks with parallel rays in ortho. Not done for ortho: zoom at the cursor, frame selected, wireframe / simpler shading modes, sky rays (the sky uses the far camera, so it looks flat).
- 2026-09-27: **U4 import units built (untested).** `MeshImporter::InspectGltf` also returns the size of what the default scene draws in the file's own units (the meshes' POSITION accessor min / max through the node transforms; no scene: the root nodes; no nodes: the meshes as they are). The import dialog (`ContentBrowserPanel`, read when it opens: `ReadImportFileInfo`): **Source Unit** combo (Meters, Centimeters, Millimeters, Inches, Feet, Custom) sets `ImportScale` through `WorldUnits::PerSourceUnit` (so it stays right after the switch to cm), the **Import Scale** drag stays (turns the combo to Custom), a **Size: W x H x D** readout in the display unit, an orange warning when the largest side is over 100 m or under 2 cm (the "wrong unit" cases), and **Fit to height (cm)** + **Fit**, which sets the scale so the model is that tall (the file's Y size). The m / cm / in buttons are gone (the combo replaces them). Big files: parsing them when the dialog opens takes as long as the import's own inspect step (a few seconds for Bistro). Next: U5 physics unit boundary.
- 2026-09-27: **U5 physics unit boundary built (untested, scale still 1: no behavior change expected).** `IPhysics3DScene::NativeUnitsPerMeter()` (Jolt: 1.0, meters) plus protected `ToNative`/`ToWorld` helpers on the interface (`PhysicsScale() = NativeUnitsPerMeter() / WorldUnits::PerMeter()`, recomputed each call so a project's `WorldUnitsPerMeter` can change without a stale cache) -- any future backend gets them for free. `JoltPhysics3DScene.cpp` converts every length that crosses the boundary: body / character positions in and out, collider half extents / radii / offsets, character radius / height / step height / jump speed / move velocity (world-unit, script-facing fields -> native at the one point each enters Jolt's own velocity math), ray / sphere cast origins, distances and hit / contact positions, `AddForce` / `AddImpulse` / `SetLinearVelocity` / `GetLinearVelocity`. Left untouched on purpose: rotations, angles (`MaxSlopeDegrees`), angular velocity (rad/s, not a length), dimensionless factors (friction, damping, restitution, gravity/air control factors), and Jolt's own internal tolerances not derived from a component field (gravity -9.81, `mCharacterPadding`, `mPredictiveContactDistance`, `mPenetrationRecoverySpeed`, `mStickToFloorStepDown` 0.5, `mMaxStrength`) -- these are written directly in Jolt's native meters and stay correct at any world unit. **Open risk for U6**, noted rather than fixed here: `shapeSettings.mDensity = rb.Mass` couples a body's mass to its shape's native-unit volume; once the world switches to cm and native geometry shrinks by ~100x per axis, native volume drops ~10^6x, so the same `rb.Mass` number will give a wildly different mass unless `CreateBody` is changed to set mass directly (`JPH::MassProperties` / `mOverrideMassProperties`) instead of density. Next: **U6, the switch to cm** (the audit table, one-off scene/prefab conversion, re-import).
- 2026-09-27: **U6 (the switch to cm) built (untested).** User confirmed: no rescale/migration tool for existing scene data either -- same "no compatibility code" rule as cooked assets ([[feedback-no-compatibility-code]] updated); delete cooked files + registry, re-import, and re-place/re-tune existing entities by hand at the new scale.
  - **`WorldUnitsPerMeter` default is now 100** (`Project.h`): a project file (like Facerun's) that never wrote the key gets 100 from this default the moment it loads -- the switch itself needs no file edit.
  - **Mass/density fix (blocking bug found in U5):** `JoltPhysics3DScene::CreateBody` no longer sets `shapeSettings.mDensity = rb.Mass` (mass computed from density x the shape's *native* volume would have changed ~10^6x when cm geometry made native volume ~100^3 smaller). `BodyCreationSettings::mOverrideMassProperties = CalculateInertia` with `mMassPropertiesOverride.mMass = rb.Mass` sets the mass directly, independent of geometry scale.
  - **Component defaults through `WorldUnits::FromMeters`** (real lengths that a fresh entity/light/camera hits immediately, `Components.h`): `CharacterController3DComponent` (Radius, Height, StepHeight, MaxAcceleration, BrakingDeceleration -- not MaxSlopeDegrees/GravityScale/AirControl, not lengths), `PointLightComponent` / `SpotLightComponent` (Range, Radius). `SceneCamera`'s perspective near/far the same way (its orthographic values are a 2D/sprite convention, left alone).
  - **`EditorCamera`** near/far clip and default orbit distance are NOT default-member-initialized through `WorldUnits` (it is a long-lived object built before any project -- and so its `PerMeter` -- is known): new `EditorCamera::ApplyWorldUnitDefaults()`, called from `EditorLayer::OpenProject` once `Project::Load` has actually set the unit.
  - **`ubo->unitsPerMeter`** added to `UniformBufferObject` (shaderIO.h), filled from `WorldUnits::PerMeter()` in `Renderer::updateUniformBuffer`. Every hardcoded meters-assuming ray offset / TMin found is now `x * ubo->unitsPerMeter`: `DDGIRadiance.slang`, `DeferredLighting.slang`, `PathTracer.slang` (NEE shadow rays, refraction/specular/diffuse bounce origins), `Reflection.slang`, `ShadowMask.slang`. Confirmed compiling with `slangc` per shader stage.
  - **C# script defaults** (`CharacterMovement`, `CameraFollow`, `Shooter` in Facerun): scripts have no `WorldUnits` access, so their `[Expose]` length defaults are hand-multiplied by 100 (walk/run/jump speed, camera Distance, shooter Speed/SpawnDistance) -- comments now say cm, not m. This only fixes freshly-attached scripts; any instance whose fields were tuned and saved under the old scale needs re-tuning by hand (same as the rest of the scene).
  - **Not audited / left alone, flagged as risk to watch:** RTXDI / ReSTIR internal distance and depth thresholds and NRD's own hit-distance parameters beyond what's listed above (no explicit meters-based override was found in the renderer's NRD setup -- it currently relies on the library's own defaults, unchanged by this work); DDGI grid origin/spacing defaults (calibrated to the existing Bistro layout, which is being rebuilt anyway -- re-tune via the Settings sliders after re-import); collider component defaults (Box/Sphere/Capsule 0.5, Offset 0,0,0) are proportions to a default 1-unit primitive mesh, not meters, so left as raw numbers. **This closes the plan's phase list (U1-U6).** Remaining follow-up if anything looks or feels off after testing: watch for GI/denoiser artifacts (the unaudited NRD/RTXDI thresholds) and self-intersection acne on curved surfaces near ray-offset code not covered above.
- 2026-09-27: **Per-field unit display/editing built (untested)**, after the user asked how a car's km/h-scale speed avoids becoming "200000" once the world is cm -- UE5's answer is per-property "Units" metadata that the Details panel converts through, storing the raw value unchanged; built the same idea here, in two places:
  - **Transform Position** (`SceneHierarchyPanel::DrawPositionControl`): shown and edited in the Settings display unit (cm / m / auto, the same setting the bounds readout uses), converted to/from `component.Translation` (still raw world units) on read/write; the drag step scales with the unit (0.1 m or 10 cm per pixel) so dragging feels the same either way. Rotation (an angle) and Scale (a ratio) are untouched -- only Position is a length.
  - **Script `[Expose]` float fields**: new `Unit` field on `Nox.ExposeAttribute` (`NoxScriptCore/Source/ExposeAttribute.cs`; a plain string field, not a nullable/property -- Coral's attribute marshalling only reads public FIELDS by name and mishandles a null string), read in `DotNetBackend::LoadModule` (`attribute.GetFieldValue<std::string>("Unit")`) into a new `ScriptFieldInfo::Unit`. The script component Inspector converts a Float field tagged `"cm"`/`"m"`/`"mm"`/`"cm/s"`/`"m/s"`/`"km/h"` through `ScriptUnitFactor` (world units, or world units per second, per display unit) before showing/editing it; the field the script reads (`WalkSpeed`, `Distance`, ...) is unchanged, still raw cm. `CharacterMovement`, `CameraFollow` and `Shooter` in Facerun now carry `Unit = "m/s"` / `"m"` so their speeds and distances are typed in m/s and m, not raw cm/cm-per-second, while still running on the cm values from the U6 pass. This is scope beyond the original U1-U6 plan (a direct follow-up the user asked for after seeing U6's raw-cm numbers) -- not itself part of the phase list above.
- 2026-09-27: **Import scale now bakes into cooked static geometry (untested)**, after the user asked why an imported cube showed Scale 50 and pressing the Inspector's reset button (hardcoded to 1.0) broke it -- traced to two things: (1) a real bug, the Source Unit combo showed "Meters" by default without actually applying the meters factor until touched (fixed, `ContentBrowserPanel::ReadImportFileInfo` now sets `ImportScale` up front); (2) by design, `ImportScale` was applied as the placed entity's `Transform.Scale`, so it always carried the unit-conversion factor instead of staying near 1.0 like UE5. Asked the user; chose UE5's way. `MeshImporter::ApplyImportScale` (new) multiplies every vertex position and every cooked node's local Translation (not Rotation/Scale) by `ImportScale`, called from `CookMesh` (isStatic) and `CookSplitMeshes` (`!skinned && !level` -- **not** for Import Into Level, which scales the whole placed scene, meshes+lights+cameras, uniformly through the level root's own `Transform.Scale` in `ModelInstance::SpawnLevel`; baking into the geometry too would double it). `EditorLayer`'s two drag-in placement sites now only apply `ImportScale` to the placement `Transform.Scale` for `AssetType::Mesh` (skeletal -- not baked yet); a static mesh instance now starts at Scale 1.0, so the Inspector's reset-to-1.0 button is correct again. **Not baked**: skeletal meshes (bind pose / animation translation channels would need the same treatment, a separate, riskier pass) -- still scaled via the placement Transform, as before. **Known follow-up**: any existing scene entity with a hand-sized collider matching its old ImportScale-carrying Transform.Scale will need the collider re-sized once that entity's Scale resets to 1.0 -- re-import and re-place, per the no-compatibility-tooling rule.
- 2026-09-27: **Skeletal import scale baked too (untested)**, closing the "static only" gap from the previous entry. `MeshImporter::ApplyImportScale` now runs unconditionally in `CookMesh` (static and skeletal alike) and in `CookSplitMeshes` whenever `!level` (still excluded for Import Into Level, same double-scale reasoning as before). New `ApplySkeletonScale` (`Node::RestTranslation`, and `RestMatrix`'s translation column when a node used `HasRestMatrix`, scaled; `Skin::InverseBindMatrices` scaled on its rotation/scale columns only -- (Scale(S) . M)^-1 = M^-1 . Scale(1/S), a right-multiply by a diagonal scale touches columns 0-2, not the translation column 3) and `ApplyAnimationScale` (`NodeAnimationChannel::PositionKeys` only -- rotation and scale keys are untouched) run before `SkeletonSerializer`/`AnimationSerializer` write the `.nskel`/`.nanim` files, in both cook paths. `EditorLayer`'s two drag-in placement sites no longer special-case `AssetType::Mesh`: no mesh kind gets `ImportScale` applied through the placement `Transform.Scale` any more, all of it is baked. The Fox needs deleting (cooked `.nmesh`/`.nskel`/`.nanim` + registry entries) and re-importing to pick this up.
- 2026-09-27: **Per-field unit display extended to every length-carrying component (untested)**, after the "half extents in cm now?" / "isn't half extents like scale, 1=100%?" confusion (a collider size is an absolute length like Position, not a ratio like Scale -- unlike Scale it needs the same cm/m display treatment). `DrawPositionControl` generalized into reusable `DrawLengthControl` (a scalar length, optional `rateSuffix` for e.g. "/s^2") and `DrawLengthVec3Control` (a vec3 length), both sharing `GetLengthUnit` (the same cm/m/auto logic Position already used). Wired into every raw length field the Inspector drew: `BoxCollider3DComponent` (Half Extents, Offset), `SphereCollider3DComponent` / `CapsuleCollider3DComponent` (Radius, Half Height, Offset), `CharacterController3DComponent` (Radius, Height, Step Height, Max Acceleration/Braking Deceleration with "/s^2" -- not Max Slope/Gravity Scale/Air Control, not lengths; Velocity, read-only), `PointLightComponent` / `SpotLightComponent` (Range, Source Radius -- the old hardcoded "%.2f m" label is gone, it's dynamic now), `CameraComponent`'s perspective Near/Far (its orthographic Near/Far/Size stay plain numbers, a 2D-camera convention, not physical lengths -- same call made for `SceneCamera` back in U6). Left alone on purpose (not lengths): Mass, damping, gravity factor, friction, restitution, angles, intensities, exposure.
