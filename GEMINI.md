# Antigravity Rules & User Directives

Engine overview, directory map and frame flow: `PROJECT_CONTEXT.md`. Render graph and NRI design: `architecture.md`.

## 1. Execution Policy (Strict)
- **NEVER execute terminal commands, scripts (`.bat`, `.ps1`, shell), builds, or background tasks** unless the user explicitly commands you to execute them (e.g. "run this command", "execute the script").
- When diagnosing, troubleshooting, or fixing scripts and tools: **only inspect, fix the files, and explain the changes**. The user will run the commands/scripts themselves.
- Never spawn compilers, background tasks, or testing runs autonomously.

## 2. Architectural Guidelines
- **Zero Graphics Leak**: Vulkan headers (and `vk_mem_alloc.h`, Streamline, NRD) remain strictly within `NoxCore/src/NRI/Vulkan/`. Engine code uses NRI types only.
- **Bindless**: resources are heap slots of `VK_EXT_descriptor_heap` and buffer device addresses in push constants. No descriptor sets.
- **Reverse-Z Projection**: Near plane is `1.0`, far plane / infinity is `0.0`. Depth comparison uses `GREATER` or `GREATER_OR_EQUAL`.
- **C++20** (root `CMakeLists.txt`). Shaders are Slang, compiled by `NoxCore/src/NRI/SlangCompiler.cpp`; GPU structs shared with C++ go in `NoxEditor/assets/shaders/shaderIO.h` with identical layout on both sides.
- **New GPU work is a render graph pass** (`NoxCore/src/NoxCore/Renderer/Passes/*.cpp`): declare every resource access in setup; execute callbacks only record.

## 3. Rendering Pipeline Invariants (Strict)
Shaders live in `NoxEditor/assets/shaders/`.
- **Deferred visibility-buffer renderer (primary)**:
  - Rasterization: task stage `Material_PBR_MeshTask.slang` (cluster LOD cut and meshlet culling, shared by every meshlet pipeline) + `VisibilityBuffer.slang` (mesh and fragment stages), after GPU culling (`InstanceCulling.slang`, `HiZBuild.slang`)
  - Material resolution: `GBufferMaterial.slang`
  - Decoupled RT passes: `ShadowMask.slang` (RT shadows), `Reflection.slang`, `RTAO.slang`; GI: `DDGI*.slang`, `ScreenProbe*.slang`, `RTXDI/GI/*`; ReSTIR DI: `RTXDI/DI/*`, `RTXDI/Presampling/*`
  - Lighting & composition: `DeferredLighting.slang` (shared BRDF and light code in `PBRLighting.slang`)
  - Denoising & upscaling: NRD (SIGMA, REBLUR / RELAX) + DLSS / DLSS-RR through Streamline, or `TAA.slang`
  - Post: `PostProcess.slang` with `Tonemapping.slang` (default Hable UC2)
- **Forward passes (live, not dead)**: the task stage above and the mesh stage of `Material_PBR_Mesh.slang` (its own `fragMain` is unused) with the fragment shaders `Material_Unlit_Mesh.slang` (unlit), `Material_TransparentLit_Mesh.slang` (transparent and the editor's extra views) and `Wireframe.slang`. Edit them when unlit, transparent or extra-view shading changes. Opaque material changes still belong in `GBufferMaterial.slang` and `DeferredLighting.slang`.
- **Path tracer**: the ray tracing pipeline in `PathTracer/` (`PathTracerRayGen.slang`, `PathTracerHit.slang`, `PathTracerAnyHit.slang`, `PathTracerMiss.slang`, BSDF files), composited by `PTComposite.slang`; ReSTIR PT in `RTXDI/PT/*`. `PathTracer.slang` at the top level is an older mesh/fragment-shader path tracer that no pipeline compiles any more.
