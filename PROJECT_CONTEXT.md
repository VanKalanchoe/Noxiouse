# Project Context: Noxiouse (NoxEngine / NoxCore / NoxEditor)

> **Workspace:** `E:/dev/noxiouse`  
> **Language Standard:** C++20 / C++23 (CMake 3.31+)  
> **Primary Technology Stack:** Vulkan 1.4, SDL3, Slang, EnTT, GLM, ImGui, ImGuizmo, Taskflow, Box2D, msdf-atlas-gen

---

## 1. Overview & Architecture Summary

**Noxiouse** is a modular 3D game engine and editor built from scratch in C++20/C++23 targeting **Vulkan 1.3 / 1.4**. The architecture emphasizes clean separation of concerns, zero graphics API leaks into high-level logic via the **Nox Rendering Interface (NRI)**, and data-driven entity-component-system (ECS) scene management.

---

## 2. Directory Structure & Key Modules

- **`architecture.md`**: NRI v2/v3 architecture specification (Descriptor Buffers `VK_EXT_descriptor_buffer`, CommandLists, RenderGraph compiler, TaskGraph pipeline).
- **`NoxCore/src/NRI/`**: Zero-leak RHI interface (`Device.h`, `CommandBuffer.h`, `DescriptorHeap.h`, `Pipeline.h`, `Swapchain.h`, `Texture.h`, `Buffer.h`, `SlangCompiler.h`).
- **`NoxCore/src/NRI/Vulkan/`**: Concrete Vulkan 1.3/1.4 backend (`DeviceVK`, `CommandBufferVK`, `DescriptorHeapVK`, `PipelineVK`, `SwapchainVK`, `TextureVK`, `BufferVK`, `MemoryAllocatorVK`).
- **`NoxCore/src/NoxCore/Renderer/`**: Engine renderer (`Renderer.cpp/.h`), batch 2D quad/sprite renderer (`Renderer2D.cpp/.h`), MSDF font rendering (`Font.cpp`), camera abstractions (`Camera.h`, `EditorCamera.cpp`), allocators.
- **`NoxCore/src/NoxCore/Scene/`**: EnTT entity registry manager (`Scene.cpp/.h`), Entity handles (`Entity.h`), Component definitions (`Components.h`), Scene hierarchy (`SceneGraph.h`), YAML serialization (`SceneSerializer.cpp/.h`).
- **`NoxCore/src/NoxCore/Core/`**: Engine application lifecycle, windowing via SDL3, spdlog logging, event dispatching, input.
- **`NoxCore/src/NoxCore/Asset/`**: Asset importing (gLTF via `tinygltf`, `tinyobjloader`, KTX textures, STB image).
- **`NoxCore/src/NoxCore/Physics/`**: 2D Physics engine integration wrapping Box2D.
- **`NoxCore/vendors/`**: Embedded dependencies (`imgui`, `ImGuizmo`, `slang`, `taskflow`, `msdf-atlas-gen`, `meshoptimizer`, `filewatch`, `VulkanMemoryAllocator-Hpp`).
- **`NoxEditor/`**: Editor application target containing `EditorLayer.cpp/.h`, `Main.cpp`, and panels (`SceneHierarchyPanel`, `ContentBrowserPanel`, `ThumbnailCache`).
- **`Facerun/`**: Sandbox project folder containing `.nproj` files (`Facerun.nproj`) and game assets.
- **`scripts/`**: Windows build setup and dependency installation batch scripts (`setup_windows_build.bat`, `install_windows_dependencies.bat`).

---

## 3. Technology Stack & Third-Party Dependencies

- **Platform & Windowing**: `SDL3`
- **Graphics Backend**: `Vulkan 1.4.350` (Vulkan API Version 1.4 tracking, `vk::raii` wrappers, Vulkan Memory Allocator `VMA`)
- **Shading Language Compiler**: `Slang` (Real-time shader compilation to SPIR-V via `SlangCompiler.cpp`)
- **Entity Component System**: `EnTT`
- **Mathematics**: `GLM`
- **Editor GUI**: `Dear ImGui` (with SDL3 and Vulkan backends), `ImGuizmo`
- **Multithreading / Task System**: `Taskflow`
- **Physics**: `Box2D`
- **Font Rendering**: `msdf-atlas-gen`
- **Geometry & Textures**: `tinygltf`, `tinyobjloader`, `meshoptimizer`, `KTX-Software`, `stb_image`
- **Serialization & Logs**: `yaml-cpp`, `spdlog`
- **Utilities**: `xxHash`, `filewatch`

---

## 4. Key Design Guidelines & Architectural Contracts

1. **NRI Zero-Leak Boundary**: Never include `<vulkan/vulkan.h>` or `<vulkan/vulkan.hpp>` outside `NoxCore/src/NRI/Vulkan/`. The rest of the engine must exclusively talk to `NRI::Device`, `NRI::CommandBuffer`, etc.
2. **Modern Bindless Vulkan**: Use Vulkan 1.3/1.4 features (Descriptor Buffers `VK_EXT_descriptor_buffer`, Buffer Device Address) rather than legacy descriptor set allocations.
3. **Slang Integration**: Compile Slang source files to SPIR-V bytecode using `SlangCompiler.cpp`.
4. **ECS Data Driven**: Scene state is managed by EnTT in `Scene.cpp`. Use components in `Components.h`.


# Architecture Migration: Forward to Visibility Buffer + G-Buffer

This document tracks the step-by-step transition of **NoxEngine** from Forward Rendering to a modern **Visibility Buffer + Decoupled Deferred G-Buffer** architecture with HDR Post-Processing and forward overlays.

---

## Migration Roadmap & Final Status

| Step | Phase | Goal | On-Screen Visual Debug Output | Status |
| :---: | :--- | :--- | :--- | :---: |
| **0** | **Vulkan Modernization** | Enable `VK_KHR_unified_image_layouts` & remove engine-wide layout transitions | Engine runs cleanly with zero layout transitions | ✅ **Completed** |
| **1** | **Visibility Buffer & Raster Pass** | Create Visibility Render Target (`R32G32_UINT`) & write `(InstanceID, MeshletID, PrimitiveID)` | **Debug View 1**: Pseudo-colored meshlets mosaic (hash of ID to RGB) | ✅ **Completed** |
| **2** | **Vertex Pulling & Barycentrics** | Fullscreen pass reading VisBuffer, pulling vertex attributes, computing perspective-correct barycentrics | **Debug View 2**: Screen displays reconstructed BaseColor, Normals, and UVs (`frac(u), frac(v)`) | ✅ **Completed** |
| **3** | **G-Buffer Material Generation** | Allocate G-Buffer textures & material resolve pass (Albedo, World Normal + Tangent perturbation, Roughness, Metalness, Emission, EntityID) | **Debug View 3**: G-Buffer views (Albedo, World Normals, Roughness, Metallic, Emissive, Occlusion) | ✅ **Completed** |
| **4** | **Decoupled PBR Lighting Pass** | Evaluate Analytical Lights (Dir, Point, Spot) + IBL (Irradiance, Prefiltered, BRDF LUT) on G-Buffer | **Debug View 4**: Full PBR shading on opaque scene & 13 debug modes (including EntityID & Depth Buffer) | ✅ **Completed** |
| **5** | **Forward 3D Integration & Skybox** | Forward pass for Unlit meshes and Fullscreen Triangle Skybox at depth == 0.0 into HDR scene target | **Debug View 5**: Skybox with zero clipping artifacts, unlit forward meshes | ✅ **Completed** |
| **6** | **Post-Processing & Tonemapping** | Dedicated HDR -> LDR tonemapping pass (`m_hdrSceneResource` -> `m_sceneResource`) with Khronos PBR Neutral | **Debug View 6**: Film-grade tonemapped 3D scene without altering 2D overlays | ✅ **Completed** |
| **7** | **Forward 2D, Outlines & Cleanup** | 2D renderer on LDR scene, outline post-process, mouse picking, NRI format fixes, dead code removal | **Debug View 7**: Full editor viewport, selection outlines, and entity picking | ✅ **Completed** |

---

## Complete Frame Render Graph

```
[ Frame Start: Swapchain -> ColorAttachment ]
       │
       ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Pass 1: Visibility Raster Pass (Indirect Meshlet Shading)              │
│ - Color Attachment:  m_visibilityResource (R32G32_UINT)                │
│ - Depth Attachment:  m_depthResource (Reverse-Z: Clear = 0.0, Test >=) │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
                                   ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Pass 2: G-Buffer Material Generation Pass (Decoupled Resolve)          │
│ - Reads: VisBuffer + BDA Page Tables (Vertices, Indices, Transforms)   │
│ - Writes MRTs:                                                         │
│     Target 0: Albedo (RGBA8)                                           │
│     Target 1: World Normal (R16G16B16A16_SFLOAT)                       │
│     Target 2: Material Params (Roughness, Metallic, Workflow)          │
│     Target 3: Emission (R16G16B16A16_SFLOAT)                           │
│     Target 4: Entity ID (R32SINT)                                      │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
                                   ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Pass 3: Decoupled Deferred PBR Lighting Pass                           │
│ - Reads: G-Buffer MRTs + Depth Buffer + Visibility Buffer              │
│ - Evaluates: Direct Lights (Dir, Point, Spot) + IBL (Irradiance, Spec) │
│ - Debug: 13 Interactive View Modes (Albedo, Normals, EntityID, Depth) │
│ - Output: HDR Target m_hdrSceneResource (R16G16B16A16_SFLOAT)          │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
                                   ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Pass 4: Forward 3D Pass (Unlit & Fullscreen Skybox)                    │
│ - Attachments: m_hdrSceneResource (HDR Color) + m_entityResource       │
│ - Depth Test: Enabled against m_depthResource (Reverse-Z, Write=False) │
│ - Unlit 3D Meshes (m_unlitPipeline)                                    │
│ - Skybox: Fullscreen triangle tested at depth == 0.0 using invViewProj │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
                                   ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Pass 5: Post-Processing & Tonemapping Pass                             │
│ - Reads: m_hdrSceneResource (R16G16B16A16_SFLOAT)                      │
│ - Applies: Exposure + Khronos PBR Neutral Tonemap + Gamma (Mode 0)     │
│ - Bypass: Debug modes (1-12) pass through raw HDR/linear data          │
│ - Writes: m_sceneResource (Surface LDR format)                         │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
                                   ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Pass 6: Forward 2D Overlays & Gizmos                                   │
│ - Attachments: m_sceneResource (Color) + m_entityResource (Entity IDs) │
│ - Depth Test: Enabled against m_depthResource (Reverse-Z, Write=False) │
│ - Renderer 2D (Sprites, Circles, Text, Gizmos with Alpha Blending)     │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
                                   ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Pass 7: Editor Outlines & Presentation                                 │
│ - Outline Post-Process (Edge detection on m_entityResource)            │
│ - Viewport Mouse Picking (copyImageToBuffer from m_entityResource)     │
│ - ImGui::Image Viewport Presentation (Swapchain -> Present)            │
└────────────────────────────────────────────────────────────────────────┘
```
