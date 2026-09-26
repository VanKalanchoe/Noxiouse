# Prefab Architecture Plan (Stage C) -- September 2026

> Status: **PLAN, nothing built.** Written 2026-09-26 with the user. Follows the collaboration contract of the other plan
> docs: the user builds and runs, phase gates (each phase ends with something the user tests), no compatibility code
> (delete and re-save when a format changes), class layout rules (public / private functions / private members), Vulkan never
> leaks outside NRI.
>
> Related: `Animation_Graph_Architecture_Plan_2026.md` section 7 (Stage A/B, `ModelInstance`, Import Into Level, outliner
> folders) and `Engine_Architecture_Plan_2026.md`.

---

## 1. What a prefab is (plain words)

A **prefab** is a *saved recipe of entities*. You build something once in the scene, save it as a file, and then place
copies ("instances") of it. Every instance stays connected to the file: change the file and all instances change.

Without prefabs, twenty balls in a level are twenty independent entities: to change the friction of all of them you
edit twenty times. With a prefab you edit the recipe once.

### Example with the assets in this project

| Prefab | What is inside (the recipe) | Why it is a good prefab |
|---|---|---|
| **Ball** | `Sphere.nsmesh` + Material + `RigidBody3D` (Dynamic, angular damping 0.5) + `SphereCollider3D` (radius 1) | Scatter 20 balls in the level. Later change the damping in one place and all 20 balls change. Each ball keeps its own position. |
| **Player** | Fox skeletal mesh + `AnimatorComponent` (graph `Fox_Locomotion`) + `CharacterController3D` + script `CharacterMovement` + a child `Camera` with script `CameraFollow` (target = the Fox inside the same prefab) | Drop a fully working, animated, controllable character into any level in one drag. Today this takes six manual steps. |
| **Floor** | `Cube.nsmesh` scaled 100/1/100 + `RigidBody3D` (Static) + `BoxCollider3D` (half extents 1,1,1) | One correct floor (mesh size and collider matched) instead of getting the numbers wrong in every scene. |
| **Bistro (level)** | *Not a prefab.* Stays a `ModelInstance` / Import Into Level | 6000 nodes must never become 6000 saved entities. A prefab may *contain* a `ModelInstance`, it does not expand it. |

What a prefab is **not**: it is not an imported glTF (that is `ModelInstance`: an *imported file* expanded from cooked
data) and not a scene (a scene is a whole level). A prefab is *authored* entities: components you added yourself
(colliders, scripts, animator, camera) that the glTF does not contain.

---

## 2. What the big engines do (researched 2026-09-26)

### Unity (the model to copy for editing)
Source: Unity manual, *Nested Prefabs*, *Override prefab instances*, *Prefab Variants*.

- A prefab asset is a file; a scene holds **instances**. An instance keeps a link to its asset.
- **Hierarchy shows instances with a blue cube icon and blue name**; the root of a prefab opened in Prefab Mode does not
  get the icon.
- **Overrides**: changes to an instance's properties (position, a component field, an added or removed component or child)
  are stored on the instance only. In the Inspector an overridden property is marked with a **bold name and a blue bar**
  in the margin. The Overrides dropdown offers **Apply** (write into the prefab asset), **Revert** (drop the override).
- An **added child** on an instance shows a **plus badge** on its icon.
- **Nested prefabs**: a prefab instance can sit inside another prefab and keeps its own link (blue cube stays).
- **Prefab Variants**: a prefab that inherits another and overrides parts of it.
- **Unpack**: turn an instance back into normal entities, removing the link.
- **Prefab Mode**: open the asset alone in an isolated editing view.

### Unreal Engine 5 (what to match where it fits)
Source: Unreal Engine docs, *Level Instancing in Unreal Engine*, *Outliner in Unreal Engine*.

- **Blueprint Actor** (the classic "prefab"): a class asset with components and script; instances are placed in the level.
  The Outliner shows it with a Blueprint icon. Per-instance edits are limited to *exposed/instance-editable* properties.
- **Level Instance**: a saved group of actors (a mini-level) placed as one actor. The actors are still listed in the
  Outliner, as **children of the Level Instance actor**. Edited by entering its edit mode. Per-instance mesh edits are not
  possible unless the experimental overrides of UE 5.5+ are enabled.
- **Packed Level Actor / Packed Level Blueprint**: a Level Instance baked for rendering (its static meshes merged into
  instanced components) -- the counterpart of our "combine" import mode.
- Outliner **folders** are a separate, purely organizational feature (we already have them: `FolderComponent`).
- I did **not** verify the exact Outliner icon colors of UE5; Unity's blue is the documented one and is what we copy.

### Decision: Unity's model for editing, UE's separation of concerns
- **Unity-style prefab** (asset + instances + overrides + apply/revert + blue in hierarchy) is the more capable and more
  familiar editing model, and matches what the user proposed (blue highlight).
- UE's lesson kept: **imported models stay a separate concept** (`ModelInstance`), and a prefab may reference one instead
  of expanding it. Baked/packed rendering variants are a later optimization, not part of the first prefab.

---

## 3. Architecture

### 3.1 Files

`.nprefab` -- YAML, **same writer/reader as scenes** (`SceneSerializer` entity code), so every component that saves in a
scene saves in a prefab for free.

```yaml
Prefab: Ball
Root: 11          # local id of the root entity
Entities:
  - Entity: 11    # LOCAL id, unique inside this prefab file (not a scene UUID)
    TagComponent: { Tag: Ball }
    TransformComponent: { Translation: [0,0,0], Rotation: [0,0,0], Scale: [1,1,1] }
    MeshComponent: { MeshHandle: 1407467..., SubmeshIndex: 0, SubmeshCount: 1 }
    RigidBody3DComponent: { BodyType: Dynamic, AngularDamping: 0.5 }
    SphereCollider3DComponent: { Radius: 1 }
```

Entity references *inside* the prefab (for example `CameraFollow.Target`) are stored as local ids and remapped to the new
instance's UUIDs on spawn (the scene serializer already remaps entity references).

New asset type `AssetType::Prefab` (extension `.nprefab`), registered like the others; it shows in the Content Browser with a
blue cube icon.

### 3.2 Instances in a scene

An instance is **one saved entity** (the instance root) carrying a new component, exactly like `ModelInstanceComponent`:

```cpp
struct PrefabInstanceComponent
{
    AssetHandle Prefab = 0;
    std::vector<PrefabOverride> Overrides;   // v1: only the root transform lives on the root entity itself
};
```

- **Saved:** only the instance root and its overrides. The children are *not* written to the scene file. A level with 500
  balls saves 500 small entries, and editing the prefab needs no scene edit.
- **Loaded:** `Prefab::Spawn(scene, root)` builds the child entities from the asset, the same way
  `ModelInstance::spawn` builds nodes from cooked data. It requests the referenced meshes and materials like any spawn.
- **Stable identity:** a spawned entity's UUID is `hash(instanceRootUUID, prefabLocalId)`, the same idea as
  `ModelInstance::NodeUUID`. Scripts and saved references to a prefab child therefore survive reloads.
- **Runtime instantiate:** `Scene::Instantiate(prefabHandle, transform)` and a C# `Scene.Instantiate(prefab)` (Play mode).
  This is what lets a script spawn projectiles or enemies.

### 3.3 Overrides (grown in steps)

| Version | Overrides supported | Notes |
|---|---|---|
| **v1** | Root transform (position/rotation/scale) | Each instance has its own place; everything else comes from the asset |
| **v2** | Property overrides on any component field: stored as `(entity local id, component, field path, value)` | Needs field-level diff/serialization; this is the large piece |
| **v3** | Added / removed entities and components on an instance; nested prefabs; **Prefab Variants** | Same mechanism as v2 plus structure changes |

Override storage is versioned by field path so a renamed field simply drops the override (no migrations, the no-compat rule).

### 3.4 Editor UX

| Where | What the user sees |
|---|---|
| **Hierarchy** | Prefab instance root: **blue name + cube icon** (Unity). Its child entities are shown greyed/indented under it (like the Level Instance children in the UE Outliner) and are **not individually deletable** (they belong to the recipe). |
| **Viewport** | Selecting a child selects... the *instance root* by default (click once = instance, `Ctrl+click` = child); the gizmo moves the instance. |
| **Inspector** | Banner at the top of an instance root: `Prefab: Ball.nprefab   [Open]  [Select Asset]  [Unpack]`. From v2: overridden fields drawn with a blue bar; `Apply` / `Revert` per field or all. |
| **Content Browser** | `.nprefab` with a blue cube thumbnail; drag into viewport or hierarchy places an instance (the placement code already exists for meshes: `EditorLayer::PlaceAssets`). |
| **Create (drag)** | **Drag from the hierarchy into the Content Browser** (onto the folder being shown or onto a folder tile): creates the `.nprefab` there from what was dragged -- one entity with its whole child hierarchy, or **all selected entities** when the dragged one is part of a multi-selection (same rule as multi-drag today). **The dragged entities are not replaced or changed**: they stay in the scene as ordinary entities; the prefab asset is only *added* to the Content Browser (the user drags it into the level to place instances). This is the main way to create a prefab. |
| **Create (menu)** | Right-click the selection in the hierarchy -> **Create Prefab...**: same result (asks for the folder in a small dialog, then the same inline rename). |
| **Unpack** | Instance -> ordinary entities, link removed (Unity's Unpack Completely). |
| **Prefab Mode (v2)** | "Open" edits the asset in an isolated scene view (the entities of the prefab only), with a `Save` button; instances update on save. |

The editor rule from the user: *prefab instances must be visibly different from ordinary entities* -> blue in hierarchy, blue
icon in the Content Browser, banner in the Inspector.

### 3.4.1 Creating a prefab from a drag (rules)

| Dragged | Prefab root | Result in the scene |
|---|---|---|
| **One entity** (with or without children) | A copy of that entity; its children are copied along | Nothing changes in the scene |
| **Several selected entities, one common parent or all top-level** | A new empty **root entity** named after the prefab; copies of the selected entities become its children (positions relative to the selection's centre) | Nothing changes in the scene |
| **Several selected entities from different parents** | Same as above (each keeps its transform relative to the common root; parent links to *unselected* entities are cut) | Nothing changes |
| A selected entity **and** its selected child | The child is treated as part of the parent (never listed twice) | Nothing changes |

Rules that apply to all of them:
- **Transforms** are stored **relative to the prefab root**; the root itself is stored at the origin, so an instance placed at the drop point lands exactly there.
- **Naming (like a new folder or file in Windows):** the prefab is created **immediately** with a default name (the dragged
  entity's name; several entities: `New Prefab`; a taken name gets a number: `Ball`, `Ball 2`) and its tile in the Content
  Browser goes straight into **inline rename mode** with the whole name selected. **Enter** (or clicking elsewhere) accepts what
  is typed, an untouched field keeps the default, **Esc** keeps the default name. Renaming moves the file and keeps the asset
  handle (so instances placed meanwhile stay linked).
- **Location:** the Content Browser folder the drop landed on (the folder currently shown, or a folder tile). Dropping on a non-folder tile does nothing.
- **What is copied:** every component that saves in a scene, plus `FolderComponent` is *not* copied (a prefab is not in an outliner folder; the instance keeps the folder of the entity it replaces).
- **What is not copied:** `ModelInstanceComponent` roots are stored **as references** (never expanded); entity references that point outside the dragged set are cleared (with a log line listing them), references inside are remapped to local ids.
- **Undo:** the scene is untouched, so there is nothing to undo there; deleting the asset in the Content Browser removes the prefab.
- The Content Browser needs a drop target for the existing hierarchy drag payload (`SCENE_HIERARCHY_ENTITY` carries one UUID; the selection is resolved from the hierarchy panel like the multi-drag already does).

### 3.5 What we can reuse (already in the engine)

- `ModelInstance` machinery: spawn-from-data, stable node UUIDs, saving only the root, removed-node lists, requesting
  assets while spawning, `Scene::CollectAssetReferences` for the sweep.
- `SceneSerializer`: entity/component read/write, entity-reference remapping.
- `Scene::Copy`, `FolderComponent` (a prefab instance can sit in an outliner folder), `PlaceAssets`, the Content
  Browser drag payloads, multi-select.
- Registry, asset types, `Content Browser` thumbnail hooks.

### 3.6 Interaction with existing systems

| System | Rule |
|---|---|
| **ModelInstance** | A prefab may contain a `ModelInstanceComponent`; it is spawned as usual when the prefab spawns. Never expanded into saved entities. |
| **Import Into Level / flat import** | Unchanged. Imports stay flat entities; "Create Prefab" from a selection of imported entities is allowed (they are ordinary entities once spawned). |
| **Physics / character** | Components on prefab entities register exactly as for scene entities when spawned (`OnComponentAdded`). Colliders are sized by the user; the collider-vs-mesh size rule (collider half extents = mesh half size; entity scale applied on top) applies inside prefabs. |
| **Scripts** | Entity fields (e.g. `Target`) inside a prefab remap to the instance's own entities; a field pointing outside the prefab stays a scene reference set per instance (v2 override). |
| **Animation graph** | `AnimatorComponent` with its graph is just a component; nothing special. |
| **Asset sweep** | `CollectAssetReferences` also collects the prefab asset and everything its entities reference. |
| **Undo/redo** | Whatever exists for entities applies to instance roots; child edits become overrides (v2). |

### 3.7 Deliberately not in the first versions

Nested prefabs and variants (v3), baked/packed rendering variants (UE's Packed Level Actor, a later optimization tied to
combine-import), world-partition cell streaming of prefabs (Phase 6c), and merging edits from two open scenes.

---

## 4. Phases (each ends with a user test)

| Phase | Deliverable | User test |
|---|---|---|
| **P1 -- Asset + spawn** | `AssetType::Prefab`, `.nprefab` read/write through the scene serializer, `Prefab::Spawn`, `PrefabInstanceComponent` (saved as root only), stable UUIDs, asset collection for the sweep. No editor UI yet: a prefab file is written by hand-copying entities. | Load a scene containing a hand-made instance; entities appear; save + reload keeps them; deleting the scene frees the assets. |
| **P2 -- Create + place** | **Drag entities from the hierarchy into the Content Browser to create the prefab (section 3.4.1: one entity with children, or a multi-selection)**, hierarchy "Create Prefab..." menu, drag `.nprefab` from the Content Browser into viewport/hierarchy (uses `PlaceAssets`), instance-root transform overrides, blue name + cube icon in the hierarchy, blue cube in the Content Browser, Inspector banner. | Drag the sphere from the hierarchy into a Content Browser folder (then again with three entities selected at once); drag the Ball in 20 times; move a few; edit the `.nprefab` (or asset) and reload: all update; blue visible. |
| **P3 -- Editing loop** | Prefab Mode ("Open" edits the asset), `Unpack`, click-selects-instance-root, children locked in the hierarchy. | Edit the Ball in Prefab Mode, Save: instances in the level update without reload. |
| **P4 -- Runtime** | `Scene::Instantiate` + C# `Scene.Instantiate(prefab)`, entity-reference remap for scripts. Script API it needs (all missing today, checked 2026-09-26): `Input.IsMouseButtonDown/Pressed`, rigid body `SetVelocity`/`AddForce`, `Destroy(entity)`. | **The GlowBall test (section 6):** left click shoots a glowing ball that lights the floor and removes itself after 5 s. |
| **P5 -- Property overrides (v2)** | Field-level overrides, blue bar + bold in the Inspector, Apply / Revert. | Change one ball's damping; only it differs; Revert restores; Apply changes all. |
| **P6 -- Structure overrides + nesting (v3)** | Added/removed entities and components, nested prefabs, variants. | Player prefab containing a Weapon prefab. |

**Recommendation:** build P1-P3 first (they already give the main value: one recipe, many instances, visible in blue), then
P4, then decide about P5/P6 by how much they are missed.

---

## 4.1 Known limitation that MUST be lifted (decided 2026-09-26)

Until **P5**, an instance can only override its own **position, rotation and scale**; every other change to a placed
instance is not saved (the asset wins on load). This is a temporary limit of the first versions, **not the target design**:
once P1-P4 work, **P5 (property overrides) and P6 (structure overrides, nesting, variants) are built as planned**, they are
not optional. Until then the Inspector says so on an instance ("Only the transform is saved per instance") so nobody loses
edits silently.

## 5. Decisions to confirm with the user before P1

1. **Format:** `.nprefab` = scene-format YAML (recommended), instances saved as root only (recommended).
2. **Selection behaviour:** click selects the instance root, `Ctrl+click` a child (Unity) -- or always the child.
3. **Where "Create Prefab" writes:** the Content Browser folder the entities were **dragged onto** (decided by the user 2026-09-26); the menu variant uses a dialog with the current folder as default.
4. **Blue:** hierarchy name and icon tint = blue for prefab instances; the exact color can be tuned in the UI theme.
5. **v1 override scope:** root transform only -- **agreed 2026-09-26 as a temporary limit; property overrides (P5) are mandatory once P1-P4 work (section 4.1).**

## 6. Worked example for Phase 4: the glowing ball

`GlowBall.nprefab`: root entity (Transform scale 0.2, Sphere mesh, a `.nmat` with a bright color and Emissive Strength ~20,
`RigidBody3D` Dynamic, `SphereCollider3D` radius 1 because the sphere mesh is 2 m wide, script `Projectile` with a `Lifetime`
of 5 s) plus a child entity with a `PointLightComponent` (orange, range 5).

A script on the player/camera, on left click:

```csharp
Entity ball = Scene.Instantiate(GlowBallPrefab);            // new in P4
ball.GetComponent<TransformComponent>().Position = cameraPosition + forward * 1.0f;
ball.GetComponent<RigidBody3DComponent>().SetVelocity(forward * 20.0f);   // new in P4
```

and `Projectile.OnUpdate` counts down `Lifetime` then calls `Scene.Destroy(Entity)` (new in P4). Editing the prefab (size,
color, light, lifetime) changes every future shot; nothing is built in code.

Missing script API this needs: `Input` mouse buttons, rigid body velocity/force, `Destroy(entity)`, `Instantiate(prefab)`.

## 7. Status log

- 2026-09-26: plan written; decisions 1-5 agreed (drag-to-create from the hierarchy into the Content Browser without replacing the entities, inline rename like Windows). P1 built (untested): `AssetType::Prefab` + `.nprefab` importer, `Prefab` asset, `PrefabInstanceComponent`/`PrefabNodeComponent`, `PrefabInstance::SpawnPending` (spawns from the asset, UUIDs `hash(instance, local id)`, parent/child and script-reference remap), scene save writes the instance root only (its spawned entities and the prefab-root components are not saved), duplicate/copy/asset-collection handled, `SceneSerializer::DeserializeEntityComponents` extracted from the scene loader and shared. Drag-placement of an existing `.nprefab` (viewport + hierarchy) was pulled into P1 so it can be tested; sample `Facerun/Assets/Prefabs/Ball.nprefab` (registered by hand).
- 2026-09-26: **P1 tested by the user (works).** Two follow-ups found while testing: the sample prefab held a stale mesh handle (the registry had been regenerated), and the Inspector's shared component header gave every component's `+` button and popup the same ImGui id (fixed with a per-component `PushID`).
- 2026-09-26: **P2 built (untested):** `SceneSerializer::SerializePrefab` (one entity = root with its hierarchy; several = children of a new empty root named after the prefab, positioned relative to the first/dragged one; scene untouched), Content Browser drop targets for `SCENE_HIERARCHY_ENTITY` (free area = current folder, folder tile = that folder), the new prefab is created at once and its tile goes into **inline rename** (Enter / click elsewhere accept, Escape keeps the default; F2 renames a selected prefab), `EditorAssetManager::RegisterExistingFile` / `RenameAsset` / `CanRename` (prefabs only: self-contained files), a placed instance takes the prefab root's rotation and scale (`PrefabInstanceComponent::InitTransform`), blue prefab instances (bright) and their spawned entities (dim) in the hierarchy, blue tile + name for `.nprefab` in the Content Browser, Inspector banner with **Unpack** (`PrefabInstance::Unpack`). **Not in this step:** the hierarchy right-click "Create Prefab..." menu variant; clearing script entity references that point outside the dragged set (they keep their UUID); locking the spawned children in the hierarchy (P3).
- 2026-09-26: **P2 tested by the user (works).**
- 2026-09-26: **P3 built (untested):** **Prefab Mode** -- double-click a `.nprefab` (or **Open** in an instance's Inspector banner) swaps the level for a scene made from the prefab (`PrefabInstance::LoadForEditing`: entities keep their file ids as UUIDs, root at the origin, orphaned parent links hang from the root); the level is kept aside (its assets stay through the sweep) and the toolbar shows **Save / Save & Exit / Exit (discard changes)** instead of Play (Ctrl+S saves the prefab, never the level; opening another scene exits Prefab Mode). Save = `SerializePrefab` of the root, the loaded `Prefab` asset takes the new content, then `PrefabInstance::RespawnAll` on the level (spawned entities destroyed, components the prefab gave the root removed, `Spawned=false`: they spawn again from the new asset; placement kept). Instance locking: spawned entities are not draggable, the Delete key skips them (with a log line), a viewport click on one picks the instance (Ctrl+click the entity itself), their Inspector shows "Part of prefab instance ..." with **Select Instance**. Unpack was done in P2.
- 2026-09-26: **P3 tested by the user (works)** after two fixes (the prefab scene had no lighting: the level's environment and sun are copied in as `Preview` entities; parent/child lists are rebuilt from the Parent links in both loaders).
- 2026-09-26: **P4 built (untested):** `Scene::Instantiate(prefab, position)` (`PrefabInstance::Instantiate`: the prefab asset is read synchronously, the instance spawns at once, world matrices are worked out for the fresh entities, then physics bodies / characters are created and script instances started when the scene runs) and `Scene::QueueDestroy` (deferred to the end of the script loop; `DestroyEntity` now also destroys the entity's Jolt body / character and its script instances while running -- it did neither before). The script loop collects its entities first (a script may instantiate or destroy). C# API: `Scene.Instantiate(path, position)`, `Scene.Destroy(entity)`, `Input.IsMouseButtonDown(MouseButton)`, `RigidBody3DComponent` (`LinearVelocity`, `AddForce`, `AddImpulse`), `EntityBehaviour.Self`. Prefabs are addressed by path under the asset directory (`Prefabs/GlowBall.nprefab`), resolved through the editor asset registry (an exported game will need its own lookup). Test case: `Facerun/Assets/Prefabs/GlowBall.nprefab` (+ `GlowBall_Material.nmat`, emissive), scripts `Shooter` (on the camera) and `Projectile` (on the prefab, `Lifetime`).
- 2026-09-26: **P4 tested by the user (works)** after two fixes: native code called from a script must not call back into managed code (`Scene.Instantiate` now queues the script start of the new entities until after the script loop), and game scripts only read input while the viewport is the target (`Input::SetGameInputEnabled`).
- 2026-09-26: **P5 built (untested) -- property overrides.** No per-field code: an instance's overrides are found by serializing each spawned entity (`SceneSerializer::EntityToNode`, full form even for the root) and comparing it with the prefab's text of that entity field by field (`PrefabInstance::CollectOverrides`; numbers compare with a small tolerance; placement, structure and entity references are skipped: root Tag/Transform, Relationship, Folder, `NodeEntities`, `EntityReferences`; components only in one of the two are not overrides yet). An override is `{entity id in the prefab file, component key, field, value as YAML text}` (`PrefabPropertyOverride`); the scene saves them on the instance root (`Overrides:`), and the spawn patches them into a clone of the prefab's text before building the entities. `Respawn` no longer collects: callers do (`CaptureOverrides` before the prefab asset's content changes -- Prefab Mode's Save does it first, otherwise every prefab edit would look like an override). `RevertOverrides` (one field or all: drop it, respawn), `ApplyOverride` (patch the loaded prefab and its file with `PrefabImporter::SavePrefab`, respawn every instance, the others keep their own differences). Duplicating an instance keeps its overrides. Inspector: **Overrides (n)** section on the instance root listing `entity: Component.Field = value` with **Revert** / **Apply** per field and **Revert All**. Limits kept for P6: added/removed components and entities, nesting, variants. The temporary limit of section 4.1 (transform only) is lifted.
- 2026-09-26: **P5 tested by the user (works)** after two fixes: a yaml-cpp gotcha (assigning a node over one that shares data with the prefab's overwrites the prefab's own text: the patched copy is now returned by value from `patchedEntityNode`) and a missing `<filesystem>` include; the Overrides list shows asset names for asset handles.
- 2026-09-26: **P6a built (untested) -- structure overrides.** `PrefabStructureChange` (`RemovedEntity`, `RemovedComponent`, `AddedComponent`) on the instance (`Structure:` in the scene file), found by comparing the serialized component keys of each spawned entity with the prefab's (`CollectStructureChanges`; a deleted entity is reported once, at the top of its deleted subtree). The spawn skips removed entities and everything below them (by the prefab's parent links), removes/adds component keys in the patched copy of the prefab's text. **Added entities** are ordinary scene entities below a spawned entity: they hang from it again after a spawn (`relink`), and survive a respawn (`respawn` detaches them from their parent's child list before the spawned entities are destroyed). The Delete key works on spawned entities (they become "removed" overrides). Overrides list rows: property, `- Entity`, `- Component`, `+ Component`, `+ Entity`, each with Revert / Apply (`PrefabInstance::ListOverrides` / `RevertOverride` / `ApplyOverride`; Apply on an added entity writes it and its subtree into the prefab with new ids, on a removed entity deletes the node and its subtree from the prefab). Not tracked: re-parenting a spawned entity; entities attached directly below the instance root now also show as `+ Entity` (Revert deletes them).
- 2026-09-26: **P6a tested by the user (works)** after fixes: the deleted entity is listed by the name it had when it was deleted (`RemovedNames`, saved in the deletion's `Value`), Revert puts a remembered rename back as an override, and a rename (`Tag.Tag`) is now applied at spawn (the child's name came from the prefab's unpatched text).
- 2026-09-26: **P6b built (untested) -- nested prefabs.** A prefab file may contain prefab instance nodes (their `PrefabInstanceComponent` with its own Overrides/Structure); the outer instance spawns them like any node and they spawn their own entities next frame (`PrefabNodeComponent.Instance` of an inner root = the outer root, of the inner entities = the inner root). The inner instance's entities are not saved, so the outer instance saves `Nested:` -- per inner instance, only what differs from the prefab file's own text of it (`PrefabInstance::CollectNested`, compared with `SceneSerializer::PrefabChangesToNode`; recursive: `PrefabNestedChange.Nested`) -- and the spawn replaces the inner instance's Overrides/Structure/Nested with them. Capture / duplicate / revert keep nested changes (Revert All clears them). Nested instance roots are excluded from the added-component diff (they have the components their own prefab gave them). A prefab that contains itself (directly or through instances) is refused at spawn. Creating a prefab from a single prefab instance wraps it in a new empty root (a prefab whose own root were an instance would be a variant: P6c). Viewport click picks the outermost instance. Not done: Apply of a nested instance's change into the OUTER prefab (Apply on the inner instance's own Overrides list writes into the inner prefab).
- 2026-09-26: **P6b tested by the user (works).**

- 2026-09-26: **P6c built (untested) -- prefab variants.** A variant is a `.nprefab` with `Base: <prefab handle>` plus the same kinds of changes an instance holds (`Overrides`, `Structure`, `Nested`) and its own `Entities` (only the ones it adds; their parent ids may be the base's). `Prefab` (asset) got `Base`, `Overrides`, `Structure`, `Nested`; `PrefabImporter` reads/writes them (`SceneSerializer::ReadPrefabChanges` / `WritePrefabChanges` are public now). The full text of a prefab is `effectiveNodes(prefab)`: for a variant, its base's effective nodes (recursively, depth <= 16) with the variant's deletions, structure and overrides patched in (`patchedEntityNode`, `withNestedChange`) and its own entities appended; instances patch their overrides on top of that, so spawn / CollectOverrides / CollectStructureChanges / CollectNested / ListOverrides all work on the effective text. Instances of a prefab include instances of its variants (`prefabDependsOn`: capture and respawn reach them). **Create**: right-click a prefab in the Content Browser -> **Create Variant** (`<Base> Variant.nprefab`, inline rename like a new file). **Edit**: double-click a variant -> Prefab Mode holds the BASE as an instance carrying the variant's changes (`LoadVariantForEditing`; the variant's own entities are attached below it, their ids are their UUIDs); Save writes the instance's Overrides / Structure / Nested and the entities attached below it back into the variant (`SaveVariant`); Ctrl+S / Save & Exit as before. **Apply** on an instance of a variant writes into the variant (base entity -> an entry in its Overrides / Structure, the variant's own entity -> its text; Apply on an added entity appends nodes to the variant's own `Entities`). Instance banner: `(variant of Base.nprefab)`. Prefab ids of added entities are now their entity UUIDs (stable while they live).

## 8. Open items (not in a phase yet)

1. **Apply of a nested instance's change into the OUTER prefab.** A change made on an inner instance (the Weapon inside a placed Player) is kept on the outer instance (`Nested`); Apply on the inner instance's own Overrides list writes into the INNER prefab. Writing it into the outer prefab's text of the inner instance (its `PrefabInstanceComponent` Overrides/Structure/Nested) is not offered yet. (Found 2026-09-26, deliberately left when P6b was built.)
2. Hierarchy right-click **Create Prefab...** menu variant (dragging into the Content Browser is the way today).
3. Clearing script entity references that point outside the dragged set when a prefab is created (they keep their UUID).
4. **Prefab Mode save prompt:** unsaved edits are lost silently on Exit or when another scene is opened.
5. Re-parenting a spawned entity is not tracked as an override; entities attached directly below an instance root also show as `+ Entity`.
6. Overrides on an instance's entity that was DELETED are dropped with it (only its name is remembered, and put back on Revert).
7. Exported games need their own prefab lookup for `Scene.Instantiate` (it resolves paths through the editor asset registry).

