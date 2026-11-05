# Master File-by-File Index (`475+` Repository Files)

Every source, header, shader, C# script, build file, and architecture document in the Noxiouse repository is cataloged and explained across the sections of this documentation site:

| Category | Directory | File Count | Documentation Page |
| :--- | :--- | :--- | :--- |
| **Root & Build System** | `/`, `NoxCore/`, `NoxEditor/`, `NoxRuntime/` | 8 | [Getting Started & Build System](getting-started.md) |
| **NRI Abstraction Layer** | `NoxCore/src/NRI/` | 20 | [NRI & Vulkan 1.4 Backend](nri-vulkan.md) |
| **Vulkan 1.4 Backend** | `NoxCore/src/NRI/Vulkan/` | 27 | [NRI & Vulkan 1.4 Backend](nri-vulkan.md) |
| **Frame-Compiled Render Graph** | `NoxCore/src/NoxCore/RenderGraph/` | 9 | [Frame-Compiled Render Graph](render-graph.md) |
| **Hybrid Renderer & Passes** | `NoxCore/src/NoxCore/Renderer/` + `Passes/` | 38 | [Hybrid Visibility-Buffer & RT Renderer](renderer.md) |
| **Slang / HLSL Shaders** | `NoxEditor/assets/shaders/` | 113 | [Complete Shader Catalog (113 Shaders)](shaders.md) |
| **Core, Events, ImGui, Math, UI, Utils, Project** | `NoxCore/src/NoxCore/{Core,Events,ImGui,Math,UI,Utils,Project}/` | 42 | [Core, Multithreading & Profiling](core-tasks-profiling.md) |
| **Multithreading & Tasks** | `NoxCore/src/NoxCore/Tasks/` | 9 | [Core, Multithreading & Profiling](core-tasks-profiling.md) |
| **Profiling, F3 Stats & Tracy** | `NoxCore/src/NoxCore/Profiling/` | 10 | [Core, Multithreading & Profiling](core-tasks-profiling.md) |
| **Scene, 47 ECS Components, Prefabs, World Partition & HLOD** | `NoxCore/src/NoxCore/Scene/` | 33 | [ECS Scene, Prefabs, World Partition & HLOD](scene-ecs-world.md) |
| **Asset Pipeline, Importers, Serializers & Streaming** | `NoxCore/src/NoxCore/Asset/` | 44 | [Asset Pipeline, Cooking & Streaming](assets-streaming.md) |
| **3D Jolt Physics & 2D Box2D Physics** | `NoxCore/src/NoxCore/Physics/` + `Jolt/` | 13 | [3D Jolt & 2D Box2D Physics](physics.md) |
| **Skeletal Animation & Sequencer Timeline** | `NoxCore/src/NoxCore/Animation/` | 12 | [Animation, Graphs, Sequencer & NodeGraph](animation-nodegraph.md) |
| **Generic NodeGraph Compiler & Runtime** | `NoxCore/src/NoxCore/NodeGraph/` | 10 | [Animation, Graphs, Sequencer & NodeGraph](animation-nodegraph.md) |
| **C++ Scripting Host (.NET 9 / Coral)** | `NoxCore/src/NoxCore/Scripting/` + `DotNet/` | 7 | [C# Scripting Host & NoxScriptCore API](scripting.md) |
| **Managed C# API (`Nox.ScriptCore`)** | `NoxScriptCore/` + `Source/` | 28 | [C# Scripting Host & NoxScriptCore API](scripting.md) |
| **`NoxEditor` & 11 Editor Panels** | `NoxEditor/src/` + `Panels/` | 31 | [NoxEditor, NoxRuntime & Facerun Sample](editor-runtime-sample.md) |
| **`NoxRuntime` Standalone Player** | `NoxRuntime/src/` | 3 | [NoxEditor, NoxRuntime & Facerun Sample](editor-runtime-sample.md) |
| **`Facerun` Sample Game Project** | `Facerun/` + `Assets/Scripts/` | 18 | [NoxEditor, NoxRuntime & Facerun Sample](editor-runtime-sample.md) |
| **Scripts & Architecture Docs** | `scripts/`, `docs/` | 14 | [NoxEditor, NoxRuntime & Facerun Sample](editor-runtime-sample.md) |
| **Total Inspected & Documented Files** | **Entire Repository (excluding `vendors/`)** | **489** | **100% Coverage** |

:::{tip}
In the interactive web viewer (`docs-site/index.html`), use the **Global Search Bar** in the top-left sidebar or the **Interactive File Explorer** on this page to filter all **489 files** by filename, directory, class name, or feature keyword in real time!
:::
