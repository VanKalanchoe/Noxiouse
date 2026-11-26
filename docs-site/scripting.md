# C# Scripting Host (`.NET 10` / Coral `320c90d`) & `NoxScriptCore` API (`35` Files)

Noxiouse features a first-class **C# scripting runtime** powered by **.NET 10 (`net10.0`)** and **Coral `320c90d`** (`NoxCore/src/NoxCore/Scripting/` + `NoxScriptCore/`). It supports **multiple scripts per entity**, **atomic stateful hot reload during Play Mode**, **rich Inspector reflection (`[Expose]` on fields, `get`/`set` properties, `enum` dropdowns, and `System.Action` events)**, **live Play-mode Inspector sync & editing**, **64-bit Large World Coordinate `Vector3d` positions**, **multi-threaded C# jobs (`Nox.Jobs`)**, **runtime procedural meshes (`MeshBuilder`)**, and **native C++ internal calls**.

---

## 1. Plain-English Tour of the Scripting Architecture

### A. Assembly Staging & Atomic Stateful Hot Reload (`ScriptEngine` + `DotNetBackend`)
- **Zero File-Lock Shadow Copying**: In the editor, `ScriptEngine` copies `.dll`, `.pdb`, `.deps.json`, and `.runtimeconfig.json` into `Intermediate/Scripts/Generation-<N>/` before loading so `dotnet build` can recompile C# scripts at any time—even while the game is playing!
- **Atomic Stateful Hot Reload (`ScriptEngine::ReloadAssembly`)**:
  1. Waits for any running background `Nox.Jobs` tasks to finish (`WaitForJobs(2000)`).
  2. Snapshots the live value of every `[Expose]` field/property on every active script instance (`FieldSnapshot`).
  3. Loads and validates the **new** `Coral::AssemblyLoadContext` first (Coral `320c90d` isolates reflection caches per context), checks `kScriptApiVersion = 23`, and reflects all `EntityBehaviour` types, fields, properties, enums, and events. If anything fails, only the new context is unloaded and the currently running scripts keep executing untouched.
  4. On success, swaps in the new context, unloads the old context, recreates script instances, restores live `[Expose]` values from the snapshot, re-binds all `EventBindings`, and calls `OnAfterReload()`.

### B. `EntityBehaviour` Lifecycle, Properties, Enums & Events
Every gameplay script inherits from `Nox.EntityBehaviour`:

```csharp
using System;
using Nox;

public enum MovementMode { Walk = 0, Sprint = 2, Fly = 5 }

public sealed class PlayerController : EntityBehaviour
{
    [Expose(Tooltip = "Inspector enum dropdown (supports explicit values & YAML persistence)")]
    public MovementMode Mode = MovementMode.Walk;

    private float _moveSpeed = WorldUnits.FromMeters(5.0f);
    [Expose(Unit = "m/s", Min = 0f, Max = 20f, Tooltip = "Validated C# property")]
    public float MoveSpeed
    {
        get => _moveSpeed;
        set => _moveSpeed = Math.Clamp(value, 0f, WorldUnits.FromMeters(20f));
    }

    // Getter-only (or private set) properties are automatically ReadOnly and update live in Play Mode:
    [Expose] public bool IsGrounded => GetComponent<CharacterControllerComponent>().IsGrounded;

    [Expose, DisallowSelf]
    public Entity FollowTarget;

    // UnityEvent-style Inspector wiring to any target entity's public void Method():
    [Expose(Tooltip = "Wired in the Inspector via EventBindings")]
    public event Action? OnJump;

    protected override void OnCreate() { }
    protected override void OnUpdate(float dt) { }
    protected override void OnFixedUpdate(float fixedDt) { } // Only called at 60Hz if overridden!
    protected override void OnLateUpdate(float dt) { }       // Called after physics & animations!
    protected override void OnAnimationEvent(string name) { }
    protected override void OnAfterReload() { }
    protected override void OnDestroy() { }
}
```

:::{note}
**Zero-Overhead Optional Callbacks**: `DotNetBackend` reflects `EntityBehaviour.GetCallbackMask()` once per class and caches a bitmask (`1 = OnFixedUpdate`, `2 = OnLateUpdate`). If your script does not override `OnFixedUpdate` or `OnLateUpdate`, the C++ engine skips the managed transition completely!
:::

---

## 2. File-by-File Reference: C++ Scripting Host (`NoxCore/src/NoxCore/Scripting/` — 7 Files)

| File | Plain-English Summary & Key Types |
| :--- | :--- |
| [`ScriptTypes.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Scripting/ScriptTypes.h) | Defines `ScriptLanguage`, `ScriptCallback` (`Create`, `Update`, `Destroy`, `AfterReload`, `FixedUpdate`, `LateUpdate`), `ScriptFieldType`, `ScriptInstanceHandle` (with reload generation counter), `ScriptValue` variant, and `ScriptFieldInfo` (`Unit`, `Tooltip`, `Min`, `Max`, `ReadOnly`, `DisallowSelf`). |
| [`IScriptBackend.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Scripting/IScriptBackend.h) | Pure abstract interface decoupling `ScriptEngine` from the underlying .NET / Coral hosting implementation. |
| [`ScriptEngine.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Scripting/ScriptEngine.h) & [`ScriptEngine.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Scripting/ScriptEngine.cpp) | Static scripting facade: assembly discovery (`FindAssemblies`), shadow-copy staging (`StageAssemblies`), stateful hot reload (`ReloadAssembly`), entity instance creation/destruction, and callback dispatch. |
| [`DotNet/CoralCompat.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Scripting/DotNet/CoralCompat.h) | Declares explicit `Coral::Attribute::GetFieldValue<std::string>` and `<bool>` specializations matching `Coral.Native`. |
| [`DotNet/DotNetBackend.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Scripting/DotNet/DotNetBackend.h) & [`DotNetBackend.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Scripting/DotNet/DotNetBackend.cpp) | Hosts `.NET 10` via `Coral::HostInstance` (`320c90d`, supporting both system .NET and packaged self-contained `dotnet/` folders), verifies `kScriptApiVersion = 23`, implements atomic hot-reload, reflects `[Expose]` fields, properties, enums, and `System.Action` events, and registers all native C++ internal calls. |

---

## 3. File-by-File Reference: Managed C# API (`NoxScriptCore/` — 28 Files)

| File | Plain-English Summary & Public API |
| :--- | :--- |
| [`Nox.ScriptCore.csproj`](file:///E:/dev/noxiouse/NoxScriptCore/Nox.ScriptCore.csproj) | .NET 10 (`net10.0`) project file with `AllowUnsafeBlocks` enabled for zero-overhead unmanaged `delegate*` pointers. |
| [`Source/ScriptApi.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/ScriptApi.cs) | Defines `ScriptApi.Version = 23`, `GetEnumMembers`, `SubscribeEvent` (native `Delegate.CreateDelegate` event wiring), and `WaitForDebugger(timeoutMs)`. |
| [`Source/InternalCalls.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/InternalCalls.cs) | Declares all **69 unmanaged `delegate*` function pointers** bound by `DotNetBackend::RegisterInternalCalls` plus `NativeString` marshaling. |
| [`Source/ExposeAttribute.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/ExposeAttribute.cs) | `[Expose]` attribute with optional `Unit` (`"cm"`, `"m"`, `"cm/s"`, `"m/s"`, `"km/h"`), `Tooltip`, `Min`, `Max`, and `ReadOnly` parameters. |
| [`Source/DisallowSelfAttribute.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/DisallowSelfAttribute.cs) | `[DisallowSelf]` attribute preventing an `Entity` reference field from being assigned to `Self` in the Inspector. |
| [`Source/EntityBehaviour.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/EntityBehaviour.cs) | Base class for all user scripts (`Self`, `EntityID`, `HasComponent<T>`, `GetComponent<T>`, `TryGetComponent<T>`, `FindChild`, and lifecycle virtual methods). |
| [`Source/Entity.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Entity.cs) | Lightweight `readonly struct Entity(ulong ID)`: component queries, `Entity.FindByName`, `FindChild`, `SetMesh`, `SetMeshCollider`, `GetMetadata`/`SetMetadata`, and glTF Material Variant switching (`SetMaterialVariant`). |
| [`Source/Component.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Component.cs) | Abstract base class `Component` and `ComponentType` enum (`Transform`, `Animator`, `CharacterController`, `RigidBody3D`, `TimelinePlayer`, `ParticleEmitter`, `Mesh`). |
| [`Source/Transform.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Transform.cs) | Sequential 48-byte struct `{ Vector3d Position; Vector3 Rotation; Vector3 Scale; }`. |
| [`Source/TransformComponent.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/TransformComponent.cs) | Reads/writes `Local`, `World`, `LocalPosition` (`Vector3d`), `LocalRotation`, `LocalScale`, `WorldPosition` (`Vector3d`), `WorldRotation`, and `WorldScale`. |
| [`Source/RigidBody3DComponent.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/RigidBody3DComponent.cs) | Controls a 3D Jolt rigid body: `LinearVelocity`, `AddForce(Vector3)`, and `AddImpulse(Vector3)`. |
| [`Source/CharacterControllerComponent.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/CharacterControllerComponent.cs) | Controls a 3D Jolt `CharacterVirtual`: `SetMoveVelocity(Vector3)`, `Jump(float speed)`, `IsGrounded`, and `Velocity`. |
| [`Source/AnimatorComponent.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/AnimatorComponent.cs) | Controls Animation Graph parameters (`SetFloat`, `GetFloat`, `SetBool`, `GetBool`) and Montage slots (`PlayMontage`, `StopMontage`, `IsMontagePlaying`). |
| [`Source/MeshComponent.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/MeshComponent.cs) | Controls per-entity morph target weights (`SetMorphWeight`, `GetMorphWeight`, `ClearMorphWeight`, `ClearMorphWeights`) and the entity's own material graph parameters (`SetScalarParameter`, `SetVectorParameter`, `SetTextureParameter`, `GetScalarParameter`, `GetVectorParameter`, `TryGetVectorParameter`, `ClearParameter`, `ClearParameters`). |
| [`Source/ParticleEmitterComponent.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/ParticleEmitterComponent.cs) | Controls a particle emitter: `Play(restart)`, `Stop()`, `Emit(int count)`, `IsEmitting`, and `ParticleCount`. |
| [`Source/VfxComponent.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/VfxComponent.cs) | Controls a unified multi-layer `.nvfx` system: `Play(restart)`, `Stop()`, `Restart()`, `IsPlaying`, `Scale`, `SetColorTint`, and named parameter overrides (`SetFloat`, `SetVector3`, `SetColor`). |
| [`Source/TimelinePlayerComponent.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/TimelinePlayerComponent.cs) | Controls a Sequencer timeline player: `Play(fromStart)`, `Stop()`, `IsPlaying`, `Time`, and `Speed`. |
| [`Source/AudioSourceComponent.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/AudioSourceComponent.cs) | Controls an Audio Source: `Play()`, `Stop(fadeOut)`, `IsPlaying`, `Volume`, `Pitch`, `Loop`, `SetSound(path)` (a clip or an audio graph), and an audio graph's parameters `SetFloat` / `SetInt` / `SetBool(name, value)` and `Trigger(name)` (API 17). |
| [`Source/Audio.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Audio.cs) | `Audio.PlayOneShot(path, Vector3d position, volume, bus)`, `PlayOneShot2D(path, volume, bus)`, `SetBusVolume(bus, volume)`, `GetBusVolume(bus)` (buses of the project mixer, by name, on top of the mix), `SetSnapshot(name, blendSeconds)` (blend to a mixer snapshot; "Default" goes back), `Headphones` (HRTF or panning for 3D sounds: a game's options); sound paths (clips and audio graphs) with an optional extension (API 19). |
| [`Source/Physics.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Physics.cs) | Defines `RaycastHit` (`Entity`, `Vector3d Point`, `Vector3 Normal`, `float Distance`) and `Physics.Raycast(Vector3d origin, Vector3 direction, float maxDistance, out RaycastHit hit)`. |
| [`Source/Mesh.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Mesh.cs) | Defines `Mesh : IDisposable` (`SetData`, `Clear`, `SetColor`, `SetMaterial`) and thread-safe `MeshBuilder` (`AddVertex`, `AddTriangle`, `AddQuad`) for procedural geometry generation on worker threads. |
| [`Source/Jobs.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Jobs.cs) | Multi-core C# job API (`Jobs.Run`, `Jobs.ParallelFor`, `JobHandle`, `JobHandle<T>.TryGetResult`, `WaitForAll`) safe across hot reloads. |
| [`Source/Scene.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Scene.cs) | Spawning (`Scene.Instantiate`, `CreateEntity`, `Destroy`), additive Level Streaming (`LoadLevel`, `UnloadLevel`, `GetLevelState`), and World Partition queries (`IsWorldStreamingCompleted`, `GetWorldStreamingStats`, `WorldStreamingStats`). |
| [`Source/Diagnostics.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Diagnostics.cs) | `Diagnostics.Memory` returning `MemoryStats` (`ProcessBytes`, `GpuBytes`, `GpuProcessBytes`, `LoadedAssets`, `LoadingAssets`). |
| [`Source/Input.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Input.cs) | Defines `KeyCode`, `MouseButton`, `Input.IsKeyDown`, `IsMouseButtonDown`, `MouseDelta`, `ScrollDelta`, and `CursorLocked`. |
| [`Source/WorldUnits.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/WorldUnits.cs) | `WorldUnits.PerMeter`, `FromMeters`, `ToMeters`, `FromCentimeters`, and `ToCentimeters`. |
| [`Source/Time.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Time.cs) | Defines `Time.FixedDeltaTime = 1.0f / 60.0f`. |
| [`Source/Log.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Log.cs) | `Log.Info(string)` logging to the engine console with a `[C#]` tag. |
| [`Source/Vector2.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Vector2.cs) | Single-precision 2D vector (`float X, Y`). |
| [`Source/Vector3.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Vector3.cs) | Single-precision 3D vector (`float X, Y, Z`) for directions, velocities, forces, Euler rotations, and scales. |
| [`Source/Vector3d.cs`](file:///E:/dev/noxiouse/NoxScriptCore/Source/Vector3d.cs) | **64-bit double-precision 3D position vector (`double X, Y, Z`)** for Large World Coordinates, supporting mixed `Vector3d +/- Vector3` arithmetic, `Length()`, and `Distance(a, b)`. |
