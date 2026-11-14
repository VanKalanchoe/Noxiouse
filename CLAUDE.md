# Noxiouse / Nox engine -- notes for Claude

## How the user wants the work done
- Build it the Unreal Engine 5 way: data-verified, no hacks, never half-baked. Say plainly what is tested and what is not.
- Test guides: very explicit, step by step, and **every step names the program it runs in** -- NoxRuntime, NoxEditor or NoxNetTest
  (server and client are both NoxRuntime; scripts are rebuilt by opening the project in NoxEditor once).
- The user writes English (sometimes German); answer in English.
- **Every new feature also goes into the docs site** in the same commit (the user asked for this always): the user manual
  `docs-site/assets/manual-pages.js` (how to use it, step by step, naming the program), the technical pages
  `docs-site/assets/docs-pages.js` with their Sphinx mirrors `docs-site/*.md` + `docs-site/index.md` (toctree, What's New), and new
  source files in `docs-site/assets/file-db.js`. Only what is built and say what is not yet. Check it renders (open the page in
  Chromium; table cells cannot contain `|`, even escaped).
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
- Networking plan: `docs/Networking_Plan_2026-11.md` (NET0-NET5 built; NET6 part 1 built: editor multiplayer Play, NetSession, Package Server; Steam backend and NoxServer target open). Offline network tests: `Tests/NetTest.cpp`
  (NoxNetTest; `--offline` skips the socket tests).
