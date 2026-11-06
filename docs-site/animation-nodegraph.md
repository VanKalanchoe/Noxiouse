# Skeletal Animation, Animation Graphs, Sequencer & Generic NodeGraph (`NoxCore/Animation` & `NodeGraph` — 22 Files)

Noxiouse provides a complete skeletal animation, visual state-machine/blend-space **Animation Graph** (`NoxCore/src/NoxCore/Animation/`), multi-track **Sequencer Timeline**, and a reusable domain-agnostic **NodeGraph** compiler and runtime (`NoxCore/src/NoxCore/NodeGraph/`).

---

## 1. Plain-English Tour of the Animation & Sequencer Pipeline

### A. Skeletons, Clips, Sync Markers, Root Motion & Parallel Evaluation
- **Skeletons & Clips ([`Skeleton.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/Skeleton.h), [`AnimationSequence.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/AnimationSequence.h))**:
  - Supports glTF 2.0 node hierarchies, multiple skins, `Step`, `Linear`, and `CubicSpline` keyframe interpolation, morph target weight curves (`MorphCurves`), and named timeline events (`AnimationNotify`: both gameplay events and **`Sync:` phase markers** like `Sync:LeftFoot`, `Sync:RightFoot`).
- **Two-Stage Parallel Evaluation (`Scene::UpdateAnimators`)**:
  - **Stage 1 (Main Thread)**: `AnimationGraphInstance::Sync` non-blockingly checks if all referenced clips/montages are loaded (requesting any missing clips via `AssetManager::RequestAsset`).
  - **Stage 2 (Parallel Workers)**: `JobSystem::ParallelFor` evaluates every entity's `Animator` / `AnimationGraphInstance` concurrently across CPU cores, computing bone skinning matrices (`BoneMatrices` and `PrevBoneMatrices` for accurate per-pixel velocity buffers!).
  - **Stage 3 (Main Thread)**: Consumes crossed `AnimationNotify` events (routing them to `ScriptEngine::OnAnimationEvent(entity, name)`) and applies extracted horizontal **Root Motion** deltas (`ConsumeRootMotion`) directly to the entity's `CharacterController3DComponent`, `RigidBody3DComponent`, or `TransformComponent`.

### B. All 11 Animation Graph Node Types ([`AnimationGraphNodes.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/AnimationGraphNodes.cpp))

| Node Type | Category | Plain-English Behavior |
| :--- | :--- | :--- |
| **`Clip`** | Sources | Plays an `AnimationSequence` asset (`Loop`, `Speed`, `StartPosition`) and outputs an `AnimPose` while collecting crossed notifies. |
| **`Blend2D`** | Blending | Linearly blends `PoseA` and `PoseB` by `Alpha` (`0..1`) using `glm::mix` for translation/scale and shortest-arc `glm::slerp` for rotations. |
| **`BlendPosesByInt`** | Blending | Variable-input selector (`Pose 0 .. Pose N-1`) that smoothly cross-fades to the newly selected `Active Index` over `BlendTime` seconds. |
| **`BlendSpace1D`** | Blending | Blends up to 4 clips along a 1D parameter axis (e.g., `Speed`), advancing a shared normalized phase and locking follower clip footsteps onto the dominant clip's `Sync:` markers (`SyncedClipTime`). |
| **`BlendSpace2D`** | Blending | 2D freeform Cartesian blend space (up to 8 clips at `(X, Y)` coordinates) using **Johansen 2009 gradient band interpolation** with leader-follower `Sync:` marker footstep locking. |
| **`LayeredBlend`** | Blending | Per-bone branch mask (`BuildBranchMask`): applies the `Blend` pose only to descendants of `Bone` (with optional `BlendDepth` feathering) while keeping the rest of the body on `Base` (e.g., upper-body aiming while running). |
| **`ApplyAdditive`** | Blending | Computes `Additive - Reference` (or rest pose) scaled by `Alpha` and applies it on top of `Base` (`ApplyAdditivePose`). |
| **`Slot`** | Montages | Blends active one-shot **Montages** (`PlayMontage(clip, slotName, blendIn, blendOut, rate)`) over the incoming `Source` pose using smooth fade-in/hold/fade-out envelopes. |
| **`GetParameter`** | Inputs | Outputs the current value of a named graph parameter (`SetFloat`, `SetBool`). |
| **`StateMachine`** | State Machines | Hierarchical state machine containing nested state subgraphs and directed `NodeTransition` rules (`Equal`, `NotEqual`, `Greater`, `Less`, `IsTrue`, `IsFalse`), cross-fading between states with linear or cubic smoothstep (`EaseInOut`) curves. |
| **`Output`** | Sinks | Terminal output node receiving the final `Pose`. |

### C. Sequencer Timeline & 11 Track Types ([`Timeline.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/Timeline.h))
A `.ntimeline` asset played by `TimelinePlayerComponent` (or scrubbed in `TimelinePanel`) supports `Constant`, `Linear`, and monotone `Smooth` (Catmull-Rom / cubic Hermite) keyframes across **11 track types**:
1. **`Translation`**, 2. **`Rotation`**, 3. **`Scale`** (`TransformComponent`)
4. **`LightColor`**, 5. **`LightIntensity`** (`LightComponent`)
6. **`CameraFOV`** (`CameraComponent`)
7. **`Event`** (fires `OnAnimationEvent(name)` on C# scripts)
8. **`CameraCut`** (switches the active primary camera to the named camera entity during Play)
9. **`MorphWeight`** (drives `MeshComponent::SequencerMorphWeights[targetName]`)
10. **`SubTimeline`** (nests another `.ntimeline` asset with start offset, speed, and loop controls)
11. **`MaterialParameter`** (animates 1–4 float material properties such as `AlbedoColor`, `EmissiveColor`, `Roughness`, or `UVScale` on `MaterialComponent::SequencerParameters`)

### D. Generic Domain-Agnostic NodeGraph Compiler & Runtime (`NoxCore/NodeGraph`)
- [`GraphCompiler.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/GraphCompiler.cpp) validates pin types, resolves default properties, detects cycles, and runs **Kahn's topological sort** (with deterministic min-heap node-ID tie-breaking) to compile an authored `NodeGraph` (and any nested state-machine subgraphs) into a flat, cache-friendly `CompiledGraph`.
- [`GraphEvalContext.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/GraphEvalContext.cpp) evaluates nodes linearly from `0 .. Nodes.size() - 1` using flat output value slots (`m_OutputValues`) and per-node persistent state (`m_NodeState`).

---

## 2. File-by-File Reference: `NoxCore/src/NoxCore/Animation/` (12 Files)

| File | Plain-English Summary & Key Types |
| :--- | :--- |
| [`Skeleton.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/Skeleton.h) | Defines `SkeletonNode`, `SkeletonSkin`, and `Skeleton : public Asset` (joint hierarchy, rest transforms, inverse bind matrices). |
| [`AnimationSequence.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/AnimationSequence.h) | Defines `AnimationInterpolation`, `AnimationChannel`, `MorphCurve`, `AnimationNotify` (`IsSyncMarker`, `SyncMarkerName`), and `AnimationSequence : public Asset`. |
| [`Animator.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/Animator.h) & [`Animator.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/Animator.cpp) | Evaluates single-clip keyframe channels, morph curves, notify crossings (`CollectNotifies`), and Sync Marker phase matching (`FindSyncPhase`, `TimeAtSyncPhase`, `SyncedClipTime`). |
| [`AnimPose.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/AnimPose.h) & [`AnimPose.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/AnimPose.cpp) | Defines `AnimPose` (local node transforms + morph weights) and implements `SampleClipPose`, `BlendPoses`, `BlendPosesMasked`, `BuildBranchMask`, `ApplyAdditivePose`, `BlendPosesWeighted`, and `PoseToSkinningMatrices`. |
| [`AnimationGraphInstance.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/AnimationGraphInstance.h) & [`AnimationGraphInstance.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/AnimationGraphInstance.cpp) | Per-entity Animation Graph evaluator (`Sync`, `Evaluate`, `SetFloat`/`SetBool`, `PlayMontage`/`StopMontage`, and loop-aware Root Motion extraction `ExtractRootMotion`). |
| [`AnimationGraphNodes.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/AnimationGraphNodes.h) & [`AnimationGraphNodes.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/AnimationGraphNodes.cpp) | Implements and registers all **11 Animation Graph node types** in `NodeTypeRegistry` (`Clip`, `Blend2D`, `BlendPosesByInt`, `BlendSpace1D`, `BlendSpace2D`, `LayeredBlend`, `ApplyAdditive`, `Slot`, `GetParameter`, `StateMachine`, `Output`). |
| [`Timeline.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/Timeline.h) & [`Timeline.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Animation/Timeline.cpp) | Defines `Timeline : public Asset`, `TimelineTrack`, `TimelineKey`, `TimelineProperty` (11 track types), and `TimelineEvaluation` (`Evaluate`, `CollectEvents`, `CameraCutAt`, `SubTimelineAt`). |

---

## 3. File-by-File Reference: `NoxCore/src/NoxCore/NodeGraph/` (10 Files)

| File | Plain-English Summary & Key Types |
| :--- | :--- |
| [`NodeGraphTypes.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/NodeGraphTypes.h) | Core variant `NodeGraphValue` (`bool`, `int32_t`, `float`, `glm::vec2`, `uint64_t` AssetHandle, `std::string`) and `NodePinDesc`. |
| [`NodeType.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/NodeType.h) & [`NodeType.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/NodeType.cpp) | Defines `NodeTypeDesc` (fixed & variable-arity pins, property defaults/ranges/asset hints, `Evaluate` callback) and `NodeTypeRegistry`. |
| [`NodeGraph.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/NodeGraph.h) | Authored graph structures: `NodeGraph`, `GraphNode`, `GraphLink`, `GraphParameter`, `NodeSubGraph`, `NodeTransition`, and `TransitionRule`. |
| [`CompiledGraph.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/CompiledGraph.h) | Immutable runtime structures produced by `GraphCompiler`: `CompiledGraph`, `CompiledNode`, `CompiledPin`, and `CompiledSubGraph`. |
| [`GraphCompiler.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/GraphCompiler.h) & [`GraphCompiler.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/GraphCompiler.cpp) | Implements `GraphCompiler::Compile`: link/type validation, cycle detection, Kahn's topological sort, and recursive subgraph compilation. |
| [`GraphEvalContext.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/GraphEvalContext.h) & [`GraphEvalContext.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/GraphEvalContext.cpp) | Per-instance evaluation state (`GraphEvalContext`: `GetInput<T>`, `SetOutput<T>`, `GetState<T>`, `GetProperty<T>`) and `EvaluateGraph()`. |
| [`NodeGraphAsset.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/NodeGraph/NodeGraphAsset.h) | Engine asset wrapper (`NodeGraphAsset : public Asset`) holding `NodeGraph Source`, `CompiledGraph Compiled`, and `CompileVersion` for live hot-reloading. |

---

## Jump / Fall (November 2026)

- **At End of State** transitions (Unity's Has Exit Time, Unreal's automatic rule): a state machine transition can wait for its state's animation to end -- Jump Start -> Jump Loop, Jump Land -> Locomotion. `CharacterMovement.cs` sends `VerticalSpeed` next to `Speed` and `IsGrounded`.

---

## IK Nodes (November 2026)

Category **IK** in the animation graph (Unreal's Two Bone IK / Look At / foot placement, Godot's SkeletonIK3D), working in the skeleton's model space on the pose coming in:

| Node | Properties | What it does |
| :--- | :--- | :--- |
| **TwoBoneIK** | End Bone, Target, Pole, Alpha | The end bone's parent and grandparent (elbow / knee, shoulder / hip) turn so it reaches the target (analytic, law of cosines); the bend stays in its plane, turned towards Pole when set; the end bone keeps its orientation |
| **LookAt** | Bone, Target, Axis (0 = automatic), Max Angle, Alpha | The bone's forward axis turns towards the target, at most Max Angle; chain several (spine 0.3, head 1.0) |
| **FootPlacement** | Left / Right Foot, Pelvis, Trace Up / Down, Max Pelvis Drop, Speed, Align To Ground, Max Align Angle, Alpha | A ray down per foot (cast by the scene after the parallel animation update, used the next frame); feet onto what it hit, the pelvis lowered for the lower foot, feet tilted to the slope; eases at Speed. Play / Simulate only |

Targets come from scripts by name: `animator.SetIKTarget("RightHand", position, weight)` / `ClearIKTarget(name)` (world positions, converted to the skeleton's space each frame). With root motion the nodes measure the pose with the root bone where it will be drawn. Test script: `IKTargetTest.cs`. Step-by-step guide: the manual's **Character Setup: Jump, Root Motion & IK**.
