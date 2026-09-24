# NoxEngine: Animation Graph & Character Movement Architecture Plan (2026)

> **Document Version:** 1.0 (September 2026)
> **Scope:** a reusable, editor-authored node-graph framework (state machine + blend space), first applied to
> character animation, with a physics-driven character controller so a character can be walked around with
> real movement and blended animation.

---

## 1. Collaboration Contract

Inherits §1 of `docs/RayTracing_Architecture_Plan_2026.md` and §2 of `docs/Engine_Architecture_Plan_2026.md`
in full (execution policy, phase gates, no-compatibility-code, class layout, module boundaries, Release-only
performance measurement). The points that matter most for this plan:

1. **The user builds and runs.** I inspect code, syntax-check with the clang-cl `/Zs` script and compile
   shaders with `slangc`; the user relaunches and tests every step. I never run MSBuild.
2. **No phase starts until the previous one is confirmed working** by the user.
3. **No compatibility code.** `.nanimgraph` is a new format; if it changes shape mid-plan, old test graphs
   are hand-recreated, not migrated.
4. **Class layout.** `public:` functions top, `private:` functions middle, `private:` members bottom.
5. **Module boundaries stay clean.** Jolt types stay inside the Physics module (`Physics/Jolt/`); the node
   graph core stays domain-agnostic — animation-specific node types live in their own files and register into
   the generic core, they never leak into it.
6. **Port library reference code verbatim.** Jolt's `CharacterVirtual` is used per its own sample/doc, not
   hand-rolled; imgui-node-editor is integrated per its own examples.

---

## 2. Reference documentation

- **Jolt Physics — Character Controller**: [JoltPhysics `Character.md`](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md#character-controller),
  `Samples/Tests/Character/CharacterVirtualTest.cpp` (vendored under `NoxCore/vendors/JoltPhysics`) is the
  canonical usage example for `JPH::CharacterVirtual`.
- **imgui-node-editor** (thedmd): [thedmd/imgui-node-editor](https://github.com/thedmd/imgui-node-editor) —
  MIT, header+source, works against a plain ImGui backend (already have 1.92.9 WIP vendored). Its
  `blueprints-example` is the reference for node/pin/link rendering and context menus.
- **entt** (ECS already in use): [skypjack/entt](https://github.com/skypjack/entt) — component registration
  conventions already followed by `Scene/Components.h`.

---

## 3. Grounding: what exists today (surveyed September 2026)

- **Animation**: `AnimationSequence` asset (`.nanim`, binary, single clip: name/duration/ticks-per-second +
  per-node position/rotation/scale keyframe channels) + `Animator` runtime sampler
  (`NoxCore/src/NoxCore/Animation/Animator.{h,cpp}`, produces `FinalBoneTransforms`) + `AnimatorComponent`
  (`Scene/Components.h`). Per-frame hook: `Scene::RegisterSystems()` → `UpdateAnimators()`
  (`Scene.cpp:556`), declared `Write<AnimatorComponent, TransformComponent, DirtyTransformComponent>`,
  running as a sibling of the physics systems, before transform propagation.
- **Physics**: Jolt (`NoxCore/src/NoxCore/Physics/Jolt/`), `RigidBody3DComponent` + collider components exist.
  **No character controller yet.**
- **Scripting**: C#/.NET (`NoxCore/src/NoxCore/Scripting/`). `ScriptComponent` already has a per-field
  override map (`FieldOverrides[className][fieldName]`) — the pattern this plan reuses for graph parameters.
- **ECS / serialization**: entt registry; components serialize as YAML blocks in `SceneSerializer.cpp`.
- **Editor**: no node-graph UI exists (`RenderGraphPanel` is a stats viewer, not a canvas). No
  double-click-to-open-editor-window infrastructure exists for assets (drag-drop assignment only). ImGui
  1.92.9 WIP is vendored; no ImNodes/imgui-node-editor/ImPlot vendored yet.

---

## 4. Architecture

### 4.1 Generic node-graph core (reusable — the actual point of building it this way)

`NoxCore/src/NoxCore/NodeGraph/` — domain-agnostic:
- **Data model**: `NodeGraph` (nodes with typed pins + positions, links, a typed parameter blackboard).
- **Registry**: `NodeType` registration (name, category, input/output pin types, an evaluate callback);
  domains (animation now, audio later) populate it, the core never references a domain by name.
- **Compiler**: `GraphCompiler` turns an authored `NodeGraph` into a flat `CompiledGraph` — topologically
  sorted, links resolved to indices, no per-frame map/pointer-chasing lookups.
- **Serializer**: YAML, matching `SceneSerializer`'s block style; each node type owns serialization of its
  own extra data (e.g. a state machine's transition conditions).
- New `AssetType::AnimationGraph`, extension `.nanimgraph` (text/YAML — human-diffable, unlike binary `.nanim`).

### 4.2 Animation domain (built on the core)

- **Clip node**: wraps a `.nanim` handle + loop/speed; calls into the existing `Animator` sampling code
  rather than duplicating it.
- **Blend2D node**: N clip inputs positioned in a 2D parameter space (e.g. speed × direction), outputs a
  blended pose.
- **Output Pose node**: sink; the compiled graph's result.
- **State Machine node**: a second, composable eval path (states are themselves sub-graphs; transitions
  aren't a pure DAG so this isn't forced through the DAG compiler) — states, transition conditions read from
  parameters, blend duration/curve.
- **Parameters**: typed blackboard (float/bool/vector2) on the graph asset, exposed on the new
  `AnimationGraphComponent` via the same override-map idiom `ScriptComponent` uses, read/write from C#.

### 4.3 Runtime integration

- `AnimationGraphInstance` (per-entity mutable state: current clip times, active state, transition blend
  progress, parameter values) paired with the asset-owned `CompiledGraph`, mirroring the existing
  `Animator` (mutable) / `AnimationSequence` (asset) split.
- `AnimationGraphComponent` (ECS): `AssetHandle Graph`, `AssetHandle Skeleton`, the runtime instance,
  parameter overrides. Serializes as a new YAML block in `SceneSerializer.cpp`. Coexists with
  `AnimatorComponent` (simple non-graph props keep using the plain animator) rather than replacing it.
- New "Animation Graph" system registered in `Scene::RegisterSystems()`, same declared access as today's
  "Animation" system, writing into the same `FinalBoneTransforms`/skin-matrix path so downstream skinning
  code is untouched.

### 4.4 Editor

- Vendor **imgui-node-editor** into `NoxCore/vendors/imgui-node-editor`.
- Generic `NodeGraphEditorPanel`, parameterized by a domain's node registry (so the same panel later serves
  an audio graph): canvas, add-node menu, typed pin linking, per-node property inspector, a special-cased
  bottom/breadcrumb view for entering a State Machine node's nested graph.
- New `AssetType → open-panel` registry on `EditorLayer` (doesn't exist today) driving double-click-to-open
  from the content browser and from an asset-reference field in the inspector.
- `AnimationGraphComponent` inspector block, matching `AnimatorComponent`'s drag-drop pattern, plus a
  double-click on the reference field.

---

## 5. Steps (each ends with a user build and test)

- [x] **Step 1 — Generic node-graph core + file format, headless.**
  `NodeGraph`/`NodeType`/`GraphCompiler`/serializer, `AssetType::AnimationGraph` (`.nanimgraph`). No editor
  UI. Verified with a hand-written test graph file loading/compiling correctly.

- [x] **Step 2 — Animation runtime (blend tree only), no editor yet.** Confirmed working end to end on Fox:
  the "Speed" parameter (0 = Walk, 1 = Run) blends live, matching the plan's exit test. Graph mode merged into
  `AnimatorComponent` per the user's question (see decisions log) rather than a separate component.
  `Facerun/Assets/Models/Fox/Fox_WalkRun.nanimgraph` is the confirmed test graph.

- [x] **Step 3 — Vendor imgui-node-editor + generic graph canvas panel.** Confirmed working (canvas, linking,
  creation, deletion, save, live recompile, asset picker). Backed by `INodeGraphCanvasBackend` so the canvas
  library is swappable.

- [~] **Step 4 — Double-click asset workflow.** Code complete, syntax-checked; needs a rebuild + test.
  `AssetType → open-function` registry on `EditorLayer` (`m_AssetOpeners`, `OpenAsset(handle)`); Content
  Browser double-click and the Animator's Graph field double-click both call it; "New Animation Graph"
  creates + registers a graph. (Graph mode lives on `AnimatorComponent`, so there's no separate component
  inspector block -- see Step 2's correction.)
  **Test:** full create → assign → edit → play loop, no manual file editing needed.

- [~] **Step 5 — Parameters + script binding.** Code complete; C++ syntax-checked, the C# side (not compiled by
  me) needs a build. Parameters panel in the graph editor, a `GetParameter` node, and
  `Nox.AnimatorComponent.SetFloat/GetFloat/SetBool/GetBool` for scripts.
  **Test:** a script drives a parameter, the blend responds live.

- [~] **Step 6 — Character controller.**
  `CharacterController3DComponent` on `JPH::CharacterVirtual` (per Jolt's own sample), a movement script
  feeding Speed/IsGrounded/MoveDirection into the graph's parameters.
  **Test:** walk a physics character around the level, idle/walk/run blending live from real movement.

- [ ] **Step 7 — State Machine node.**
  Nested sub-graphs per state, transitions with conditions/blend duration/curve, breadcrumb editing.
  **Test:** Idle↔Walk↔Run↔Jump state machine driven by real movement, smooth blended transitions.

- [ ] **Step 8 (future, not scheduled) — Audio graph domain.**
  Reuses the step 1/3 core and editor panel unchanged, proving out the "reusable graph compiler" goal.

---

## 6. Decisions log

- **Step 1 (September 2026):** built as `NoxCore/src/NoxCore/NodeGraph/` (`NodeGraphTypes.h`, `NodeType.h/.cpp`,
  `NodeGraph.h`, `CompiledGraph.h`, `GraphCompiler.h/.cpp`, `GraphEvalContext.h/.cpp`, `NodeGraphAsset.h`) plus
  the asset-system glue in `NoxCore/src/NoxCore/Asset/` (`NodeGraphSerializer.h/.cpp`,
  `NodeGraphImporter.h/.cpp`), matching `AnimationImporter`'s shape and `SceneSerializer`'s YAML style.
  - `AssetType::AnimationGraph` added to `Asset.h`/`Asset.cpp`; `.nanimgraph` added to
    `EditorAssetManager.cpp`'s extension map and native-file scan list; `NodeGraphImporter` registered in
    `AssetImporter.cpp`.
  - Runtime pin/property values are a `std::variant<bool, int32_t, float, glm::vec2, uint64_t, std::string>`
    (`NodeGraphValue`); `uint64_t` doubles as `AssetHandle`/`UUID` since the core has no notion of assets.
  - Pin types are domain-defined strings ("Pose", "Float", ...) compared by name at compile time — the core
    never needs a fixed type list, which is what lets a future audio graph reuse it untouched.
  - `GraphCompiler::Compile` does Kahn's-algorithm topological sort (stable tie-break by original node index),
    validates pin-type names match on every link, and fails closed (empty `CompiledGraph`) on any unknown
    type, cycle, bad link or missing output node.
  - `GraphEvalContext` type-erases pin values via `std::any`, one per instance, sized once in `Init()`; node
    `Evaluate` callbacks move values by value (`GetInput`/`SetOutput`/`GetProperty` all return/take by value)
    to sidestep lifetime issues — acceptable since a full graph evaluation runs at most once per entity per
    frame, not in a hot inner loop.
  - No `NodeGraph::SubGraph`/nested-container field yet — deliberately deferred to Step 7 (State Machine node)
    rather than guessing its shape now.
  - **Not yet tested in-engine** (no node types registered, no editor): verified so far only by clang-cl `/Zs`
    syntax checks on every new/edited file. Step 2 registers the first real node types (Clip/Blend2D/Output)
    and a hand-written test `.nanimgraph`, which is where this actually gets exercised end to end.

- **Step 2 (September 2026):** built the animation domain on top of Step 1's core, all syntax-checked, but
  **could not be exercised end to end yet** -- two blockers found while wiring it up, both outside this
  step's control:
  - The `Facerun` project's asset registry has no `AnimationSequence`/`Skeleton` assets at all yet (Bistro is
    static geometry only), so there's no real `AssetHandle` to hand-author a test `.nanimgraph` against.
  - `Scene::GetBoneTransforms` -- the function any skinned mesh's bone matrices come from -- has no caller
    anywhere in the renderer yet, for `AnimatorComponent` either. Skeletal mesh rendering isn't wired into the
    render path in this codebase yet, independent of the animation graph.
  - **Needs from the user:** import a rigged/skinned character (drag a `.glb`/`.gltf` with a skeleton + at
    least one clip into the content browser) so we have real assets to build a test graph against, and confirm
    whether skinned rendering is expected to already work today with plain `AnimatorComponent` (if not, that's
    a separate prerequisite this plan doesn't own).
  - What got built regardless (ready to exercise once the above is available):
    - `Animator::InterpolatePosition/Rotation/Scale` and `FindKeyframeIndex` promoted to `public static` on
      `Animator` so clip sampling has exactly one implementation, shared by the plain `Animator` and the
      graph's Clip node (`Animator.h`/`.cpp`).
    - `AnimPose` (`Animation/AnimPose.h/.cpp`): a skeleton's local pose, index-parallel to
      `Skeleton::AllNodes`; `SampleClipPose` (ports `Animator::UpdateTransforms` steps 1-2), `BlendPoses`
      (lerp/slerp, shortest-path like `InterpolateRotation`), `ApplyPoseToSkeleton` (ports steps 3-4 verbatim,
      including writing into the `Skeleton` asset's shared `Node` objects the same way `Animator` already
      does -- not a per-instance-safe design, but matching existing behavior exactly rather than quietly
      redesigning it).
    - `Animation/AnimationGraphNodes.h/.cpp`: registers `Clip`, `Blend2D`, `Output` under domain
      `"AnimationGraph"`. **`Blend2D` today is only a 2-source linear blend** driven by one named parameter
      (`ParameterName` property, falling back to a constant `X` property) -- the full N-source 2D blend space
      the name promises (multiple sources positioned in X/Y, barycentric/nearest-3 blending) needs
      variable-arity node inputs, which the current fixed-pin-list `NodeTypeDesc` doesn't support; deferred
      rather than guessed at.
    - `Animation/AnimationGraphInstance.{h,cpp}`: per-entity runtime (mirrors `Animator`/`AnimationSequence`'s
      asset/instance split); `Sync()` only ever calls `AssetManager::FindLoadedAsset` (never blocks on disk
      I/O) and reports unresolved handles through an `outMissingAssets` vector, matching
      `Scene::UpdateAnimators`'s convention, since this runs inside a parallel scene-system task.
    - `Scene/Components.h`: `AnimationGraphComponent` (`Graph`/`Skeleton` handles, `Playing`, the
      `AnimationGraphInstance`); added to `AllComponents` and `DuplicateEntity`'s `DuplicatableComponents`.
      Self-contained skeleton evaluation only (no per-node ECS joint entities yet -- that's
      `AnimatorComponent`'s more complex path, out of scope here).
    - `Scene.cpp`: new `"Animation Graph"` system (`UpdateAnimationGraphs`), registered after `"Animation"`;
      `GetBoneTransforms` falls back to it when there's no `AnimatorComponent`; the mesh-skinning-detection
      and asset-dependency-collection code paths that already checked for `AnimatorComponent` now also check
      for `AnimationGraphComponent`.
    - `SceneSerializer.cpp`: YAML read/write block, parameter overrides serialized through
      `NodeGraphSerializer::EmitValue/ReadValue` (promoted from that file's anonymous namespace to public
      static methods so this doesn't duplicate the tagged-variant encoding).
    - `Application.cpp`: `RegisterAnimationGraphNodeTypes()` called once from `SDL_AppInit`, right after
      `Log::Init()` -- engine-level, not editor-only, so it also covers a future standalone runtime.
    - `SceneHierarchyPanel.cpp`: a minimal "Animation Graph" inspector block (asset combo + drag-drop for
      Graph/Skeleton, Playing checkbox, a slider/checkbox per runtime parameter) -- this is the permanent
      parameter-tweaking UI, separate from the node-canvas *graph* editor Step 3/4 add; not a throwaway.
    - Fixed along the way: `GraphEvalContext::GetInput`/`GetProperty` originally called `std::get_if<T>` on a
      `NodeGraphValue` unconditionally, which is a hard compile error for any `T` that isn't one of the
      variant's alternatives (e.g. `AnimPose` on a "Pose" pin) -- added `kIsNodeGraphValueType<T>` in
      `NodeGraphTypes.h` and gated those calls behind `if constexpr`/`static_assert` on it.

- **Correction (September 2026):** the user asked "shouldn't AnimationGraph be a thing inside
  AnimatorComponent instead of a separate component?" and pointed out modern engines do this. They were
  right, and the reasons matter:
  - **UE5 does exactly this**: `USkeletalMeshComponent` has one `AnimationMode` enum
    (`AnimationSingleNode` vs. `AnimationBlueprint`) on the *same* component, not two component types.
    Unity's `Animator` always references a graph (a trivial one for a single clip) -- neither engine keeps a
    separate "simple" and "graph" component.
  - **The concrete bug this caused**: `AnimationGraphComponent` only implemented the "legacy self-contained"
    skeleton path (mutating the `Skeleton` asset's shared `Node` tree directly). But
    `Scene::UpdateAnimators`'s comments make clear that path is the *fallback* -- the actual path real
    imported skeletal meshes use is `NodeEntities` (joint entities driven via ECS `TransformComponent`,
    `GetBoneTransforms` reading their `WorldTransformComponent`). A rigged import like Fox populates
    `NodeEntities`, so the separate-component design would never have rendered it correctly, even with a
    valid `.nanimgraph` assigned. This was caught before it could waste the user's test time, but it should
    have been caught during Step 2's own design, not after.
  - **I was also wrong that skinned rendering wasn't wired up.** My Step 2 report said
    `Scene::GetBoneTransforms` "has no caller anywhere in the renderer" -- I had grepped for callers *outside*
    `Scene.cpp`/`Scene.h` and stopped there, missing that it's called from inside `Scene.cpp` itself
    (`SyncGpuScene` → `GetBoneTransforms` → `Renderer::SetMeshInstancesBones` → `m_boneMatrices` → GPU bone
    buffer → shader). The user's "animations work tho" on the Fox import confirms this path is live. Noted so
    the same shortcut (checking file-level grep hits instead of the actual call graph) doesn't get repeated.
  - **What changed**: `AnimationGraphComponent` was deleted. `AnimatorComponent` gained `AssetHandle Graph = 0`
    and `AnimationGraphInstance GraphInstance` (0 = single-clip mode, unchanged). `AnimationGraphInstance`'s
    API was reshaped to mirror `Animator`'s own dual-mode split: `Evaluate()` now just returns the raw
    `AnimPose*` (or `nullptr`) instead of applying it internally, so `Scene::UpdateAnimators` can apply it
    either way -- `ApplyPoseToSkeleton` for the self-contained case, or written straight into every mapped
    `NodeEntities` `TransformComponent` (the graph's full dense pose, unlike a single clip's sparse
    per-channel targets) for the joint-entity case -- exactly like it already chooses between
    `Animator::UpdateTransforms` and `Animator::UpdateNodeAnimation`. `GetBoneTransforms`,
    `SceneSerializer`, `SceneHierarchyPanel`'s inspector, `DuplicatableComponents`, the skinned-mesh-detection
    check and the asset-dependency collector all lost their separate-component branch and gained the
    `AnimatorComponent::Graph` field instead, net fewer lines than the separate-component version.
  - **Also fixed**: a latent, pre-existing Vulkan validation bug the Fox import exposed for the first time --
    `RasterPasses.cpp`'s G-Buffer pass issued a manual buffer barrier (for the previous frame's
    compute-skinning history buffer) from inside its `Execute` lambda, which runs between
    `vkCmdBeginRendering`/`vkCmdEndRendering`; `vkCmdPipelineBarrier2` with a buffer barrier is illegal there
    without `VK_KHR_dynamic_rendering_local_read`/`VK_EXT_shader_tile_image`, neither of which this device
    enables. Never triggered before because no skinned mesh existed in the project to make
    `skinnedHistoryValid` true. Fixed by moving the barrier into its own small `RGPassFlags::None` pass
    immediately before G-Buffer (same idiom already used for "Mip Feedback Clear"); the render graph executes
    passes in registration order, so ordering is guaranteed without a data dependency forcing it.
  - **Test graph added**: `Facerun/Assets/Models/Fox/Fox_WalkRun.nanimgraph`, hand-authored against Fox's real
    asset handles from `AssetRegistry.nxr` (`Fox.nskel`, `Fox_Walk.nanim`, `Fox_Run.nanim`) -- two `Clip`
    nodes into one `Blend2D` blended by a `"Speed"` parameter into `Output`. Confirmed working: the Speed
    slider blends Fox between Walk and Run live.
  - `Fox_WalkRun.nanimgraph` had no asset-registry entry (the recursive rescan that would have picked it up
    only runs after specific import events, not on every launch) -- hand-added its `Handle`/`FilePath`/`Type`
    entry to `Facerun/Assets/AssetRegistry.nxr` directly so the "Graph" combo could find it. A real "create
    new graph asset" action (writing its own registry entry, same as other asset types already do) is
    something Step 4's editor integration should cover so this doesn't need doing by hand again.

- **Step 3 (September 2026):** vendored `thedmd/imgui-node-editor` into `NoxCore/vendors/imgui-node-editor`
  (fetched: `imgui_node_editor.{h,cpp}`, `imgui_node_editor_{api,internal}.{cpp,h,inl}`, `imgui_canvas.{h,cpp}`,
  `imgui_bezier_math.{h,inl}`, `imgui_extra_math.{h,inl}`, `crude_json.{h,cpp}`), wired into
  `NoxCore/CMakeLists.txt` the same way `IMGUIZMO_SOURCES`/`IMGUIZMO_DIR` are.
  - **One compatibility patch needed**: `imgui_extra_math.inl` unconditionally redefined
    `operator*(float, const ImVec2&)`, which the vendored ImGui 1.92.9 WIP (`IMGUI_VERSION_NUM` 19286) already
    defines under `IMGUI_DEFINE_MATH_OPERATORS` -- a hard redefinition error, plus cascading "no matching
    function" errors elsewhere in the library from the same cause. Fixed by guarding it with
    `#if IMGUI_VERSION_NUM < 19002`, matching the version guard the same file already uses for the adjacent
    `operator==`/`operator!=` it also patches -- both errors disappeared once this one operator was guarded, so
    the "no matching function" errors were secondary fallout of the first, not separate bugs.
  - **`NodeGraphEditorPanel`** (`NoxEditor/src/Panels/`): domain-agnostic -- it only ever looks at
    `NodeTypeRegistry` entries whose `Domain` matches the open graph's `NodeGraph::Domain`, so the same panel
    will serve a future audio graph unchanged. Edits the loaded `NodeGraphAsset` in place, calling
    `Recompile()` after every structural or property edit (so a running scene picks changes up immediately
    without a save); `Ctrl+S`/File > Save writes it to disk via `NodeGraphSerializer`.
  - Pin/link identity: `ed::PinId` encodes `(nodeId << 20) | (isOutput << 19) | pinIndex` (no separate id
    table needed); `ed::LinkId` reuses a link's *input*-pin encoding, since an input pin accepts only one
    incoming link -- this keeps link deletion correct even across multi-select without depending on array
    index (which shifts as other links are erased in the same batch).
    Node positions aren't stored in imgui-node-editor's own settings file (`Config::SettingsFile = nullptr`);
    they round-trip through `GraphNode::EditorPosition`, seeded into `ed::SetNodePosition` once per node on
    first draw and read back every frame after `ed::EndNode()`.
  - Context menus (background right-click to add a node, node right-click for "Set as Graph Output") follow
    imgui-node-editor's own canonical pattern from its `application/blueprints-example`: triggered and drawn
    inside `ed::Suspend()`/`ed::Resume()`, since popups need to escape the canvas' pan/zoom transform.
  - Entry point is temporary, per the plan: `EditorLayer`'s Window menu gained an "Animation Graphs" submenu
    listing every `AssetType::AnimationGraph` in the registry; Step 4 replaces this with double-clicking a
    graph asset reference.

- **Crash fix (September 2026):** saving a graph in the new panel crashed the app on the following
  auto-reimport. Root cause: `out << YAML::Key << "Position" << YAML::Value << node.EditorPosition;`
  (`node.EditorPosition` is `glm::vec2`) never actually went through `YAML::convert<glm::vec2>` -- yaml-cpp's
  `Emitter::operator<<` has no overload for an arbitrary type, so overload resolution fell through to GLM's
  own templated stream operator (found via ADL on the `glm::vec2` argument, which matches *any* stream-like
  type, including `Emitter&`), silently writing `"vec2(0.000000, 0.000000)"` text instead of a YAML sequence.
  `SceneSerializer.cpp` doesn't have this bug because it defines its own exact (non-template)
  `operator<<(YAML::Emitter&, const glm::vec2&)` in the same namespace as its call sites, which wins over the
  ADL-found template -- `NodeGraphSerializer.cpp` didn't have the equivalent overload. Ported it over verbatim.
  On load, the malformed text failed `YAML::convert<glm::vec2>::decode`, and the resulting
  `YAML::TypedBadConversion` exception was never caught (only `YAML::LoadFile` itself was inside a try/catch),
  crashing whatever called `Deserialize` -- in this case the asset watcher's auto-reimport, which has nothing
  upstream prepared to catch an exception. Wrapped the rest of `Deserialize` in its own try/catch too, so a
  malformed file logs an error and fails the load instead of crashing the app. The already-corrupted
  `Fox_WalkRun.nanimgraph` on disk was hand-fixed to the correct `[x, y]` sequence format.
  - Also added: `ed::NavigateToContent()` once on first open (after node positions are seeded), so a graph
    with nodes spread out (like the Output node at x=600 in the test graph) doesn't open with some of them
    off-screen -- this is almost certainly what looked like "only 3 nodes, 2 Clip nodes" (the Output node was
    just outside the initial view, not missing).
  - **Follow-up link error**: the ported `operator<<(Emitter&, glm::vec2)` is a plain namespace-scope free
    function, not a class member -- unlike `YAML::convert<glm::vec2>`'s member functions (implicitly `inline`
    because they're defined inside the struct body, so duplicate definitions across TUs are fine), a free
    function defined the same way in two `.cpp` files has external linkage twice over and the linker rejects
    it as a duplicate symbol. Marked `static` (internal linkage) in `NodeGraphSerializer.cpp`; ordinary
    unqualified lookup within that one file still finds it, which is all it needs to do.

- **Visual fixes (September 2026)**: two things the user flagged as looking wrong once the panel actually
  rendered a graph.
  - **Clip nodes far wider than Blend2D/Output**: the "Clip" property is an `AssetHandle` (`uint64_t`), shown
    as its raw 19-digit number (`ImGui::Text("Clip: 6982408203583889678")`) -- that one line alone forces the
    node wider than everything else. Changed to resolve it against the asset registry and show the file's stem
    (`Fox_Walk`, `Fox_Run`) instead, falling back to the number only if the handle isn't found.
  - **Links plugging into the middle of pin text instead of a socket**: pins had no dedicated icon -- just
    `ed::BeginPin`/`EndPin` wrapped around a text label (`"-> Name : Type"` / `"Name : Type ->"`), so ed::
    anchored the link to the whole text's bounding box, and the "->"/"<-" arrows the label text used made it
    look like the arrows themselves were meant to be the connection point. First pass added a small hand-drawn
    circle; superseded below by the library's own real icon renderer once the user asked for the Blueprint
    look, which needed pulling that code in anyway.

- **Blueprint-style visuals (September 2026):** the user asked to reproduce the look of the library's own
  `blueprints-example` screenshots (UE4-Blueprint style: colored rounded node headers, shaped/colored pin
  icons). Investigated running the actual example: not practical here -- its polished look comes from
  `examples/blueprints-example/utilities/builders.h/.cpp`'s `BlueprintNodeBuilder`, which depends on
  `ImGui::Spring()`/`BeginHorizontal()`/`BeginVertical()` layout helpers that only exist in thedmd's own
  patched ImGui fork bundled at `examples/external/imgui` -- not part of the standalone library already
  vendored, not part of this project's ImGui, and the examples also need their own GLFW/DX11 app scaffolding
  incompatible with this engine's Vulkan/SDL3 setup. Rather than either running an incompatible external app or
  hand-approximating everything, split the difference:
  - **Vendored `examples/blueprints-example/utilities/drawing.{h,cpp}` and `widgets.{h,cpp}` verbatim** (added
    to `IMGUI_NODE_EDITOR_SOURCES`) -- these are self-contained pin-icon renderers (`ax::Drawing::DrawIcon`:
    Circle/Square/Grid/RoundSquare/Diamond/Flow, filled or hollow) with no dependency on the patched-fork
    layout helpers, confirmed by grep before vendoring. `builders.h/.cpp` (the part that *does* need them) was
    deliberately **not** vendored -- it wouldn't compile.
  - `NodeGraphEditorPanel`'s pins now use `ax::Widgets::Icon(...)`, shaped and colored by the pin's declared
    Type string (`PinIconType`/`PinColor`): `RoundSquare` for the domain's composite type ("Pose"), colored
    `Circle` for scalars -- the same "shape says which types can connect, color says which type" convention
    Blueprint-style editors use, extendable per-type as new pin types are added.
  - The colored rounded header band is a **hand-written approximation**, not ported code: captures the title
    row's screen-space rect (`ImGui::GetItemRectMin/Max()`, the same technique `BlueprintNodeBuilder::End()`
    uses for its texture), then paints a flat category color (`CategoryHeaderColor`: Sources=blue,
    Blending=orange, Sinks=green) via `ed::GetNodeBackgroundDrawList(nodeId)->AddRectFilled(..., ed::GetStyle().
    NodeRounding, ImDrawFlags_RoundCornersTop)` right after `ed::EndNode()`. Gets the visual result (a colored,
    rounded header distinguishing node categories) without the gradient header texture or the patched-fork
    layout system.
  - **Reference build**: cloned `thedmd/imgui-node-editor` to `E:\dev\VulkanAdventure\imgui-node-editor` (a
    separate, unrelated reference checkout, not part of Noxiouse) and built its examples with CMake+Ninja/MSVC
    so the actual `blueprints-example.exe` could be run for visual comparison. Its committed `external/imgui`
    (1.84 WIP) is older than `imgui_node_editor.cpp` at `master` HEAD expects; two local-only compatibility
    fixes were needed to link: `IM_TRUNC=ImFloor` (compile definition) and dropping a redundant local
    `ImGui::GetKeyIndex` shim that collided with 1.84's own real one (a version-number guard wasn't reliable
    here -- 1.84 predates the threshold used elsewhere in the same file for the same API transition, but still
    has this one function). All 5 examples (`canvas`, `simple`, `widgets`, `basic-interaction`, `blueprints`)
    built clean; `build/bin/blueprints-example.exe` is the one with the UE4-Blueprint styling.

- **Library switch: thedmd/imgui-node-editor -> Fattorino/ImNodeFlow (September 2026).** After seeing the
  reference build, the user asked to actually get that look, which meant porting `BlueprintNodeBuilder`'s
  `Spring()`/`BeginHorizontal()`/`BeginVertical()` layout helpers -- not a standalone utility like
  `drawing.cpp`/`widgets.cpp` were, but a genuine patch to ImGui's *core* (new `ImGuiLayout` state persisted
  per-ID across frames, new fields on `ImGuiWindowTempData`), only available in thedmd's own patched ImGui fork.
  Doing that for real would mean patching NoxCore's shared vendored ImGui -- used by every other editor panel --
  and adapting ~300 lines of internal-state-dependent code across a large internals gap (1.84 -> 1.92.9). Given
  the user's "is there no new library for this" and a comparison against the actively-maintained options on
  [ocornut/imgui's Useful-Extensions wiki](https://github.com/ocornut/imgui/wiki/Useful-Extensions#node-editors),
  switched to **Fattorino/ImNodeFlow** (2024-2026, "Node based editor/blueprints for ImGui") instead of
  building the equivalent by hand:
  - Plain, unpatched ImGui (just needs `IMGUI_DEFINE_MATH_OPERATORS`, its documented, standard requirement --
    added as a project-wide `target_compile_definitions` on `NoxCore`, matching its own README's CMake example).
    Built-in `NodeStyle`/`PinStyle` presets (colored, rounded headers; shaped/colored pin sockets) replace all
    of the hand-drawn header-band and pin-icon code from the previous step -- zero custom styling code now.
  - Vendored to `NoxCore/vendors/ImNodeFlow` (`include/ImNodeFlow.h`, `src/ImNodeFlow.{cpp,inl}`,
    `context_wrapper.h`, `imgui_bezier_math.{h,inl}`, `imgui_extra_math.{h,inl}`); compiled clean against the
    project's ImGui with no patches needed (its `imgui_extra_math.inl` already version-guards its `operator*`
    fix at `IMGUI_VERSION_NUM < 19268`, unlike thedmd's unguarded copy that needed a local patch in Step 3).
    `thedmd/imgui-node-editor` was removed from `NoxCore/vendors` and `CMakeLists.txt` entirely.
  - **Architecture note**: ImNodeFlow's usual pattern is one C++ class per node type (`addIN<T>`/`addOUT<T>`
    with concrete C++ types, values resolved in-editor via `behaviour` lambdas). That doesn't fit a data-driven,
    domain-agnostic `NodeTypeDesc` registry, so `NodeGraphEditorPanel.cpp` defines a single `GenericFlowNode`
    whose pins are declared dynamically from whatever `NodeTypeDesc` it's built from -- the domain's pin Type
    string maps to a concrete C++ type only so `ConnectionFilter::SameType()` (a `typeid` comparison) still
    enforces correct connections; the actual value is never read (evaluation stays entirely in
    `GraphCompiler`/`GraphEvalContext`, never in the editor). `PoseTag` is an empty tag struct filling this role
    for the "Pose" pin type.
  - Node/link state syncs from ImFlow back into `NodeGraph` once at load (`RebuildFlowFromGraph`, including
    replaying saved links via `Pin::createLink`) and then every frame (`SyncGraphFromFlow`): positions from
    `BaseNode::getPos()`, node deletions by diffing `m_FlowNodes` against `ImNodeFlow::getNodes()` (which
    prunes Delete-key-removed nodes itself), and links fully rebuilt each frame from live pin connection state
    (`Pin::isConnected()`/`getLink()`) rather than tracked as discrete events -- simpler and immune to ordering
    issues at editor-authoring scale. `GraphNode*` is never held across a frame (`NodeGraph::Nodes` can
    reallocate on insert); `GenericFlowNode` only stores the stable `GraphNode::Id` and looks it up through the
    panel (`FindGraphNode`) whenever needed, same discipline as the previous panel.
  - Right-click-to-create-node and "Set as Graph Output" now go through ImNodeFlow's own built-in
    `rightClickPopUpContent` hook instead of manual `Suspend`/`Resume`/`ShowBackgroundContextMenu` plumbing.
    Node and link deletion (Delete key) needed no code at all -- built into the library.

- **Reverted to thedmd/imgui-node-editor, behind a new backend interface (September 2026).** The user pointed
  out `thedmd/imgui-node-editor`'s last two commits ("fixing for modern imgui", Feb-Mar 2026) specifically fix
  the library for modern ImGui. Re-checked: those commits only touch `imgui_node_editor.cpp` itself (the
  `FloorRect`/`ImGui::GetKeyIndex` compatibility shims from Step 3's investigation) -- they don't touch the
  *committed* `external/imgui` the upstream examples bundle (still 1.84 WIP), which is why the
  `E:\dev\VulkanAdventure` reference build still needed local patches: it was compiled against that stale
  bundled copy, not against real modern ImGui. Against this project's actual ImGui 1.92.9, a fresh vendor of
  `imgui_node_editor.cpp` needed **zero** patches to the core library logic -- only the same one-line
  `imgui_extra_math.inl` `operator*` guard from Step 3 (this time using the more precise `IMGUI_VERSION_NUM <
  19268` threshold, copied from ImNodeFlow's already-correctly-guarded copy of the same file rather than the
  looser guess used the first time). `NoxCore/vendors/imgui-node-editor` re-fetched fresh from master
  (including the fix commits) plus the blueprints-example pin-icon utilities, same as Step 3.
  - **New scalable seam, per the user's request**: `NoxEditor/src/Panels/NodeGraph/` now holds
    `INodeGraphCanvasBackend` (one method: `Draw(NodeGraph&, onGraphEdited)`), `NodeGraphPropertyEditor.{h,cpp}`
    (the shared `DrawNodeGraphProperty` helper, library-agnostic), and `ThedmdCanvasBackend.{h,cpp}` (all of
    the ax::NodeEditor-specific code: pin icons, the hand-painted category header band, link/deletion/create-
    node handling -- exactly what Step 3 already had, just moved behind the interface). `NodeGraphEditorPanel`
    is now a thin shell (asset, title, dirty state, Save(), the ImGui window) that owns one
    `Scope<INodeGraphCanvasBackend>`, currently always a `ThedmdCanvasBackend`. Swapping to a different canvas
    library later (as just happened once already, both directions) means adding a new backend file and
    changing the one `CreateScope<...>()` line in the panel's constructor -- not touching the panel, the
    property editor, or the `NodeGraph`/`GraphCompiler` core at all.
  - Visual styling (pin icons, category header color) is back to where Step 3 left it before the detour;
    further "modern Unreal Engine style" polish is the immediate next thing to work on with the user,
    interactively, now that both a real reference build and a working baseline exist to compare against.

- **Bug found by testing (September 2026): a node added in the editor had no way to become usable.** The user
  added a `Clip` and an `Output` node and got no animation. Root cause: `NodeTypeDesc` never carried default
  property values, so a freshly created `GraphNode` had a completely empty `Properties` map -- a new `Clip`
  node had no `Clip`/`Loop`/`Speed` fields to even look at, let alone edit, so there was no way to say which
  clip it should play. (Not a "next phase" gap -- this blocked the basic "author a graph in the editor" loop
  Step 3 exists for, so fixed immediately rather than deferred.)
  - Added `NodeTypeDesc::DefaultProperties` (seeded into a new node's `Properties` at creation time only;
    existing/loaded nodes are untouched) and populated it for `Clip` (`Clip: 0, Loop: true, Speed: 1.0`) and
    `Blend2D` (`ParameterName: "", X: 0.0`).
  - Fixing that exposed the other half of the same gap: the `Clip` property (a `uint64_t` `AssetHandle`) was
    rendered **read-only** in `DrawNodeGraphProperty` -- even with the field now visible, there was no way to
    actually pick a clip. Upgraded it to a filterable combo over the asset registry. Noted honestly: this lists
    every asset in the project, not just `AnimationSequence`s -- `NodeGraphValue`'s `uint64_t` alternative
    carries no "this property means an `AssetHandle` to `AssetType::X`" metadata to filter on yet. A real
    typed-asset-picker (matching the property's intended asset type) is Step 4 inspector territory; this is the
    minimum needed to unblock authoring a working graph today.

- **Follow-up bug from the same testing pass: the asset picker's dropdown opened in the wrong place and wasn't
  clickable.** Root cause: a plain `ImGui::BeginCombo` doesn't work inside an imgui-node-editor node -- this is
  a known, still-open upstream limitation (github.com/thedmd/imgui-node-editor issues #48 and #154; thedmd
  himself, closing #154: "no other way to handle combobox other than [the] workaround... I tried every approach
  to fixup position without modifying ImGui itself"). The documented workaround, taken from the library's own
  `widgets-example.cpp` (`develop` branch): the node body only draws a **button** that records "show the
  picker"; the actual popup is drawn **after `ed::EndNode()`**, wrapped in `ed::Suspend()`/`ed::Resume()` (which
  is what makes it render in real screen coordinates instead of canvas-transformed ones).
  - `ThedmdCanvasBackend` now special-cases `uint64_t` properties before ever calling the shared
    `DrawNodeGraphProperty`: `DrawAssetPickerButton` draws the inline button and queues a pending request
    (node id, property name, `AssetType` hint); `DrawPendingAssetPicker` -- called from the same
    `Suspend()`/`Resume()` block the right-click context menus already use -- draws the actual filterable list
    once every node has finished drawing for the frame. `NodeGraphPropertyEditor`'s generic `uint64_t` handling
    stays a plain read-only label -- the deferred-popup dance is backend-specific by nature (a different canvas
    library might not have this limitation at all), so it doesn't belong in the domain-agnostic shared helper.
  - Also addressed "shows all files instead of only animations": added `NodeTypeDesc::PropertyAssetTypeHints`
    (property name -> an asset-type string, e.g. `"AssetType::AnimationSequence"`, the exact string
    `AssetTypeToString`/`AssetTypeFromString` already use -- kept as an opaque string rather than a real
    `AssetType`, so `NodeGraph/NodeType.h` still doesn't depend on the asset system, same principle as
    `NodePinDesc::Type`). `Clip`'s `"Clip"` property is hinted to `AnimationSequence`; the picker filters to it.
  - **Not solved**: "only from that mesh" (only animations belonging to the same character). A `.nanimgraph`
    asset isn't bound to one specific skeleton at authoring time -- it can be assigned to any
    `AnimatorComponent::Skeleton` -- so there's no reliable signal *in the node graph editor itself* for which
    mesh's clips to prefer. Left as full-registry-filtered-by-type rather than force a fragile heuristic
    (e.g. guessing from another clip already set on the same graph); worth revisiting if it's still a problem
    once graphs are typically authored per-character in practice.

- **Bug found by testing: connecting Clip -> Output produced "no valid OutputNode" on every edit.**
  `graph.OutputNode` only ever got set through the right-click "Set as Graph Output" menu item -- an easy-to-
  miss manual step, and every edit (even just wiring the one obvious sink) recompiled and re-logged the error
  until it was done. Fixed in `GraphCompiler::Compile` (not the editor): when `OutputNode` doesn't resolve, look
  for a node whose type declares zero output pins -- structurally the only kind of node "the graph's result"
  can mean, in any domain -- and use it automatically if there's exactly one. Ambiguous graphs (zero or several
  such sinks) still need the explicit right-click, with a clearer error message. Kept in the compiler (not
  node-creation time) so it also fixes hand-authored/loaded graphs, not just ones built through the editor.
  - **Follow-up crash: adding a second Clip -> "vector subscript out of range".** The editor recompiles the
    live asset after every edit (`NodeGraphAsset::Recompile()` replaces `Compiled`), but
    `AnimationGraphInstance` only `Init()`ed its `GraphEvalContext` when the graph *handle* changed. Adding a node
    grows the compiled graph (2 -> 3 nodes), so `EvaluateGraph` indexed `m_NodeState[2]` / `m_OutputValues[...]` on
    buffers still sized for the old graph. Never surfaced before because earlier live edits only rewired links
    (same node count). Worse than the crash alone: if slots reshuffle, a stale `std::any` of the wrong type could
    sit in a slot and `GetState<float>` would dereference a null `any_cast`, so a bounds check wasn't enough.
    Fix: `NodeGraphAsset::CompileVersion`, bumped by every `Recompile()`; `AnimationGraphInstance` remembers the
    version its context was built for, `Sync()` re-`Init()`s when it changes (node playheads reset on a
    structural edit, the only safe option once slots can move), and `Evaluate()` bails if it's stale.
  - **Step 5 built (September 2026).**
    - **Blend2D rework** (no compatibility path, per the contract): the `ParameterName`/`X` string properties are
      gone. `Blend2D` now has a third input pin `Alpha` (Float); unconnected, `GraphEvalContext::GetInput` already
      falls back to the property named after the pin (`Alpha: 0`), so one pin is both the constant and the
      wire-in point. Existing saved Blend2D nodes keep the two dead properties until re-authored.
    - **`GetParameter` node** (`Inputs` category, Float output): reads the instance parameter named by its
      `"Parameter"` string property (bool/int read as 0/1/number, unknown = 0 -- `NodeGraphValueToFloat` in
      `NodeGraphTypes.h`). Nobody types the name: the create-node menu lists `Parameters / <name>` for each
      declared parameter, which makes a pre-filled node. That works off the **`kParameterReferenceProperty`
      convention** ("a node that reads a parameter has a string property `Parameter`"), not any node type name,
      so the generic editor stays domain-agnostic and a future audio graph gets it for free; such types are hidden
      from the plain list (useless with no parameter chosen).
    - **Parameters panel** in `NodeGraphEditorPanel` (backend-independent; edits `NodeGraph::Parameters`): add,
      rename (applied per edit, skipping empty/duplicate names; renaming also re-points nodes that read it),
      retype Float/Bool/Int, default, delete. Every change recompiles, which bumps `CompileVersion`.
    - **`AnimationGraphInstance::Sync`** now prunes instance parameters the graph no longer declares and resets
      one whose declared type changed -- fixes the "why is Speed there when I never added it" leftovers now that
      deleting a parameter is possible. The Animator inspector's parameter float is a `DragFloat`, no longer a
      0-1 slider.
    - **Scripts**: `Animator_SetFloat/GetFloat/SetBool/GetBool` Coral internal calls (`DotNetBackend.cpp`),
      `ComponentType.Animator = 2` (also handled in `HasComponent`), and a new C# `Nox.AnimatorComponent`
      (`NoxScriptCore/Source/AnimatorComponent.cs`). Set* stores any name (a script may run before the graph
      loads; unknown names are pruned on the next `Sync`).
  - **Step 5 feedback pass (September 2026), three fixes from testing:**
    - **Delete also deleted the scene entity.** `EditorLayer::OnKeyPressed` runs from raw SDL key events and only
      checked "is an ImGui text field active", so Delete in the graph window removed the node *and* the entity
      selected in the hierarchy. Same for Q/W/E/R gizmo keys, Ctrl+S/N/O, and viewport picking. Now each
      `NodeGraphEditorPanel` reports `WantsInput()` (focused *or* hovered, child windows and its own popups
      included) and `IsHovered()`; `OnKeyPressed` stands down while any graph window wants input. Mouse picking
      gates on **hover only** -- gating on focus would swallow the first click back in the viewport after using
      the graph window, since focus only changes the following frame.
    - **Alpha ranged 0-1.** `NodeTypeDesc::PropertyRanges` (property -> min,max), an editor hint like
      `PropertyAssetTypeHints`: a float with a range renders as a slider with `ImGuiSliderFlags_AlwaysClamp`
      (Ctrl+click typing can't escape it) instead of an open drag box. `Blend2D.Alpha` = 0..1. Evaluation
      already clamped (`BlendPoses`), so a wired `GetParameter` beyond 0..1 is harmless. Other float properties
      (Clip `Speed`, parameter defaults) stay unbounded on purpose.
    - **Parameters no longer in the right-click list; a searchable palette instead.** Unreal's model: the
      right-click menu is an action palette with a search box, and variables appear as "Get <name>" (plus a
      "My Blueprint" variables panel you drag from). The create-node menu is now a search box (focused as it
      opens, Enter takes the first match) over one flat entry list: each node type, plus one `Get <name>` per
      declared parameter under a `Variables` category. Empty search browses by category submenus
      (Sources / Blending / Sinks / Inputs / Variables); typing searches label and category across all of them at
      once, so it stays usable at hundreds of node types.
    - **Drag a parameter onto the canvas (Unreal's other route), added right after.** Each row in the Parameters
      list has a `::` drag handle; dropping it on the canvas creates a node reading that parameter at the drop
      position. The seam: the payload name `kNodeGraphParameterPayload` lives in `INodeGraphCanvasBackend.h`
      (the panel is the source, a backend is the target, since only the backend can turn a screen position into
      a graph position). `ThedmdCanvasBackend::HandleParameterDrop` uses `BeginDragDropTargetCustom` over the
      canvas window's inner rect -- the canvas has no item to hang a normal target on -- inside the same
      `Suspend`/`Resume` block as the menus, and shares a new `CreateNode(...)` with the create-node menu.
      Only one "reader" type exists (Get), so the drop makes that; a "Set"-style type later would need a choice.
  - **Step 4 built (September 2026).** No new library or architecture, just the plumbing the plan described:
    - `EditorLayer::m_AssetOpeners` (`AssetType` -> `std::function<void(AssetHandle)>`) and `OpenAsset(handle)`;
      `AnimationGraph` -> `OpenNodeGraphEditor`. A future asset editor is one entry here, not new double-click
      code in every panel that shows references. Panels get a `SetOpenAssetCallback(...)` and know nothing about
      editor windows.
    - `ContentBrowserPanel`: double-clicking any asset calls the callback (the existing `MeshSource` import-
      dialog double-click is unchanged and takes precedence for that type). New **New Animation Graph** button +
      modal (name validation and already-exists check, same shape as "New C# Script"): writes
      `CreateEmptyAnimationGraph()` (just an Output node, already the graph's output) via
      `NodeGraphSerializer`, then calls the already-public `EditorAssetManager::ScanAndRegisterNewAssets(dir)` --
      this is the "create a graph asset without hand-editing `AssetRegistry.nxr`" gap flagged after Step 2 -- then
      opens it in the editor.
    - `SceneHierarchyPanel`: double-clicking the Animator's Graph combo opens its editor (checked right after
      `BeginCombo`, while the combo is still the last item -- inside the popup it isn't).
  - Noted for later, not acted on now (the user's own words: "we can do that later if needed"): rename/reshape
    `Clip` into something like an "Animation Player" node, since "Clip [node] shouldn't own the clip[s]" --
    worth revisiting once the state-machine work (Step 7) clarifies how multiple clips/states actually want to
    be authored, rather than guessing at the right shape now.

- **Step 6 — Character controller (built, awaiting user test).**
  - Native `CharacterController3DComponent` (core, not a script -- the user's call) wrapping Jolt's
    `CharacterVirtual`, ported from `Samples/Tests/Character/CharacterVirtualTest.cpp`: capsule shape offset so the
    entity origin is the feet, supporting volume = lower cap, ground-velocity/jump/gravity velocity logic and
    `ExtendedUpdate` (stick-to-floor 0.5, walk-stairs step = `StepHeight`) exactly as the sample. Collision filters
    use `PhysicsLayers::CHARACTER`.
  - Jolt types stay in `JoltPhysics3DScene` (`m_EntityToCharacterMap`); `IPhysics3DScene` only gained
    `CreateCharacter/DestroyCharacter`. Characters are created in `Init()`; stepped inside the fixed 1/60 loop right
    before `PhysicsSystem::Update`; results (translation, `IsGrounded`, `Velocity`) written back after the loop like
    rigid bodies. The controller owns translation only -- rotation (yaw) stays with the script.
  - Component fields: authored `Radius, Height, StepHeight, MaxSlopeDegrees, GravityScale, AirControl`; script
    input `MoveVelocity`, `JumpSpeed` (one-shot, consumed by the next step); runtime `IsGrounded, Velocity`.
    Serialized (authored fields only), inspector block + Add Component entry; "Physics 3D" system now declares
    `Write<CharacterController3DComponent>`.
  - Scripting: `ComponentType.CharacterController = 3`, `Nox.CharacterControllerComponent`
    (`SetMoveVelocity/Jump/IsGrounded/Velocity`), four `Character_*` internal calls, `KeyCode.Space/LeftShift`.
    Sample script `Facerun/Assets/Scripts/Source/CharacterMovement.cs`: WASD (+Shift run, Space jump), turns the
    entity towards its movement, sets Animator params `Speed` and `IsGrounded`.
  - Known limitations: no transform interpolation between 60 Hz steps (same as rigid bodies); controllers added at
    runtime after `Init()` aren't created; 3D physics components aren't copied by Duplicate (pre-existing).
  - **Test:** put `CharacterController3DComponent` + `CharacterMovement` script on the Fox entity (Animator graph
    needs a `Speed` parameter; `Fox_WalkRun` has it), with a static box collider floor; Play; walk/run/jump. Tune
    `ModelYawOffset` if the Fox faces the wrong way. C# is not compiled by me -- rebuild the script projects.
