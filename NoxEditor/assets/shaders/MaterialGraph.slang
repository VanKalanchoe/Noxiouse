// Material graphs (Renderer/MaterialGraph.h; docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step 4): what the code generated
// from a .nmatgraph reads and writes. Each graph becomes, inside the generated project shader file (CustomLitShaders.slang),
//
//   namespace MaterialGraph<slot> { static const uint Outputs = ...; void surface(MaterialGraphInputs i, inout MaterialGraphOutputs o) }
//
// and material_graph_surface(slot, i, o) calls the one a material names. The caller fills `o` with the material's own values first
// (its .nmat fields and textures): the graph writes only the Material Output pins it connects (Outputs says which).
#pragma once
#include "shaderIO.h"

// What a graph's input nodes read. Derivatives (Dx / Dy: the change across one pixel to the right / down) for what a Texture Sample's UV
// may be computed from: the G-buffer resolve gets them from the triangle's barycentric derivatives, the forward shader from ddx / ddy.
struct MaterialGraphInputs
{
    float2 uv0;
    float2 uv0Dx;
    float2 uv0Dy;
    float2 uv1;
    float2 uv1Dx;
    float2 uv1Dy;
    float3 position;      // world position (the renderer's world units)
    float3 positionDx;
    float3 positionDy;
    float3 normal;        // the vertex normal (world), facing the viewer's side
    float4 vertexColor;
    float3 view;          // towards the camera
    float2 screenUV;      // 0..1 across the view
    float2 screenUVDx;
    float2 screenUVDy;
    float time;           // seconds (UniformBufferObject::timeSeconds)
    float3 cameraPosition;
    float4 parameters[16]; // the material's parameter values (GpuMaterial::graphParameters: overrides, else the graph's defaults)
    uint textures[8];      // their textures (0xFFFFFFFF: none)
    UniformBufferObject* ubo;
};

// What a graph writes: the Material Output pins it connects.
struct MaterialGraphOutputs
{
    float3 baseColor;
    float metallic;
    float roughness;
    float3 normal;     // tangent space (+z out of the surface); the caller applies the surface's frame
    float3 emissive;
    float occlusion;
    float opacity;
};

// A Texture Sample: the material's texture in slot `slot` at `uv`, its mip from the UV's derivatives (times the frame's texture LOD bias,
// as every material texture). No texture: white -- for a normal map, flat. A normal map is decoded to a tangent-space normal in rgb
// (two-channel BC5 maps: z rebuilt), its alpha 1.
float4 material_graph_texture(MaterialGraphInputs i, uint slot, float2 uv, float2 uvDx, float2 uvDy, bool normalMap)
{
    const uint texture = i.textures[min(slot, 7u)];
    if (texture == 0xFFFFFFFFu)
        return normalMap ? float4(0.0, 0.0, 1.0, 1.0) : float4(1.0);
    SamplerState linearRepeat = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    const float scale = exp2(i.ubo->textureLodBias);
    float4 value = SampleMaterialTextureGrad(i.ubo, texture, linearRepeat, uv, uvDx * scale, uvDy * scale);
    if (normalMap)
    {
        float3 n = float3(value.xy * 2.0 - 1.0, 0.0);
        n.z = sqrt(saturate(1.0 - dot(n.xy, n.xy)));
        value = float4(n, 1.0);
    }
    return value;
}

// Smooth value noise in [0, 1] (Noise node).
float material_graph_hash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float material_graph_noise(float3 x)
{
    const float3 cell = floor(x);
    const float3 f = frac(x);
    const float3 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(material_graph_hash(cell + float3(0, 0, 0)), material_graph_hash(cell + float3(1, 0, 0)), u.x),
                     lerp(material_graph_hash(cell + float3(0, 1, 0)), material_graph_hash(cell + float3(1, 1, 0)), u.x), u.y),
                lerp(lerp(material_graph_hash(cell + float3(0, 0, 1)), material_graph_hash(cell + float3(1, 0, 1)), u.x),
                     lerp(material_graph_hash(cell + float3(0, 1, 1)), material_graph_hash(cell + float3(1, 1, 1)), u.x), u.y), u.z);
}
