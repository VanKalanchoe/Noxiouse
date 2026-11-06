# Core Engine, Multithreading, Project & Profiling (`NoxCore`)

This page covers the foundational runtime modules in `NoxCore/src/NoxCore/`: **Core**, **Events**, **ImGui**, **Math**, **UI**, **Utils**, **Project**, **Tasks (Multithreading)**, and **Profiling**.

---

## 1. Core Application, VFS (`.npak`), Units & Utilities

### Plain-English Overview
- **Application Lifecycle ([`Application.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Application.cpp))**: Boots the logger, mounts `.npak` archives if present, initializes the multi-threaded `JobSystem`, creates the SDL3 window (`Window`), initializes `Renderer`, `PhysicsEngine`, and `ScriptEngine`, and runs the main loop across a `LayerStack` (`EditorLayer` or `RuntimeLayer`, `StatsOverlayLayer`, `ImGuiLayer`).
- **Virtual File System & `.npak` Archives ([`VirtualFileSystem.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/VirtualFileSystem.cpp))**:
  - Maps logical asset paths through mounted `.npak` archives first, falling back to the OS filesystem in the editor.
  - Supports **lock-free byte-range reads** (`ReadBytes(path, offset, size, dst)`) via Windows `ReadFile` with `OVERLAPPED` offsets (or POSIX `pread`), allowing multiple IO worker threads to stream 128 KB geometry pages and texture mips out of the same `.npak` file simultaneously without mutex locks!
- **World Units ([`WorldUnits.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/WorldUnits.cpp))**: Stores the active project's `UnitsPerMeter` scale (default `100.0f` = `1 cm` per world unit) and provides `Meters(x)`, `Centimeters(x)`, `ToMeters(v)`, and `FromMeters(v)`.
- **Large World Coordinate Math ([`Math.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Math/Math.cpp))**: Provides `Math::RelativeTo(const glm::dmat4& world, const glm::dvec3& origin)` which subtracts the camera's double-precision world origin in `double` before casting to `glm::mat4` (`float`) for the GPU.

### File-by-File Reference: `Core/`, `Events/`, `ImGui/`, `Math/`, `UI/`, `Utils/`, `Project/` (42 Files)

| File | Plain-English Summary & Key Types |
| :--- | :--- |
| [`NoxPCH.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxPCH.h) | Precompiled header for `NoxCore`, pre-parsing standard library containers, `<glm/glm.hpp>`, `<spdlog/spdlog.h>`, and `Core/core.h`. |
| [`Core/core.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/core.h) | Defines `Scope<T>` (`std::unique_ptr<T>`), `Ref<T>` (`std::shared_ptr<T>`), `CreateScope`, `CreateRef`, `BIT(x)`, and debugger-aware `NOX_ASSERT` / `NOX_CORE_ASSERT` macros. |
| [`Core/Ref.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Ref.h) | Defines `RefCounted` (with atomic `m_RefCount`), intrusive smart pointer `IntrusiveRef<T>`, and `WeakRef<T>`. |
| [`Core/Application.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Application.h) & [`Application.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Application.cpp) | Top-level engine host (`Application`). Manages `Window`, `Renderer`, `LayerStack`, main-thread task queue (`SubmitToMainThread`), deferred post-frame hooks (`DeferPostFrame`), and the main loop. |
| [`Core/Layer.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Layer.h) & [`Layer.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Layer.cpp) | Defines `Layer` (`OnAttach`, `OnDetach`, `OnUpdate`, `OnImGuiRender`, `OnEvent`) and `LayerStack` (ordered layers + top-most overlays). |
| [`Core/Window.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Window.h) & [`Window.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Window.cpp) | Wraps an `SDL_Window*` configured with `SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY`, polls SDL3 events, tracks mouse delta/scroll, and manages relative mouse capture (`SetCursorLocked`). |
| [`Core/Input.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Input.h) & [`Input.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Input.cpp) | Static polling API (`Input::IsKeyPressed`, `IsMouseButtonPressed`, `GetMousePosition`, `GetMouseDelta`, `GetMouseScrollDelta`, `SetCursorLocked`). |
| [`Core/Buffer.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Buffer.h) | Non-owning (`Buffer`) and RAII owning (`ScopedBuffer`) byte buffer views (`uint8_t* Data`, `uint64_t Size`). |
| [`Core/Timestep.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Timestep.h) | Frame delta-time wrapper (`GetSeconds()`, `GetMilliseconds()`). |
| [`Core/UUID.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/UUID.h) & [`UUID.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/UUID.cpp) | 64-bit random unique identifier (`UUID`, generated via `std::mt19937_64`) used for all entities, assets, and bone nodes. |
| [`Core/Hash.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Hash.h) & [`Hash.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Hash.cpp) | Compile-time `constexpr` FNV-1a 32-bit string hash (`Hash::GenerateFNVHash`) and runtime CRC32 (`Hash::CRC32`). |
| [`Core/Log.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Log.h) & [`Log.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/Log.cpp) | Initializes dual `spdlog` loggers (`NOX` core logger and `APP` client logger) writing colorized console output plus `Nox.log`. |
| [`Core/VirtualFileSystem.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/VirtualFileSystem.h) & [`VirtualFileSystem.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/VirtualFileSystem.cpp) | Implements `.npak` binary archive reading/writing (`NPAK` v1 magic, FNV-1a 64-bit path lookup) and lock-free `OVERLAPPED`/`pread` byte-range streaming (`ReadBytes`). |
| [`Core/WorldUnits.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/WorldUnits.h) & [`WorldUnits.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Core/WorldUnits.cpp) | Manages the project's world-units-per-meter ratio (`100.0` default = `1 cm`) and unit conversion helpers. |
| [`Events/Event.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Events/Event.h) | Defines `EventType`, `EventCategory`, `Event` base class, and type-safe `EventDispatcher`. |
| [`Events/InputEvents.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Events/InputEvents.h) | Defines `KeyPressedEvent` (capturing `SDL_Keymod` at key-down time), `KeyReleasedEvent`, `MouseMovedEvent`, `MouseScrolledEvent`, `MouseButtonPressedEvent`, `MouseButtonReleasedEvent`, and `ExternalFileDropEvent`. |
| [`Events/WindowEvents.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Events/WindowEvents.h) | Defines `WindowClosedEvent` and `WindowResizeEvent`. |
| [`ImGui/ImGuiLayer.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/ImGui/ImGuiLayer.h) & [`ImGuiLayer.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/ImGui/ImGuiLayer.cpp) | Manages Dear ImGui (docking, DPI scaling, OpenSans fonts) and `ImGuizmo` theme/frames in editor builds. |
| [`Math/Math.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Math/Math.h) & [`Math.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Math/Math.cpp) | Implements `DecomposeTransform` (for both `glm::mat4` and LWC `glm::dmat4`) and camera-relative `Math::RelativeTo` helpers. |
| [`UI/UI.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/UI/UI.h) | RAII `Nox::UI::ScopedStyleColor` wrapper around `ImGui::PushStyleColor` / `PopStyleColor`. |
| [`Utils/NOXWatcher.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Utils/NOXWatcher.h) & [`NOXWatcher.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Utils/NOXWatcher.cpp) | Background directory watcher (`filewatch::FileWatch`) with `last_write_time` verification and 300ms debouncing to ignore false-positive read events. |
| [`Utils/PlatformUtils.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Utils/PlatformUtils.h) & [`PlatformUtils.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Utils/PlatformUtils.cpp) | Queries OS process RAM (`WorkingSetBytes`, `PrivateBytes`) and `IsDebuggerAttached()`. |
| [`Utils/Utils.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Utils/Utils.h) & [`Utils.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Utils/Utils.cpp) | Native SDL3 file open/save dialogs and memoized streaming 128-bit `XXH3_128bits` file hashing (`calcul_hash_streaming`) + `.hash` sidecar I/O. |
| [`Project/Project.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Project/Project.h) & [`Project.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Project/Project.cpp) | Holds `ProjectConfig` (`Name`, `StartScene`, `AssetDirectory`, `UnitsPerMeter`, `ProjectRendererSettings`) and owns the active `AssetManagerBase` (`EditorAssetManager` or `RuntimeAssetManager`). |
| [`Project/ProjectRendererSettings.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Project/ProjectRendererSettings.h) | Serializable struct capturing all project-level rendering settings (VSync, Upscaler/DLSS, GI mode, Shadow/Reflection/AO modes, Path Tracer bounces, NRD denoiser, Tonemapper, DDGI cascades, Virtual Texturing, Geometry Streaming). |
| [`Project/ProjectSerializer.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Project/ProjectSerializer.h) & [`ProjectSerializer.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Project/ProjectSerializer.cpp) | Reads and writes `.nproj` YAML project files, persisting both `ProjectConfig` and `ProjectRendererSettings`. |

---

## 2. Multithreading, Job System & Frame Arenas (`NoxCore/Tasks/` — 9 Files)

### Plain-English Overview
Noxiouse uses a multi-pool **Taskflow-backed Job System** (`JobSystem`) designed so long asset cooks and disk I/O never starve frame-critical animation, physics, or Render Graph command recording:
- **3 Dedicated Thread Pools**:
  1. **Frame Workers (`N - 1` threads)**: Reserved for frame-critical parallel work (`SceneGraph::UpdateWorldTransforms`, parallel `Animator` evaluation, Jolt physics jobs, and parallel Render Graph command buffer recording).
  2. **Background Workers (`max(1, N / 2)` threads)**: Runs multi-frame CPU-heavy jobs (`MeshImporter` Cluster LOD cooking, `TextureImporter` BC7/BC5 compression, `HLODBuilder` baking).
  3. **IO Workers (`2` threads)**: Dedicated to asynchronous disk/`.npak` reads and `AssetRegistry.nxr` YAML serialization.
- **Lock-Free Per-Worker Frame Arenas (`FrameArena` + `WorkerLocal<T>`)**:
  - Every worker thread owns a 1 MB bump-pointer linear allocator (`FrameArena`) that is reset in $O(1)$ at the start of every frame (`ResetFrameArenas()`).
  - Used for Render Graph execute lambdas, `EntityCommandBuffer` recordings, and temporary scratch buffers with **zero `malloc`/`free` calls or mutex contention**.
- **Declarative System Dependency Graph (`SystemGraph`)**:
  - Automatically orders engine systems by inspecting their read/write resource declarations (`ReadComponent<T>`, `WriteComponent<T>`): **Write-after-Write** and **Write-after-Read** insert hard dependencies, while systems that only **Read** the same component run in parallel!

### File-by-File Reference: `NoxCore/src/NoxCore/Tasks/` (9 Files)

| File | Plain-English Summary & Key Types |
| :--- | :--- |
| [`JobSystem.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Tasks/JobSystem.h) & [`JobSystem.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Tasks/JobSystem.cpp) | Manages the 3 `tf::Executor` thread pools (`m_FrameExecutor`, `m_BackgroundExecutor`, `m_IOExecutor`). Provides `ParallelFor` (automatically running inline on the caller if already inside a worker to prevent nested deadlocks), `RunTasks`, `DispatchBackground`, `DispatchIO`, and `WaitForAll`. |
| [`FrameArena.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Tasks/FrameArena.h) & [`FrameArena.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Tasks/FrameArena.cpp) | Bump-pointer linear arena allocator (`Allocate`, `Create<T>`, `AllocateArray<T>`, `Reset`) growing in 1 MB blocks and retaining peak capacity across frames. |
| [`WorkerLocal.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Tasks/WorkerLocal.h) | Cache-line padded (`alignas(64)`) per-worker storage container (`WorkerLocal<T>`) indexed by `JobSystem::CurrentWorkerIndex()` to prevent false sharing across CPU cores. |
| [`SystemGraph.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Tasks/SystemGraph.h) & [`SystemGraph.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Tasks/SystemGraph.cpp) | Builds a `tf::Taskflow` DAG from system component/resource read-write declarations (`AddSystem`, `ReadComponent<T>`, `WriteComponent<T>`, `Run`). Includes a fallback mode (`NOX_SINGLE_THREAD_SYSTEMS=1`) that executes systems serially in topological order for deterministic debugging. |
| [`TaskObserver.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Tasks/TaskObserver.h) & [`TaskObserver.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Tasks/TaskObserver.cpp) | Implements `tf::ObserverInterface` to record every worker task's start/end timestamp, thread index, and name into `Profiler` and Tracy. |

---

## 3. Telemetry, F3 Stats Overlay & Tracy Profiler (`NoxCore/Profiling/` — 10 Files)

### Plain-English Overview
- **Zero-Overhead Telemetry (`Profiler`)**: Collects a 240-frame rolling ring buffer of CPU scope timings (`NOX_PROFILE_SCOPE`), worker task execution timelines, GPU pass timestamps & mesh/fragment invocation counts, process RAM / GPU VRAM usage, and asset/world streaming counters.
- **In-Game F3 Overlay (`StatsOverlay` + `StatsOverlayLayer`)**: Press **F3** in either `NoxEditor` or `NoxRuntime` to cycle through a 5-page translucent HUD rendered directly via `Renderer2D` (works even in shipping `NoxRuntime` builds without Dear ImGui!):
  - **Page 1 (`Summary`)**: FPS, frame time, 1% low, CPU/GPU/RenderThread ms, draw calls, instances, VRAM, and 120-frame sparkline.
  - **Page 2 (`CPU Scopes`)**: Hierarchical CPU scope timings with average and max ms.
  - **Page 3 (`GPU Passes`)**: Per-RenderGraph-pass GPU milliseconds and task/mesh/fragment shader invocations.
  - **Page 4 (`Memory & Streaming`)**: Process RAM, VMA GPU budgets, Texture Streamer residency, Virtual Texture tiles, Geometry Streamer pages, and World Partition cell latency.
  - **Page 5 (`Thread Timeline`)**: Horizontal Gantt chart of the Main Thread + all Frame/Background/IO worker threads.
- **JSON & Chrome `about://tracing` Exporter (`StatsReport`)**: Press **Shift+F3** (or click Export in the Profiler panel) to dump a `.json` summary report and a `trace.json` Chrome Tracing / Perfetto timeline.

### File-by-File Reference: `NoxCore/src/NoxCore/Profiling/` (10 Files)

| File | Plain-English Summary & Key Types |
| :--- | :--- |
| [`Profiler.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Profiling/Profiler.h) & [`Profiler.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Profiling/Profiler.cpp) | Defines `NOX_PROFILE_FUNCTION()`, `NOX_PROFILE_SCOPE(name)`, `ProfileScopeTimer`, `FrameSample`, and the `Profiler` singleton storing the 240-frame ring buffer. |
| [`StatsOverlay.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Profiling/StatsOverlay.h) & [`StatsOverlay.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Profiling/StatsOverlay.cpp) | Renders the 5-page **F3** HUD (`Summary`, `CPUScopes`, `GPUPasses`, `MemoryAndStreaming`, `ThreadTimeline`) using `Renderer2D` quads, lines, and MSDF text. |
| [`StatsOverlayLayer.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Profiling/StatsOverlayLayer.h) & [`StatsOverlayLayer.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Profiling/StatsOverlayLayer.cpp) | Engine `Layer` that listens for **F3** (cycle overlay pages) and **Shift+F3** (write JSON + Chrome trace report to `profiling/`). |
| [`StatsReport.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Profiling/StatsReport.h) & [`StatsReport.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Profiling/StatsReport.cpp) | Exports 240-frame profiling metrics to JSON (`WriteStatsReport`) and Chrome Tracing / Perfetto format (`WriteChromeTrace`). |
| [`TracyBackend.h`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Profiling/TracyBackend.h) & [`TracyBackend.cpp`](file:///E:/dev/noxiouse/NoxCore/src/NoxCore/Profiling/TracyBackend.cpp) | Bridges `Profiler` CPU zones, worker tasks, plots, and calibrated GPU timestamps into the **Tracy v0.14.1** profiler when built with `-DNOX_ENABLE_TRACY=ON`. |

---

## Frame Profiler Extras (November 2026)

The editor's frame timeline (Window > Profiler) gained:

- **Save / Load captures** (`.noxprof`, binary): the kept frames, the slow frames and the scope / thread names. A loaded capture is
  shown instead of the live frames until **Close Capture**, so a capture can be sent and opened on another machine.
- **Spikes:** frames slower than a threshold (frame or GPU time, default 16.7 ms) are copied apart once their GPU timings are in --
  the last 64 -- and picked in the panel's frame combo, so a hitch stays to look at after it scrolled out (Unreal Insights' and
  PIX's "find the spikes").
- **Memory graph:** VRAM (device-local heaps) and process RAM per frame, drawn under the frame graph.
- **GPU Transfer track:** one scope per upload submission (texture / geometry streaming) next to the graphics and async compute
  tracks: timestamps of the dedicated transfer family in a query pool of its own, reset on the host (Vulkan 1.2 `hostQueryReset`,
  since `vkCmdResetQueryPool` is not allowed on a transfer-only queue), placed on the CPU clock through the calibrated timestamps.
