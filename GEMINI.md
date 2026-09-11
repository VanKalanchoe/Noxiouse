# Antigravity Rules & User Directives

## 1. Execution Policy (Strict)
- **NEVER execute terminal commands, scripts (`.bat`, `.ps1`, shell), builds, or background tasks** unless the user explicitly commands you to execute them (e.g. "run this command", "execute the script").
- When diagnosing, troubleshooting, or fixing scripts and tools: **only inspect, fix the files, and explain the changes**. The user will run the commands/scripts themselves.
- Never spawn compilers, background tasks, or testing runs autonomously.

## 2. Architectural Guidelines
- **Zero Graphics Leak**: Vulkan headers remain strictly within `NoxCore/src/NRI/Vulkan/`.
- **Reverse-Z Projection**: Near plane is `1.0`, far plane / infinity is `0.0`. Depth comparison uses `GREATER` or `GREATER_OR_EQUAL`.

## 3. Rendering Pipeline Invariants (Strict)
- **Active Renderers in NoxEngine**:
  1. **Deferred Visibility Buffer Renderer (Primary)**:
     - Rasterization: `VisibilityBuffer.slang`
     - Material resolution: `GBufferMaterial.slang`
     - Decoupled RT passes: `RTShadows.slang`, `Reflection.slang`
     - Lighting & Composition: `DeferredLighting.slang`
     - Denoising & Upscaling: NRD + DLSS 3.5 / DLSS-RR
  2. **Path Tracer**:
     - Ground-truth / interactive path tracer: `PathTracer.slang`
- **Obsolete / Dead Shaders**:
  - `Material_PBR_Mesh.slang` (and `Material_PBR_MeshTask.slang`, `Material_Unlit_Mesh.slang`) are **old deprecated forward shaders**. Never use, edit, or reference them. All material changes belong in `GBufferMaterial.slang`, `DeferredLighting.slang`, and `PathTracer.slang`.
