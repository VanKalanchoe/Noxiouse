# Noxiouse

A 3D game engine and editor written from scratch in C++20 on Vulkan 1.4.

- **NoxCore** (engine library): NRI, a thin graphics layer over Vulkan (bindless through `VK_EXT_descriptor_heap`); a render
  graph; a GPU scene with GPU culling and cluster LOD; a visibility-buffer + deferred renderer with ray-traced shadows,
  reflections and AO, GI (DDGI, screen probes, ReSTIR GI), ReSTIR DI, a path tracer with ReSTIR PT, NRD denoising, DLSS /
  DLSS-RR and TAA; EnTT scenes, prefabs, animation graphs and timelines; Jolt (3D) and Box2D (2D) physics; C# scripting
  through Coral on .NET 9; a Taskflow job system.
- **NoxEditor**: the editor.
- **NoxRuntime**: the standalone player, also used by File > Package Game.
- **NoxScriptCore**: the C# API for game scripts.
- **Facerun**: the sample project (world unit = 1 cm).

## Documentation

| File | What it covers |
|---|---|
| `PROJECT_CONTEXT.md` | Overview, directory map, frame flow, current features, design rules |
| `architecture.md` | NRI, descriptor heaps, render graph, GPU scene, job system |
| `GEMINI.md` | Rules for coding agents working in this repo |
| `docs/Status_Audit_2026-10.md` | What is built and what is open, checked against the code |
| `docs/Backlog_2026.md` | Index of open work |
| `docs/Engine_Architecture_Plan_2026.md` | Threading, render graph, GPU scene, memory, streaming, profiling (with measurements) |
| `docs/RayTracing_Architecture_Plan_2026.md` | Lighting, GI, denoising, DLSS, path tracer |
| `docs/Hybrid_Renderer_Plan_2026-10.md` | Hybrid renderer quality levels, procedural sky, VFX |
| `docs/Scripting_Architecture_and_Status.md` | C# scripting |
| `docs/Animation_Graph_Architecture_Plan_2026.md`, `docs/Prefab_Architecture_Plan_2026.md`, `docs/Units_And_World_Tools_Plan_2026.md`, `docs/Runtime_And_Packaging_Plan_2026-10.md`, `docs/Editor_Features_Plan_2026-10.md` | Feature plans with status logs |

## Building (Windows)

The setup scripts target Windows with Visual Studio 2026 and ClangCL.

1. Install the **Vulkan SDK 1.4.350 or newer** (CMake requires it) and the **.NET 9 SDK** (the editor build runs
   `dotnet build` for `NoxScriptCore` and the Facerun scripts).
2. Fetch the git submodules in `NoxCore/vendors/` (taskflow, imgui, ImGuizmo, tinygltf, meshoptimizer, msdf-atlas-gen,
   JoltPhysics, tracy, OffsetAllocator, bc7enc_rdo, VulkanMemoryAllocator-Hpp).
3. Run `scripts/install_windows_dependencies.bat`. It installs vcpkg into `NoxCore/vendors/vcpkg` and the vcpkg packages
   (SDL3, GLM, EnTT, spdlog, xxHash, yaml-cpp, Box2D, stb, KTX, tinyobjloader, ...), and downloads Slang, NVIDIA Streamline,
   NRD, the RTXDI library and filewatch into `NoxCore/vendors/`.
4. Put [imgui-node-editor](https://github.com/thedmd/imgui-node-editor) (master) in `NoxCore/vendors/imgui-node-editor`. The
   script does not fetch it; `NoxCore/CMakeLists.txt` lists the files it compiles, including `utilities/drawing.cpp` and
   `utilities/widgets.cpp` from its blueprints example.
5. Run `scripts/setup_windows_build.bat` to configure into `build/`, then build. Coral is fetched by CMake
   (`NoxCore/CMake/Dependencies.cmake`).

CMake options:

| Option | Default | Effect |
|---|---|---|
| `NOX_BUILD_RUNTIME` | `OFF` | Also build `NoxRuntime` (needed for File > Package Game). |
| `NOX_DIST` | `OFF` | Shipping runtime: no console, no profiler, log in the user's folder. Use with the Release configuration; turns `NOX_BUILD_RUNTIME` on. |
| `NOX_PROFILE_STATS` | `ON` | The Nox Stats overlay (F3). |
| `NOX_ENABLE_TRACY` | `OFF` | Tracy profiler client. |

Run the editor from `NoxEditor/` as the working directory (Visual Studio is set up that way) and open
`Facerun/Facerun.nproj` (pass it on the command line or open it from the editor). `NoxRuntime <project.nproj>` plays a
project's start scene. Timings are only compared in Release builds.
