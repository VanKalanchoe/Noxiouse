# Getting Started, Build System & Project Structure

This page explains how Noxiouse is configured, built, and structured on disk in plain, simple language.

---

## 1. System Requirements

- **OS**: Windows 10 / 11 (x64)
- **GPU**: Vulkan 1.4 capable GPU (`1.4.350+` SDK) supporting:
  - `VK_EXT_descriptor_heap` (Bindless resource & sampler heaps)
  - `VK_KHR_unified_image_layouts` (Permanent `VK_IMAGE_LAYOUT_GENERAL`)
  - `VK_EXT_mesh_shader` (Task + Mesh shader stages)
  - `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_ray_tracing_pipeline` (Hardware Ray Tracing)
  - Optional NVIDIA RTX features: **DLSS Super Resolution** and **DLSS Ray Reconstruction** via Streamline (`sl.interposer.dll`), plus **NRD** real-time denoisers.
- **Compiler & Tooling**:
  - Visual Studio 2022 (MSVC C++20)
  - CMake `3.25+`
  - .NET 9.0 SDK (`net9.0`) for building `Nox.ScriptCore` and game C# scripts

---

## 2. Root Build Files & CMake Options

### Root Files Inspected

| File Path | Plain-English Explanation |
| :--- | :--- |
| [`CMakeLists.txt`](file:///E:/dev/noxiouse/CMakeLists.txt) | Root CMake build definition (`C++20`). Defines build options (`NOX_DIST`, `NOX_BUILD_EDITOR`), configures **Jolt Physics** (with `JPH_DOUBLE_PRECISION=ON` for 64-bit Large World Coordinates and `JPH_DEBUG_RENDERER`), adds the **Coral** .NET 9 C++/C# interop library (`NoxCore/vendors/Coral/Coral.Native`), adds `msdf-atlas-gen` for SDF font generation, and includes `NoxCore`, `NoxEditor`, and `NoxRuntime`. |
| [`README.md`](file:///E:/dev/noxiouse/README.md) | Quick-start overview of the repository, prerequisites, and build steps. |
| [`PROJECT_CONTEXT.md`](file:///E:/dev/noxiouse/PROJECT_CONTEXT.md) | High-level architectural map of the engine's modules, conventions, and frame flow. |
| [`architecture.md`](file:///E:/dev/noxiouse/architecture.md) | Deep-dive specification of the `NRI` abstraction layer and the frame-compiled `RenderGraph` design decisions (`D1–D15`). |
| [`GEMINI.md`](file:///E:/dev/noxiouse/GEMINI.md) | Repository engineering rules (Zero Graphics Leak, Bindless, Reverse-Z, Render Graph passes, and Rendering Pipeline invariants). |
| [`NoxCore/CMakeLists.txt`](file:///E:/dev/noxiouse/NoxCore/CMakeLists.txt) | Builds the `NoxCore` static library with precompiled header `src/NoxPCH.h`, linking SDL3, GLM (`GLM_FORCE_EXPLICIT_CTOR`), Vulkan 1.4, EnTT, Slang (delay-loaded via `/DELAYLOAD:slang.dll`), Streamline, NRD, RTXDI, Dear ImGui, ImGuizmo, `imgui-node-editor`, `meshoptimizer`, `OffsetAllocator`, `bc7enc`/`rgbcx`, `xatlas`, `bcdec`, `yaml-cpp`, `spdlog`, `xxHash`, `box2d`, `msdf-atlas-gen`, `KTX`, and `Jolt`. |
| [`NoxEditor/CMakeLists.txt`](file:///E:/dev/noxiouse/NoxEditor/CMakeLists.txt) | Builds the `NoxEditor` executable, runs `dotnet build` on `NoxScriptCore/Nox.ScriptCore.csproj` and `Facerun/Assets/Scripts/Facerun.csproj`, and copies required runtime DLLs (SDL3, Slang, Streamline/DLSS/NGX, NRD, KTX) next to `NoxEditor.exe`. |
| [`NoxRuntime/CMakeLists.txt`](file:///E:/dev/noxiouse/NoxRuntime/CMakeLists.txt) | Builds the lightweight `NoxRuntime` shipping game executable (`WIN32` windowed subsystem when `NOX_DIST=ON`) and copies runtime DLLs so **File > Package Game** can bundle it directly. |

### Key CMake Options

| Option | Default | What It Does |
| :--- | :--- | :--- |
| `NOX_BUILD_EDITOR` | `ON` | Builds the `NoxEditor` authoring application. |
| `NOX_DIST` | `OFF` | Enables distribution/shipping mode: strips developer overlays, hides the console window on `NoxRuntime`, and locks runtime configurations. |
| `NOX_PROFILE_STATS` | `ON` | Compiles the built-in CPU/GPU/Memory/Streaming telemetry collector and **F3** in-game stats overlay. |
| `NOX_ENABLE_TRACY` | `OFF` | Compiles the on-demand **Tracy v0.14.1** profiler client into `NoxCore`. |
| `ENABLE_CPP20_MODULE` | `OFF` | Optional C++20 module compilation for `vulkan.cppm`. |

---

## 3. Coordinate System & World Units

Noxiouse uses a right-handed coordinate system with **+Y Up**, **-Z Forward**, and **+X Right**:

- **Default World Unit (`UnitsPerMeter = 100.0f`)**: By default, **1 world unit = 1 centimeter (`1 cm`)**, matching Unreal Engine conventions (so a 1.8m character is `180` units tall).
- **Per-Project Customization**: A project's `.nproj` file can set `UnitsPerMeter: 100.0` (centimeters) or `UnitsPerMeter: 1.0` (meters).
- **Physics & Atmosphere Automatic Scaling**:
  - Jolt 3D Physics and Box2D v3 always simulate internally in **SI meters (`m`, `m/s`, `kg`)** for numerical stability. `Physics3D` and `Physics2D` convert positions and velocities at the API boundary using `WorldUnits::ToMeters()` and `WorldUnits::FromMeters()`.
  - C# scripts can annotate `[Expose(Unit = "m/s")]` on fields so the Inspector displays human-friendly units while passing native world units to the script.

---

## 4. Anatomy of a Noxiouse Project (`.nproj`)

A Noxiouse project (such as [`Facerun/Facerun.nproj`](file:///E:/dev/noxiouse/Facerun/Facerun.nproj)) is a YAML file pointing to an `Assets/` folder:

```yaml
Project:
  Name: Facerun
  StartScene: Scenes/SampleScene.nox
  AssetDirectory: Assets
  AssetRegistryPath: Assets/AssetRegistry.nxr
  ScriptModulePath: Assets/Scripts/bin
  UnitsPerMeter: 100.0
```

### Asset File Extensions Reference

| Extension | Magic / Format | Asset Type | Plain-English Description |
| :--- | :--- | :--- | :--- |
| **`.nproj`** | YAML | Project | Project configuration, startup scene, unit scale, and saved `ProjectRendererSettings`. |
| **`.nxr`** | YAML | Asset Registry | Maps 64-bit `AssetHandle` UUIDs to cooked paths, source paths, import settings, and `FormerPaths` redirectors. |
| **`.nox`** | YAML | Scene | Human-readable scene file storing all entities and their 47 ECS components. |
| **`.noxcell`** / **`.noxcell.cooked`** | YAML / Binary (`NXCL`) | World Partition Cell | Spatially partitioned streaming cell (authored YAML + auto-cooked binary cache). |
| **`.nprefab`** | YAML | Prefab | Reusable entity hierarchy or Prefab Variant (`BasePrefab` + property/structural overrides). |
| **`.ntex`** | Binary (`NTX1` / `NTV1`) | Texture2D | Cooked BC7/BC5/BC4/FP16 texture with mip chain (`NTX1`) or 128x128 + 4px border Virtual Texture tiles (`NTV1`). |
| **`.nsmesh`** / **`.nmesh`** | Binary (`NSM;` / `NMS;`) | Static / Skeletal Mesh | Cooked mesh with 36B packed vertices, material slots, and 128 KB Nanite-style Cluster LOD streaming pages. |
| **`.nskel`** | Binary (`NSKL`) | Skeleton | Joint hierarchy, rest transforms, and inverse bind matrices. |
| **`.nanim`** | Binary (`NAN2`) | Animation Sequence | Keyframed bone translation/rotation/scale channels and morph target curves (+ `.nanim.notifies` YAML sidecar). |
| **`.nanimgraph`** | YAML | Animation Graph | Visual node graph asset with state machines, blend spaces, montage slots, and parameters. |
| **`.nmat`** | YAML / Binary (`NMA3`) | Material | PBR + OpenPBR/glTF multi-lobe material definition. |
| **`.ntimeline`** | YAML | Sequencer Timeline | Cutscene/sequence asset with 11 track types (transforms, lights, FOV, camera cuts, sub-timelines, morphs, materials, events). |
| **`.npak`** | Binary (`NPAK` v1) | Packaged Archive | Contiguous read-only archive produced by **File > Package Game**, mounted at runtime via `VirtualFileSystem`. |
