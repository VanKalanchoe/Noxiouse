// Material graphs (Renderer/MaterialGraph.h; docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step 4): what the code generated
// from a .nmatgraph reads and writes. Each graph becomes, inside the generated project shader file (CustomLitShaders.slang),
//
//   namespace MaterialGraph<slot> { static const uint Outputs = ...; void surface(MaterialGraphInputs i, inout MaterialGraphOutputs o) }
//
// and material_graph_surface(slot, i, o) calls the one a material names. The caller fills `o` with the material's own values first
// (its .nmat fields and textures): the graph writes only the Material Output pins it connects (Outputs says which).
#pragma once
#include "shaderIO.h"
#include "MipFeedback.slang"

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
    GpuMaterial* material; // its graphParameters (the material's overrides, else the graph's defaults) and graphTextureIndex
    bool mipFeedback;      // this pixel reports the mips its textures need (texture streaming: isMipFeedbackPixel)
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
// as every material texture; texture streaming told which mip it needs, as for the material's own textures). No texture: white -- for a
// normal map, flat. A normal map is decoded to a tangent-space normal in rgb
// (two-channel BC5 maps: z rebuilt), its alpha 1.
float4 material_graph_texture(MaterialGraphInputs i, uint slot, float2 uv, float2 uvDx, float2 uvDy, bool normalMap)
{
    const uint texture = i.material.graphTextureIndex[min(slot, 7u)];
    if (texture == 0xFFFFFFFFu)
        return normalMap ? float4(0.0, 0.0, 1.0, 1.0) : float4(1.0);
    if (i.mipFeedback)
        reportMipFeedback(i.ubo, texture, uv, uvDx, uvDy);
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

// The graph's Normal (tangent space, +z out of the surface) in world space: the surface's frame from its normal N and the stored tangent
// (Gram-Schmidt against N, the bitangent cross(N, T) x handedness -- the G-buffer's normal mapping); without a tangent, any frame
// around N.
float3 material_graph_world_normal(float3 tangentNormal, float3 N, float3 tangent, float handedness)
{
    const float3 n = dot(tangentNormal, tangentNormal) > 1e-12 ? normalize(tangentNormal) : float3(0.0, 0.0, 1.0);
    float3 T = tangent - N * dot(N, tangent);
    if (dot(T, T) <= 1e-12)
        T = abs(N.x) < 0.9 ? cross(float3(1.0, 0.0, 0.0), N) : cross(float3(0.0, 1.0, 0.0), N);
    T = normalize(T);
    const float3 B = cross(N, T) * (handedness < 0.0 ? -1.0 : 1.0);
    return normalize(n.x * T + n.y * B + n.z * N);
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
