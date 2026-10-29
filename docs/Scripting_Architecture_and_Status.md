# Noxiouse scripting architecture and implementation status

Last updated: 2026-09-22

## Goals

Noxiouse scripting is runtime-neutral. Coral/CoreCLR is the first backend,
not the public scripting architecture. The design must permit a Lua backend or
another C# runtime later without changing Scene, serialization, or editor
concepts.

The dependency direction is:

```text
Scene / Editor / Serializer
          |
          v
 ScriptEngine + IScriptBackend
          |
     +----+----+
     |         |
 DotNet      future Lua
     |
   Coral / CoreCLR
```

No Coral type may be stored in an ECS component or exposed to scene/editor
code. Backend objects are represented by generation-checked opaque handles.

## Dependency integration

Coral is downloaded by CMake `FetchContent` into the build tree. It is not a
vendored working copy and does not need a Git submodule checkout.

- Declaration: `NoxCore/CMake/Dependencies.cmake`
- Repository: `https://github.com/StudioCherno/Coral.git`
- Pinned commit: `d53b2685725f7535bc4d1deaa8a22bf16d112fe2`
- Coral examples and tests are disabled.
- `NoxCore` links `Coral.Native`.
- `Coral.Managed.dll`, `.deps.json`, and `.runtimeconfig.json` are copied next
  to `NoxEditor` after building.
- Set `FETCHCONTENT_SOURCE_DIR_CORAL` to a local Coral checkout for offline
  development.

The pinned Coral revision has two compatibility defects handled
deterministically during configure:

1. Its .NET check excludes exactly runtime 9.0.0 despite requiring 9.0+.
2. Its Windows diagnostic sends `wchar_t*` to `std::cerr`, rejected by newer
   MSVC. Nox patches this fetched build-tree copy to use `std::wcerr`.

Remove these patches when the pinned Coral revision contains upstream fixes.

Requirements are CMake, Git, and the .NET 9 SDK/runtime. A clean configure
downloads Coral automatically. Release packaging must later decide between a
framework-dependent .NET runtime requirement and shipping a self-contained
runtime.

## Current source layout

```text
NoxCore/src/NoxCore/Scripting/
  ScriptTypes.h              common language-neutral types (values, field metadata, callbacks)
  IScriptBackend.h           backend contract
  ScriptEngine.h/.cpp        scene-facing facade, instance registry, finding and staging the assemblies
  DotNet/
    DotNetBackend.h/.cpp     Coral host, ALC, reflection, internal calls, API version check

NoxEditor/src/ScriptBuilder.h/.cpp   dotnet build of the project's scripts, build on save, reload

NoxScriptCore/                       the managed engine API (Nox.ScriptCore.dll)
  Source/
    EntityBehaviour.cs  Entity.cs  Component.cs  TransformComponent.cs  Transform.cs
    RigidBody3DComponent.cs  CharacterControllerComponent.cs  AnimatorComponent.cs
    ParticleEmitterComponent.cs  TimelinePlayerComponent.cs  Physics.cs  Mesh.cs  Jobs.cs
    Scene.cs  Input.cs  Log.cs  Time.cs  WorldUnits.cs  Vector2.cs  Vector3.cs
    ExposeAttribute.cs  DisallowSelfAttribute.cs  ScriptApi.cs  InternalCalls.cs

<Project>/Assets/Scripts/            a project's game scripts (Facerun: Facerun.csproj, Source/*.cs)
```

## Runtime ownership and lifetime

```text
NoxEditor process
  Coral::HostInstance                     process lifetime
    NoxGameScripts AssemblyLoadContext    one assembly generation
      Nox.ScriptCore.dll                  stable managed engine API
      Facerun.dll                         game behaviours
      managed behaviour instances         play-scene lifetime
```

CoreCLR is initialized once. Game assemblies live in an unloadable Coral
Assembly Load Context. A reload destroys all managed objects and cached type
pointers before unloading that context. Every instance handle includes an ALC
generation so a stale object cannot be called after reload.

The current `ScriptEngine` is a compatibility facade for the existing scene
hooks. The actual runtime is behind `IScriptBackend`. A future `LuaBackend`
implements that interface and maps callbacks to Lua functions.

## Engine API boundary

Managed code never receives an EnTT entity or native pointer. It receives the
stable 64-bit entity UUID. Internal calls resolve the UUID against the active
play scene and validate components.

Implemented calls:

- `Log.Info`
- `Input.IsKeyDown` (every letter, digit, F key, arrow and modifier as `KeyCode`), `Input.IsMouseButtonDown`
- `Input.MouseDelta` (pixels moved during the last frame, also with the cursor locked), `Input.ScrollDelta` (wheel notches),
  `Input.CursorLocked` (SDL relative mouse mode: hidden and held in the window, for mouse look; Stop releases it). In the editor
  the game has the keys while the viewport is the target and the mouse while the cursor is over it. Facerun's `FlyCamera` uses
  them: the editor camera's fly mode in Play (right mouse to look, WASD, E / Q, Shift, wheel for speed). Script API version 4.
- `Scene.IsWorldStreamingCompleted(position, range, WorldCellState)`: a world partition's cells around a point have reached a stage
  (Loaded: built and their assets in, not in the world; Activated: in the world), Unreal's IsStreamingCompleted. Script API 5.
- `Scene.GetWorldStreamingStats()` (a world partition's cells by state and its bounds) and `Diagnostics.Memory` (process private
  bytes, VRAM in use, assets loaded and loading): Facerun's `StreamingWorldTest` fly-through logs them. Script API 6.
- Generic `GetComponent<T>()`, `HasComponent<T>()`, and `TryGetComponent<T>()`
  access from both `EntityBehaviour` and `Entity`
- `TransformComponent` explicit `Local`/`World` state and
  `LocalPosition`/`WorldPosition`, `LocalRotation`/`WorldRotation`, and
  `LocalScale`/`WorldScale` properties
- Relative child lookup with `FindChild("Node")` or `FindChild("Body/Door")`
- Exposed entity references with `[Expose] public Entity Target;`
  - The Script inspector discovers exposed `Entity` fields through Coral reflection.
  - Regular scene entities are saved by UUID.
  - Imported model nodes are saved by model-instance UUID plus model-node index, so renaming a node does not break the reference.
  - References are injected into the managed object before `OnCreate` and are restored after assembly reload.
- Reflected `[Expose]` value fields. The inspector is generated from the loaded
  C# type; no matching C++ property UI has to be written for each script.

Supported exposed C# types are `bool`, `int`, `uint`, `long`, `ulong`,
`float`, `double`, `string`, `Vector3`, and `Entity`. A C# field initializer is
the inspector default. Nox stores only values changed in the inspector as
scene overrides, then applies those values to each managed instance before
`OnCreate`. Unsupported exposed types produce a warning.

Example:

```csharp
public sealed class HouseController : EntityBehaviour
{
    [Expose] public Entity Door;

    protected override void OnUpdate(float deltaTime)
    {
        if (!Door.IsValid)
            return;

            TransformComponent doorTransform = Door.GetComponent<TransformComponent>();
            Transform transform = doorTransform.Local;
            transform.Rotation.Y += deltaTime;
            doorTransform.Local = transform;
    }
}
```

Assign `Door` from the Script component using its entity combo, or drag an entity from the Scene Hierarchy onto the field.

Entity fields may opt out of self-reference:

```csharp
[Expose, DisallowSelf] public Entity Target;
```

The inspector disables the behaviour's own entity for that field, rejects a
self drag/drop, and displays existing invalid self-references as an error.
Scripts must still validate semantic requirements at runtime; `CameraFollow`
does so before reading or changing transforms.

Ordinary values work the same way:

```csharp
public sealed class DoorController : EntityBehaviour
{
    [Expose] public Entity Door;
    [Expose] public float OpenSpeed = 2.0f;
    [Expose] public bool Locked;
    [Expose] public Vector3 OpenRotation = new(0.0f, 90.0f, 0.0f);

    protected override void OnUpdate(float deltaTime)
    {
        if (!Locked && Door.IsValid)
        {
            Transform transform = Door.LocalTransform;
            transform.Rotation.Y += OpenSpeed * deltaTime;
            Door.LocalTransform = transform;
        }
    }
}
```

Coral binds native calls to private static C# function-pointer fields
(`delegate*`), not to `[MethodImpl(InternalCall)]` extern methods. Managed
wrappers own string conversion and expose ordinary safe methods to game code.

`GetComponent<T>()` is the required-component path and throws a clear exception
when the component is absent. Use `TryGetComponent<T>()` for optional
components. Managed component objects are lightweight entity-ID wrappers; ECS
storage remains native. New script component APIs extend this common mechanism
instead of adding component-specific methods directly to `EntityBehaviour`.

Transform mutation marks the scene graph transform dirty. `Vector3` uses an
explicit sequential layout compatible with the native three-float struct.

The next refactor should introduce a backend-independent `ScriptWorldAPI`.
Both DotNet internal calls and future Lua C functions will delegate to it, so
validation and ECS behavior exist in only one place.

## Scene lifecycle

`Scene::OnRuntimeStart` sets the scripting scene context, finds every
`ScriptComponent`, constructs its managed object, assigns `EntityID`, and
calls `OnCreate`.

Each stepping frame `Scene::OnUpdateRuntime` calls, in this order:

1. `OnFixedUpdate(fixedDeltaTime)` at a fixed 60 Hz (`Time.FixedDeltaTime`): as many
   steps as the frame's time holds, at most 5 (a long hitch does not snowball).
2. `OnUpdate(deltaTime)` once.
3. The update systems: physics, animation, timelines, transform propagation, particles.
4. `OnAnimationEvent(name)` for the animation notifies and timeline events crossed.
5. `OnLateUpdate(deltaTime)`; what it moves is propagated again, so a camera that
   follows a character shows where it is this frame.

`OnFixedUpdate` and `OnLateUpdate` are only called on classes that override them
(`EntityBehaviour.GetCallbackMask`, read once per class).

`Scene::OnRuntimeStop` calls `OnDestroy`, destroys managed handles, clears the
scene context, and then tears down physics.

Scripts execute on the main thread. `Nox.Jobs` runs background work; engine calls
from a job are refused (logged), and reload / shutdown wait for running jobs.

## The project's assembly

The game assembly is the project's: its name is the file name of the project's
`ScriptModulePath` (`Scripts/Binaries/Facerun.dll` -> `Facerun`), else the project's
name, and its C# project is the `.csproj` under the project's `Assets/Scripts` (the one
named like the assembly, else the first). `ScriptEngine::LoadProjectAssembly` loads the
newest of:

- `Scripts/<Name>/<Name>.dll` next to the executable (the editor's CMake build of the
  sample project; a packaged game);
- the project's `ScriptModulePath` (relative to the asset directory);
- the project's own build, `Assets/Scripts/bin/<Configuration>/<Name>.dll`.

`Nox.ScriptCore.dll` comes from `Scripts/Core` next to the executable or the engine
checkout's `NoxScriptCore/bin`, whichever is newer. Opening another project loads its
assembly.

## Hot reload

The editor builds and reloads by itself (`ScriptBuilder`):

- **Script > Build and Reload** (Ctrl+B) runs `dotnet build` on the project's `.csproj`;
- **Build on Save** (on by default): 750 ms after a `.cs` file under the project's
  scripts changed (several saves build once);
- **Reload assembly** (Ctrl+R) reloads what was last built (a build in Visual Studio /
  Rider, for example).

The build runs on its own thread; its compiler errors go to the editor's log (full
output in `obj/NoxBuild.log`). A reload:

1. Copies the assemblies (with `.pdb`, `.deps.json`, `.runtimeconfig.json`) into
   `<Project>/Intermediate/Scripts/Generation-N`: the loaded copies stay locked, the
   build output does not.
2. Waits for running `Nox.Jobs` (2 s, else the reload is skipped).
3. Snapshots the running instances' exposed field values.
4. Loads the copies into a new Assembly Load Context and checks the API version; a
   build that does not load leaves the running scripts as they were.
5. Destroys the old instances, unloads the old context (a new generation: old handles
   stop working).
6. Recreates the instances with the snapshot's values (fields that kept their name and
   type), calls `OnCreate`, then `OnAfterReload`.

## API version

`Nox.ScriptApi.Version` (`ScriptApi.cs`) must equal `DotNetBackend::kScriptApiVersion`.
Raise both whenever the internal calls change: the engine refuses a Nox.ScriptCore of
another version (logged) instead of calling functions that moved.

## Debugging scripts

**Script > Wait for Debugger on Play**: Play waits (up to 60 s) until a .NET debugger
attaches, then breaks once. Visual Studio: Debug > Attach to Process, NoxEditor, code
type *Managed (.NET Core)*; Rider: Run > Attach to Process. The `.pdb` files are staged
with the assemblies, so breakpoints in the scripts hit.

## Editor and serialization target model

The current `ScriptComponent` contains a list of C# class names, and the editor
can add/remove multiple behaviours on one entity. Runtime instances are stored
per entity and updated in list order. This is still a minimal representation;
replace it after reflection/serialization is working with:

```cpp
struct ScriptBehaviour {
    UUID BehaviourID;
    ScriptLanguage Language;
    AssetHandle ScriptAsset;
    std::string TypeName;
    bool Enabled;
    int32_t ExecutionOrder;
    ScriptFieldStorage Fields;
    ScriptInstanceHandle RuntimeInstance; // never serialized
};

struct ScriptComponent {
    std::vector<ScriptBehaviour> Behaviours;
};
```

This permits multiple C# or Lua behaviours on one entity. Scripts should be
assets referenced by handle, not bare paths. Persistent fields belong to
native `ScriptFieldStorage`, keyed by behaviour ID and stable field ID; managed
objects are never authoritative serialized state.

`ScriptValue` is already a language-neutral variant and persists the supported
exposed values in the scene. `[Expose]` carries `Unit`, `Tooltip`, `Min` / `Max`
(the Inspector's range, in the unit when one is set; equal: none) and `ReadOnly`:

```csharp
[Expose(Unit = "m/s", Min = 0, Max = 20, Tooltip = "Top running speed")]
public float Speed = 600.0f;
```

Still to add: display name and a stable field ID. C# obtains it through `[Expose]` reflection; Lua will declare an
equivalent metadata table. Do not return to VanK's fixed 16-byte field buffer.

## Creating a script

The Content Browser has a **New C# Script** button. Enter a valid C# class name
and Nox creates `Assets/Scripts/Source/<Name>.cs` in the current project. The
existing SDK-style `.csproj` includes it automatically; Nox does not create a
project per script or rewrite the solution.

The shipped template is
`NoxEditor/assets/Templates/CSharpBehaviour.cs.template`. It is an ordinary
editable text file and supports `{{NAMESPACE}}` and `{{CLASS_NAME}}`
placeholders. A project can override it by adding
`Assets/Scripts/Templates/CSharpBehaviour.cs.template`; the project template
takes priority over the editor default. This lets projects add comments,
fields, or additional lifecycle methods without changing or rebuilding C++.

The generated class uses the project name as its namespace and includes the
normal lifecycle skeleton:

```csharp
using Nox;

namespace Facerun;

public sealed class MyBehaviour : EntityBehaviour
{
    protected override void OnCreate()
    {
    }

    protected override void OnUpdate(float deltaTime)
    {
    }
}
```

Saving the file builds and reloads the scripts (Build on Save); Ctrl+B does it on
request.

## Smoke tests

### WASD movement

Run `Facerun/Assets/Scripts/config/config-win.bat`, then open the generated
`Facerun/Assets/Scripts/build/win/Facerun.sln` in Visual Studio or Rider. This
follows Coral's managed example: normal SDK-style C# projects are checked in,
and CMake invokes `dotnet build` from the engine build. Only the solution is
generated locally. It explicitly includes both `Facerun.csproj` and
`Nox.ScriptCore.csproj`, so both `Source` folders appear in Solution Explorer.
Run the config script after cloning or when project structure changes. VS Code can open
the repository folder and use the C# Dev Kit. Source belongs under
`Assets/Scripts/Source`; generated DLL/PDB/obj output belongs under
`Assets/Scripts/build` or `Assets/Scripts/Binaries` and must not be imported as
a game asset.

1. Add a Script component to an entity with a Transform.
2. Enter `Facerun.WASDMovement` in the Class field.
3. Enter Play mode.
4. W/S move on Z and A/D move on X.
5. `OnCreate` writes the entity UUID to the engine log.

The initial example moves the transform directly. A physics example should
use a new Rigidbody internal API and apply impulse/force in `FixedUpdate`
rather than writing the transform of a dynamic body.

### Camera test

Name the moving entity `BlenderBox` and attach `Facerun.CameraFollow` to the
camera entity. On creation and every update, the script places the camera at
the target world position plus `(0, Height, Distance)` (defaults to +10 on Z)
and computes the camera Euler rotation in C# so local `-Z` points toward the
target with local `+Y` as up. Native code only exposes transform data; gameplay
camera behavior remains managed. This keeps the target centered and works
across parent/child hierarchies. Assign `Target` in the Script inspector using
the entity picker or hierarchy drag/drop. The reference survives ordinary
renaming; imported nodes use model-instance UUID plus node index. Optional
camera interpolation is still future work.

## Status

Completed:

- Coral integration, unloadable per-generation Assembly Load Contexts, subclass
  discovery, scene runtime integration, multiple behaviours per entity.
- `OnCreate`, `OnUpdate`, `OnFixedUpdate` (60 Hz), `OnLateUpdate`, `OnDestroy`,
  `OnAfterReload`, `OnAnimationEvent`.
- APIs: log, input, entities and components, transforms, rigidbody forces / impulses /
  velocity, character controller, animator, particles, timelines, raycasts, run-time
  meshes and mesh colliders, jobs, world units, scene loading.
- `[Expose]` fields with units, tooltip, range, read-only; entity references, model
  nodes; values persisted in the scene and kept across reloads.
- Per-project game assembly; build on save and Ctrl+B; transactional reload (a broken
  build keeps the running scripts); job fence; API version check; debugger wait.
- Content Browser C# script creation from a template.

Next:

1. Replace class-name entries with stable script behaviour IDs / script assets.
2. Asset references as exposed fields.
3. A Lua backend without changing Scene or serialization contracts.

## Important invariants

- Coral headers stay inside the DotNet backend.
- ECS components never own managed objects or runtime pointers.
- Entity identity crossing a scripting boundary is a UUID.
- Destroy every Coral object and cached reflection pointer before ALC unload.
- Reload only at a main-thread frame safe point.
- Failed builds/reloads must retain the previous working generation.
- Persistent state is native, typed, and language-independent.
- Backend-specific features are capabilities, not assumptions in Scene code.
