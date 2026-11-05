# 3D Jolt Physics & 2D Box2D Physics (`NoxCore/Physics` — 13 Files)

Noxiouse integrates **Jolt Physics** (compiled with **`JPH_DOUBLE_PRECISION=ON`** for 64-bit Large World Coordinates) for 3D simulation and **Box2D v3** for 2D simulation in `NoxCore/src/NoxCore/Physics/`.

---

## 1. Plain-English Tour of the Physics Architecture

### A. Fixed 60 Hz Timestep, Pose Interpolation & Unit Conversion Boundary
- **Fixed 60 Hz Step with Accumulator (`Scene.cpp`)**:
  - During Play and Simulation, physics steps at a deterministic `FixedTimeStep = 1.0f / 60.0f` (`MaxSubSteps = 4`).
  - Before each step, `ScriptEngine::OnFixedUpdateEntity` runs C# `OnFixedUpdate(fixedDt)` callbacks.
  - After all substeps, `PhysicsPoseComponent` interpolates between `PrevPosition`/`CurrPosition` (`glm::mix`) and `PrevRotation`/`CurrRotation` (`glm::slerp`) by `alpha = accumulator / FixedTimeStep`, giving butter-smooth rendering at any monitor refresh rate.
- **Automatic World-Units $\leftrightarrow$ Meters Boundary**:
  - Jolt and Box2D simulate internally in **SI meters (`m`, `m/s`, `N`, `N·s`)** for solver stability (`-9.81 m/s²` gravity).
  - `JoltPhysics3DScene` converts all positions, collider dimensions, velocities, forces, impulses, and raycast distances at the boundary using `WorldUnits::ToMeters()` and `WorldUnits::FromMeters()`.

### B. 3D Jolt Physics Features (`JoltPhysics3DScene`)
- **Double-Precision Large World Coordinates (`JPH::RVec3`)**: Jolt bodies and characters store world positions in 64-bit double-precision meters (`JPH::RVec3`), matching Noxiouse's `glm::dvec3` transforms across planetary-scale worlds.
- **Colliders & Compound Hierarchies**:
  - Supports **Box**, **Sphere**, **Capsule** (`Y`/`X`/`Z` axis), **Cylinder** (with adaptive convex radius), **Infinite Half-Space Plane**, and **Cooked Mesh Colliders** (`ConvexHullShape` for dynamic bodies, `MeshShape` for static/kinematic geometry, cached by `(meshHandle, scale, type)` with automatic invalidation when a runtime `DynamicMesh` changes).
  - **Automatic Compound Colliders**: Any child entity with a collider component (and no `RigidBody3DComponent` of its own) is automatically folded into its ancestor rigid body's `StaticCompoundShapeSettings` with its relative transform!
- **6-DOF & Specialized Physics Joints (`PhysicsJoint3DComponent`)**:
  - Supports `Fixed`, `Point`, `Distance`, `Hinge`, `Slider`, `Cone`, and full `SixDOF` constraints (`JPH::SixDOFConstraintSettings`) with per-axis Free/Locked/Limited modes, linear/angular limits, spring frequency/damping, and velocity/position motors.
- **Virtual Character Controller (`CharacterController3DComponent`)**:
  - Powered by `JPH::CharacterVirtual` with `ExtendedUpdate`: supports walkable slope limits (`MaxSlopeAngle`), automatic stair step-up (`StepUpHeight`), downhill ramp stick-to-floor (`StickToFloorStepDown`), moving platform velocity inheritance, and pushing dynamic rigid bodies.
- **Custom Rolling Resistance (`JoltRollingResistance`)**:
  - Implements a custom `JPH::ContactListener` that computes an opposing torque $\tau = -\mu_r \cdot F_n \cdot \hat{\omega}$ from `PhysicsMaterial3D::RollingFriction` during contacts and applies it at the start of the next physics step, ensuring spheres and cylinders naturally roll to a stop on flat ground.
- **Shared Worker Thread Pool (`NoxJoltJobSystem`)**:
  - Implements `JPH::JobSystemWithBarrier` directly on top of Noxiouse's `JobSystem` Frame Workers (`RunTasks`), so Jolt never spawns duplicate background threads.

---

## 2. File-by-File Reference: `NoxCore/src/NoxCore/Physics/` (13 Files)

| File | Plain-English Summary & Key Types |
| :--- | :--- |
| [`Physics2D.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Physics2D.h) | Box2D v3 conversion helpers (`Rigidbody2DTypeToBox2DBody`, `Rigidbody2DTypeFromBox2DBody`) mapping `Static`, `Dynamic`, and `Kinematic` bodies. |
| [`Physics3DTypes.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Physics3DTypes.h) | Public Jolt-free 3D physics types: `RigidBody3DType`, `ColliderAxis3D`, `MeshCollider3DType`, `PhysicsJoint3DType` (7 joint types), `JointAxisMode` (`Free`, `Locked`, `Limited`), `JointAxisDriveMode`, `PhysicsMaterial3D`, `PhysicsFilter3D`, `PhysicsBodyHandle3D`, `RaycastHit3D` (`glm::dvec3 Position`), `OverlapHit3D`, and `Physics3DStats`. |
| [`Physics3DScene.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Physics3DScene.h) | Pure abstract interface (`Physics3DScene`) for a per-`Scene` 3D physics world: `Step`, `SyncTransformsFromScene`, `WriteTransformsToScene`, `Raycast`, `RaycastAll`, `OverlapSphere`, `OverlapBox`, body velocity/force/impulse control, and `CharacterVirtual` commands. |
| [`Physics3D.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Physics3D.h) | Convenience umbrella header including `Physics3DTypes.h`, `Physics3DScene.h`, and `PhysicsEngine.h`. |
| [`PhysicsEngine.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/PhysicsEngine.h) & [`PhysicsEngine.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/PhysicsEngine.cpp) | Process-wide Jolt lifecycle manager (`PhysicsEngine::Init()`, `Shutdown()`, `CreateScene()`, `ClearShapeCache()`). Initializes `JPH::Factory`, registers Jolt types, and owns the shared `NoxJoltJobSystem`. |
| [`Jolt/JoltUtils.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Jolt/JoltUtils.h) | Math conversion helpers between GLM (`glm::vec3`, `glm::dvec3`, `glm::quat`) and Jolt (`JPH::Vec3`, `JPH::RVec3`, `JPH::Quat`), including double-precision LWC meter conversions (`ToJoltPosition`, `FromJoltPosition`). |
| [`Jolt/JoltRollingResistance.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Jolt/JoltRollingResistance.h) & [`JoltRollingResistance.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Jolt/JoltRollingResistance.cpp) | Implements `RollingResistanceCollector` (`JPH::ContactListener` accumulating contact normal impulses weighted by `RollingFriction`) and `ApplyRollingResistance()`. |
| [`Jolt/NoxJoltJobSystem.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Jolt/NoxJoltJobSystem.h) & [`NoxJoltJobSystem.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Jolt/NoxJoltJobSystem.cpp) | Bridges Jolt's `JPH::JobSystemWithBarrier` (`MaxPhysicsJobs = 2048`, `MaxPhysicsBarriers = 16`) onto `Nox::JobSystem::Get().RunTasks()`. |
| [`Jolt/JoltPhysics3DScene.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Jolt/JoltPhysics3DScene.h) & [`JoltPhysics3DScene.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Physics/Jolt/JoltPhysics3DScene.cpp) | Full `Physics3DScene` implementation over `JPH::PhysicsSystem`. Implements broadphase layers (`NonMoving`, `Moving`), `EntityGroupFilter` (layer masks + `IgnoreEntities`), compound shape folding, mesh collider cooking & caching, 7 joint types, `CharacterVirtual` extended updates, raycasts/shapecasts, and `PhysicsPoseComponent` interpolation state updates. |
