// The path tracer's push constants (shaderIO.h PushConstantPathTracer) for its ray tracing stages (ray generation, miss, closest
// hit, any hit). A global, not a `uniform` entry-point parameter as the compute / mesh / fragment shaders take theirs: Slang puts a
// ray tracing entry point's uniform parameters in the shader record (ShaderRecordBufferKHR on Vulkan, a local root signature on
// D3D12) -- the bytes after the shader's handle in the shader binding table, which holds none -- so every stage read zeros (a
// viewport of 0 x 0: the ray generation returned at once, the image stayed black). Vulkan: push data ([[vk::push_constant]]);
// D3D12: the global root signature's root constants at b0, space0 (NoxRHI DeviceD3D12, as every shader's push data).
#pragma once
#include "../shaderIO.h"

[[vk::push_constant]]
ConstantBuffer<PushConstantPathTracer> pushConstant : register(b0, space0);
