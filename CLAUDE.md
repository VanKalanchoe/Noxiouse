# Noxiouse / Nox engine -- notes for Claude

## How the user wants the work done
- Build it the Unreal Engine 5 way: data-verified, no hacks, never half-baked. Say plainly what is tested and what is not.
- Test guides: very explicit, step by step, and **every step names the program it runs in** -- NoxRuntime, NoxEditor or NoxNetTest
  (server and client are both NoxRuntime; scripts are rebuilt by opening the project in NoxEditor once).
- **Never shorthand in a guide** (the user got lost at "Texture Coordinate -> Panner -> Texture Sample"): one action per line, the
  exact menu path / button label, how to do it (e.g. graph editor: right-click the empty canvas > type in *Search nodes...* > click
  the node; inputs are pins on a node's left, outputs on its right; drag from an output pin to an input pin to connect), the exact
  field and pin names as the UI shows them, the values to type, and what the user should see after. **Check every name in the code
  before writing it** (node type names, pin names, property labels, menu items) -- and that the thing is really reachable in the UI.
  Say what each node / setting does in a few words.
- The user writes English (sometimes German); answer in English.
- **Every new feature also goes into the docs site** in the same commit (the user asked for this always): the user manual
  `docs-site/assets/manual-pages.js` (how to use it, step by step, naming the program), the technical pages
  `docs-site/assets/docs-pages.js` with their Sphinx mirrors `docs-site/*.md` + `docs-site/index.md` (toctree, What's New), and new
  source files in `docs-site/assets/file-db.js`. Only what is built and say what is not yet. Check it renders (open the page in
  Chromium; table cells cannot contain `|`, even escaped).
- **Checklist before every commit of a feature or fix** (the user asked for this in every session -- do not skip it):
  1. user manual page(s) in `manual-pages.js` updated (steps name the program; troubleshooting for what the user hit);
  2. technical page in `docs-pages.js` and its `docs-site/*.md` mirror updated together (same text), What's New in `index.md`;
  3. new / moved source files in `file-db.js`;
  4. the plan doc (`docs/*_Plan_*.md`) status and `docs/Backlog_2026.md` updated, with fixes after the user's tests noted;
  5. pages opened in Chromium (no script errors, no raw `|` rows), then commit, push, and a test guide naming every program.
- Branch: develop on `claude/read-future-md-6zyo6c`, push with `git push -u origin claude/read-future-md-6zyo6c`. No pull requests
  unless asked. No model ids in commits, code or docs.

## The user's local test assets (on their machine, NOT in git -- never assume they are missing)
`.gitignore` excludes `Facerun/Assets/Models` and `*.nanim`, and some test scenes are not committed, so a fresh clone (this cloud
container) does not have them. The user has, under `Facerun/Assets`:
- **Animated character UAL1_Standard** (a skinned mesh with a skeleton) and **UAL1_Standard_RM** (its root-motion variant), set up in
  the scene **`AnimationTest2.nox`** with the scripts `CharacterMovement` (WASD/Shift/Space, feeds the graph `Speed` m/s,
  `VerticalSpeed` m/s, `IsGrounded`), `IKTargetTest` and `CameraFollow`, stairs, a ramp and IK targets (`LookTarget`, `HandTarget`).
- Animation graphs **`UAL_Locomotion.nanimgraph`** (state machine Locomotion / JumpStart / JumpLoop / JumpLand, Speed blend space,
  LookAt spine_03 + Head, TwoBoneIK hand_r, FootPlacement) and **`UAL_Locomotion_RM.nanimgraph`** (the root-motion one).
- Asset handles (from AnimationTest2.nox): UAL1_Standard mesh 16531713622870338582, animation 17644891489966042489,
  skeleton 6087766411216149246, graph 8121772498988545577; the RM variant mesh 10922256994577822127, animation
  7687004529706626963, skeleton 16507523730877177931, graph 5969131657674364168.
- The Fox model under `NoxEditor/assets/models/Fox` (also ignored).
When a feature needs an animated character (animation replication, IK, root motion, montages), use these and ask the user to test
with them; reference them by handle or path, and say that the files live only on the user's machine.

## Engine facts worth keeping
- World unit: centimetres in Facerun (WorldUnits); positions are double precision.
- MCP plan: `docs/MCP_Plan_2026-11.md` (MCP0-MCP5 built). Shared server code is the `NoxMcp/` library (protocol, HTTP, token,
  game tools); the editor's tools are `NoxEditor/src/Mcp`; NoxRuntime's server is `NoxRuntime/src/McpRuntime` (`--mcp`,
  `--mcp-port` 8770, `--mcp-access` write by default -- `quit` needs execute). Editor MCP port 8765; multiplayer Play's games with
  "Players' MCP servers" use editor port + 1 (dedicated server) / + n (player n). One token per project in `<Project>/.nox/mcp-token`.
  When the user tests runtime features, remind them to **build NoxRuntime too** (building NoxEditor does not). Offline tests:
  `Tests/McpTests.inl` + `Tests/ToolsTest.cpp` in NoxToolsTest (226 checks).
- Stylized rendering / material graph plan: `docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md` (step 1 Toon and step 2
  outlines + Post Process Volume built and tested; step 3 Custom Lit with hand-written Material Shaders built; step 4a material graph built (`.nmatgraph`, `Renderer/MaterialGraph.h`, `MaterialGraph.slang`; graphs in the generated `MaterialGraphs.slang`
  (Material Shaders in `CustomLitShaders.slang`; pipelines cached by source hash, not force-compiled), the G-buffer resolve's variant `GBufferMaterialGraph.slang` + the forward variant; `GpuMaterial::graphSlot` /
  `graphParameters`; `.nmat` NMA6 `SurfaceGraph` + `GraphParameters`); 4b part 1 Opacity Mask built and tested (`opacity_mask()` per graph, `VisibilityBufferMaterialGraph.slang` for Masked
  buckets + shadow mask depth, editor overlays' variants; shared `material_graph_mask_cuts`); generated variants build on high-priority
  threads (drafts and remembered compile times were tried and removed); start-up compiles `Material_TransparentLit_Mesh.slang` first (`Device::precompileShaderModules`); 4b part 2 WPO built (`world_position_offset()`, `material_graph_offset` in
  `MaterialGraph.slang`; WPO materials in the Masked buckets; generated `ForwardMeshMaterialGraph.slang`; `previousTimeSeconds` for motion vectors; bounds by
  `graphMaxDisplacement`; tested); 4c part 1 per-object graph parameters built (`MaterialComponent::GraphParameters`, C# `MeshComponent.SetScalarParameter`
  etc., script API 23; tested); 4c part 2 Opacity Mask in ray tracing built (`RayHitAlpha`/`RayHitCoverage` + `NOX_MATERIAL_GRAPH_MASK` ->
  `material_graph_ray_mask`; `MaterialGraphMasks.slang`; `<name>GraphMask.slang` variants, `Renderer::updateRayTracingGraphMasks`; tested; forward shader's secondary-ray functions `[noinline]` --
  its fragMain was 4.7 MB SPIR-V and ~30 s in the GPU driver; "GPU driver compiled ... in N ms" log; modules from `CompileModulesNow` pinned until a pipeline takes them; graph variants compiled in parallel; `CustomLitStatus::GraphsLive` -> "Built on opaque surfaces" status); 4c Custom Lit output,
  graph surface at ray hits, WPO in the BLAS open; surface
  code, Physical / Stylized setting open). Custom Lit: `.slang` assets (`AssetType::MaterialShader`) with `light()` / `ambient()`
  (`CustomLit.slang`), generated into `<exe>/shaders/Generated` (`Renderer/CustomLitShaders.h`), lit by the "Custom Lit" pass after the
  deferred lighting (G-buffer model bits 0xC0) and the forward variant (`forwardLitPipeline()`); push-constant vec4s sit on 16 bytes. Post effects + outlines live in `PostProcessSettings`; a scene's
  `PostProcessVolumeComponent` (saved, ships) replaces the editor's Renderer Settings copy (not saved); outlines: `SceneOutline.slang`. `MaterialData::ShadingModel` (Default Lit / Unlit / Toon) replaced `Unlit`; toon pixels use G-buffer flag 0x40 and carry
  their settings in the lobe target; lighting in `ToonShading.slang`. Material Editor labels use Unreal's names (labels only).
- Networking plan: `docs/Networking_Plan_2026-11.md` (NET0-NET5 built; NET6 part 1 built: editor multiplayer Play, NetSession, Package Server; Steam backend and NoxServer target open). Offline network tests: `Tests/NetTest.cpp`
  (NoxNetTest; `--offline` skips the socket tests).
