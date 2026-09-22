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
  ScriptTypes.h              common language-neutral types
  IScriptBackend.h           backend contract
  ScriptEngine.h/.cpp        scene-facing facade and instance registry
  DotNet/
    DotNetBackend.h/.cpp     Coral host, ALC, reflection and internal calls

NoxScriptCore/
  Nox.ScriptCore.csproj
  Source/
    EntityBehaviour.cs
    InternalCalls.cs
    Vector3.cs
    Input.cs
    Log.cs
    ExposeAttribute.cs

Facerun/
  Assets/Scripts/
    Facerun.csproj             normal SDK-style .NET 9 project
    config/config-win.bat      generates the editable solution locally
    build/win/Facerun.sln      generated; open in Visual Studio/Rider
    Binaries/                  generated managed assemblies
    Source/
      WASDMovement.cs
      CameraFollow.cs
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
- `Input.IsKeyDown`
- `EntityBehaviour.LocalTransform` and `WorldTransform` get/set
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

        Transform transform = Door.LocalTransform;
        transform.Rotation.Y += deltaTime;
        Door.LocalTransform = transform;
    }
}
```

Assign `Door` from the Script component using its entity combo, or drag an entity from the Scene Hierarchy onto the field.

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

Transform mutation marks the scene graph transform dirty. `Vector3` uses an
explicit sequential layout compatible with the native three-float struct.

The next refactor should introduce a backend-independent `ScriptWorldAPI`.
Both DotNet internal calls and future Lua C functions will delegate to it, so
validation and ECS behavior exist in only one place.

## Scene lifecycle

`Scene::OnRuntimeStart` sets the scripting scene context, finds every
`ScriptComponent`, constructs its managed object, assigns `EntityID`, and
calls `OnCreate`.

`Scene::OnUpdateRuntime` calls `OnUpdate(deltaTime)` while the scene is
stepping, before normal update systems propagate dirty transforms.

`Scene::OnRuntimeStop` calls `OnDestroy`, destroys managed handles, clears the
scene context, and then tears down physics.

Scripts currently execute on the main thread. Do not invoke managed scripts
from worker jobs until reload includes an explicit script-job fence.

## Hot reload

The editor menu and Ctrl+R invoke `ScriptEngine::ReloadAssembly`. Compilation
is deliberately explicit: build `Facerun.sln` in Visual Studio/Rider first,
then request reload in the editor. Current behavior:

1. Visual Studio/Rider validates and compiles the projects into their normal
   `bin/<Configuration>` directories.
2. Reload verifies the required DLLs exist and shadow-copies them into a unique
   `build/managed/HotReload/Generation-N` directory to avoid file locking.
3. Destroy managed instances and cached managed types.
4. Unload the Coral Assembly Load Context.
5. Create a new context and load both shadow-copied assemblies.
6. Rediscover subclasses of `Nox.EntityBehaviour`.
7. Recreate running-scene instances and call `OnCreate`.

Reload never invokes a compiler. A candidate that fails managed loading can
still leave scripting unloaded, and runtime fields are not preserved.

Target design:

```text
source change -> debounced dotnet build -> unique generation directory
             -> load/validate candidate ALC
             -> frame-safe state snapshot and swap
             -> restore compatible fields
             -> unload old ALC
```

If candidate compilation or loading fails, the old generation must continue
running. File watching must observe source/project files, not the output DLL,
and must never reload during a managed call.

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
exposed values in the scene. Extend metadata with
display name, range, tooltip, read-only status, default value, and stable
field ID. C# obtains it through `[Expose]` reflection; Lua will declare an
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

Build the existing C# project in Rider/Visual Studio, then use Reload Scripts
in Nox. Reload loads the already-built DLL and never invokes the compiler.

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

- Reproducible pinned Coral FetchContent integration.
- Coral native and managed build/deployment wiring.
- Backend-neutral interface, values, callbacks, and opaque handles.
- Coral HostFXR/CoreCLR startup and shutdown.
- Unloadable per-generation Assembly Load Context.
- `Nox.ScriptCore` and Facerun managed projects.
- Subclass discovery and managed lifecycle invocation.
- Scene runtime integration.
- Manual Ctrl+R/menu reload with play-scene recreation.
- Log, input, entity lookup, and local transform internal calls.
- Script class editor field with validity feedback.
- Multiple script behaviours per entity in the component, editor, and runtime.
- Scene save/load persistence for the current script class list.
- Reflected `[Expose]` fields with typed inspector controls and scene persistence.
- Stable exposed entity references for regular entities and imported model nodes.
- Content Browser C# creation with an `OnCreate`/`OnUpdate` skeleton.
- WASD and camera transform smoke-test scripts.

Next, in order:

1. Verify runtime launch and managed calls in an actual play scene.
2. Add debounced source watching and transactional reload.
3. Snapshot and migrate non-serialized runtime state across reload.
4. Replace class-name entries with stable script behaviour IDs/assets.
5. Add asset references and richer field metadata/attributes.
6. Add `FixedUpdate` and rigidbody force/impulse API.
7. Add optional target-follow camera interpolation.
8. Add script API version validation and debugger support.
9. Add a Lua backend without changing Scene or serialization contracts.

## Important invariants

- Coral headers stay inside the DotNet backend.
- ECS components never own managed objects or runtime pointers.
- Entity identity crossing a scripting boundary is a UUID.
- Destroy every Coral object and cached reflection pointer before ALC unload.
- Reload only at a main-thread frame safe point.
- Failed builds/reloads must retain the previous working generation.
- Persistent state is native, typed, and language-independent.
- Backend-specific features are capabilities, not assumptions in Scene code.
