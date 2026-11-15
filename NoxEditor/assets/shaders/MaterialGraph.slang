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

// --- Opacity Mask in the geometry passes (VisibilityBuffer.slang, EditorStableDepth.slang, SelectionMask.slang; their material graph
// variants): what their vertices add for a graph's inputs, and the alpha test with the graph's mask. One implementation, so the view,
// the shadows and the editor's overlays cut a dissolving surface the same way.

// A vertex's world normal for a graph (Vertex Normal), skinned as its position is: the compute-skinned cache, else the bone matrices.
float3 material_graph_vertex_normal(InstanceData instance, Vertex vert, uint localVertexIndex, PushConstantMeshlets pushConstant)
{
    float3 localNormal = vert.normal;
    if (instance.skinnedVertexOffset != NoSkinnedVertices && pushConstant.skinnedVertexReference != 0)
        localNormal = ((SkinnedVertex*)pushConstant.skinnedVertexReference)[instance.skinnedVertexOffset + localVertexIndex].normal.xyz;
    else if (instance.boneMatrixOffset != 0xFFFFFFFF && pushConstant.boneMatrixReference != 0)
    {
        float4x4* boneMatrices = (float4x4*)pushConstant.boneMatrixReference;
        const uint baseOffset = instance.boneMatrixOffset;
        const float4x4 skinTransform = boneMatrices[baseOffset + vert.boneIDs.x] * vert.boneWeights.x + boneMatrices[baseOffset + vert.boneIDs.y] * vert.boneWeights.y +
                                       boneMatrices[baseOffset + vert.boneIDs.z] * vert.boneWeights.z + boneMatrices[baseOffset + vert.boneIDs.w] * vert.boneWeights.w;
        localNormal = mul((float3x3)skinTransform, localNormal);
    }
    return mul((float3x3)instance.normalMatrix, localNormal);
}

// --- World Position Offset (MaterialGraphOutputWorldPositionOffset): one implementation for every pass that draws the geometry --
// the visibility buffer and the shadow maps (VisibilityBuffer.slang), the G-buffer resolve (GBufferMaterial.slang: the triangle it
// rebuilds must be the one rasterized, and last frame's for the motion vectors), the forward mesh stage (Material_PBR_Mesh.slang) and
// the editor's overlays -- so they all move the vertex the same way.

// Whether the material's graph offsets its vertices.
bool material_graph_has_offset(GpuMaterial* material)
{
    return material.graphSlot != 0xFFFFFFFFu && (material.graphOutputs & MaterialGraphOutputWorldPositionOffset) != 0u;
}

// A vertex's offset (world units) at `time`: `renderPos` its position before the offset (render space), `worldNormal` its normal
// (world), its UVs and vertex colour. The graph's inputs have no screen derivatives here (a Texture Sample reads the finest mip; Unreal
// samples level 0 in vertex shaders too); Camera Vector and Camera Position are the main camera's in every pass (the shadow views use
// the same offset as the view). Clamped to the graph's Max World Position Offset Displacement when it has one (Unreal's).
float3 material_graph_offset(GpuMaterial* material, float3 renderPos, float3 worldNormal, float2 uv0, float2 uv1, float4 vertexColor, float time,
                             UniformBufferObject* ubo)
{
    if (!material_graph_has_offset(material))
        return float3(0.0);
    MaterialGraphInputs i = (MaterialGraphInputs)0;
    i.uv0 = uv0;
    i.uv1 = uv1;
    i.position = renderPos + ubo->renderOrigin;
    i.normal = dot(worldNormal, worldNormal) > 1e-12 ? normalize(worldNormal) : float3(0.0, 0.0, 1.0);
    i.vertexColor = vertexColor;
    const float3 toCamera = ubo->cameraWorldPos.xyz - renderPos;
    i.view = dot(toCamera, toCamera) > 1e-12 ? normalize(toCamera) : float3(0.0, 0.0, 1.0);
    i.screenUV = float2(0.5); // (not available per vertex: the graph compiler refuses Screen Position there)
    i.time = time;
    i.cameraPosition = ubo->cameraWorldPos.xyz + ubo->renderOrigin;
    i.material = material;
    i.mipFeedback = false;
    i.ubo = ubo;
    float3 offset = material_graph_world_position_offset(material.graphSlot, i);
    if (any(isnan(offset)) || any(isinf(offset)))
        return float3(0.0);
    const float limit = material.graphMaxDisplacement;
    const float length2 = dot(offset, offset);
    if (limit > 0.0 && length2 > limit * limit)
        offset *= limit * rsqrt(length2);
    return offset;
}

// The same for a vertex of a meshlet in a mesh stage: its normal (skinned as its position is) and colour are read only when the
// material offsets vertices.
float3 material_graph_vertex_offset(GpuMaterial* material, InstanceData instance, Vertex vert, uint localVertexIndex, uint globalVertexIndex,
                                    float3 renderPos, UniformBufferObject* ubo, PushConstantMeshlets pushConstant)
{
    if (!material_graph_has_offset(material))
        return float3(0.0);
    return material_graph_offset(material, renderPos, material_graph_vertex_normal(instance, vert, localVertexIndex, pushConstant), vert.uv0, vert.uv1,
                                 loadMeshVertexColor(ubo, globalVertexIndex, instance.vertexColors), ubo->timeSeconds, ubo);
}

// Whether a Masked material whose graph connects Opacity Mask has this pixel cut away (its mask below its Opacity Mask Clip Value);
// `handled` false: the material has no graph mask (the pass tests its base colour's alpha as before). The graph's inputs from the
// interpolated vertex values (render-space `worldPos`), their derivatives from this pixel's quad; `viewProj`: the pass's projection
// (Screen Position). Called where every pixel of a quad runs (the derivatives), with a draw's material: the same for the whole quad.
bool material_graph_mask_cuts(float2 uv0, float2 uv1, float3 worldPos, float3 normal, float4 vertexColor, bool isFrontFace, float4x4 viewProj,
                              UniformBufferObject* ubo, GpuMaterial* material, InstanceData instance, out bool handled)
{
    handled = instance.alphaMode == 1 && material.graphSlot != 0xFFFFFFFFu && (material.graphOutputs & MaterialGraphOutputOpacityMask) != 0u;
    MaterialGraphInputs i;
    i.uv0 = uv0;
    i.uv0Dx = ddx(uv0);
    i.uv0Dy = ddy(uv0);
    i.uv1 = uv1;
    i.uv1Dx = ddx(uv1);
    i.uv1Dy = ddy(uv1);
    i.position = worldPos + ubo->renderOrigin;
    i.positionDx = ddx(worldPos);
    i.positionDy = ddy(worldPos);
    const float3 n = normalize(normal);
    i.normal = instance.doubleSided != 0 && !isFrontFace ? -n : n;
    i.vertexColor = vertexColor;
    i.view = normalize(ubo->cameraWorldPos.xyz - worldPos);
    const float4 clip = mul(viewProj, float4(worldPos, 1.0));
    i.screenUV = clip.w > 1e-6 ? float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) : float2(0.5);
    i.screenUVDx = ddx(i.screenUV);
    i.screenUVDy = ddy(i.screenUV);
    i.time = ubo->timeSeconds;
    i.cameraPosition = ubo->cameraWorldPos.xyz + ubo->renderOrigin;
    i.material = material;
    i.mipFeedback = false; // (the G-buffer resolve reports the textures' mips)
    i.ubo = ubo;
    if (!handled)
        return false;
    return material_graph_opacity_mask(material.graphSlot, i) < instance.alphaMaskCutoff;
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
