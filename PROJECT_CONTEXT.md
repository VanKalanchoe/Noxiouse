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
