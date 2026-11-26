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
    float3 pixelNormal;   // the shading normal (world, after normal map; step 4h)
    float pixelDepth;     // linear view-space depth of this pixel in world units (step 4h)
    float sceneDepth;     // linear view-space depth of the opaque scene behind this pixel in world units (step 4h)
    // The vertex in the mesh's own space before skinning and morph targets (Unreal's Pre-Skinned Local Position): a pattern on it stays on
    // an animated character. The mesh's units (its import scale applied).
    float3 localPosition;
    float3 localPositionDx;
    float3 localPositionDy;
    float4 tangent;       // the vertex tangent (world, skinned as the normal is; 0 where the mesh has none), w the bitangent's handedness
    float4x4 objectToWorld;      // instance model matrix (render space; world pivot = column 3 + ubo.renderOrigin)
    float4x4 objectNormalMatrix; // transpose(inverse(objectToWorld)); transpose((float3x3)) is inverse 3x3
    float3 objectBoundsMin;      // mesh local-space AABB min
    float3 objectBoundsMax;      // mesh local-space AABB max
    float4 vertexInterp[4];      // Vertex Interpolator slots 0..3 (step 4f)
    // VFX Input Nodes (VFX_And_Volumes_Plan_2026-11.md step V3b): populated by VfxMeshParticle / VfxRibbonBeam.
    uint hasParticleData;        // 1: particleColor / ribbonUV are live VFX values; 0: regular mesh defaults
    float particleAge;           // 0..1 normalized lifetime
    float particleRandom;        // 0..1 per-particle seed
    float3 particleVelocity;     // world units / second
    float4 particleColor;        // linear HDR RGBA
    float2 ribbonUV;             // u along length, v across width
    float4 vertexInterpDx[4];
    float4 vertexInterpDy[4];
    float4 vertexColor;
    float3 view;          // towards the camera
    float2 screenUV;      // 0..1 across the view
    float2 screenUVDx;
    float2 screenUVDy;
    float time;           // seconds (UniformBufferObject::timeSeconds)
    float3 cameraPosition;
    GpuMaterial material; // its graphParameters (the material's overrides, else the graph's defaults) and graphTextureIndex
    bool mipFeedback;      // this pixel reports the mips its textures need (texture streaming: isMipFeedbackPixel)
    UniformBufferObject ubo;
};

// What a graph writes per vertex (step 4f: Vertex Normal / Vertex Color overrides and up to 4 Vertex Interpolators).
struct MaterialGraphVertexOutputs
{
    float3 normal;       // world space (initialized to the vertex's own normal)
    float4 color;        // vertex colour (initialized to the vertex's own colour)
    float4 interp[4];    // Vertex Interpolator slots 0..3
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
    // Pixel-stage lobe outputs (step 4g): written over the material's own lobe values.
    float specular;
    float3 specularColor;
    float clearCoat;
    float clearCoatRoughness;
    float3 clearCoatNormal; // tangent space (+z out of the surface)
    float3 sheenColor;
    float sheenRoughness;
    float anisotropy;
    float2 anisotropyDirection; // tangent space
    float transmission;
    float ior;
    float thickness;
    float3 attenuationColor;
    float attenuationDistance;
    float diffuseTransmission;
    float iridescence;
    float iridescenceThickness; // nanometers
    float dispersion;
    // Extended outputs (step R1+):
    float3 subsurfaceColor;
    float aoLightAffect;
};

// Applies a material graph's connected lobe outputs (step 4g) over the evaluated GBufferLobes and diffuseTransmission.
void material_graph_apply_lobes(uint outputs, MaterialGraphOutputs graphOut, GpuMaterial material,
                                float3 geomNormal, float3 rawTangent, float tangentSign, bool backFace, float3 viewDir,
                                inout GBufferLobes lobes, inout float3 anisotropicT, inout float diffuseTransmission)
{
    if ((outputs & MaterialGraphOutputAllLobes) == 0u)
        return;

    const float ior = (outputs & MaterialGraphOutputIOR) != 0u ? max(graphOut.ior, 1.0) : (material.ior > 0.0 ? material.ior : 1.5);
    const float iorF0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
    if ((outputs & (MaterialGraphOutputSpecular | MaterialGraphOutputSpecularColor | MaterialGraphOutputIOR)) != 0u)
    {
        if ((lobes.flags & MaterialLobeSpecular) == 0u)
        {
            lobes.dielectricF0 = min(iorF0 * material.specularColorFactor, float3(1.0));
            lobes.specularWeight = material.specularStrength;
        }
        if ((outputs & MaterialGraphOutputSpecular) != 0u)
            lobes.specularWeight = saturate(graphOut.specular);
        if ((outputs & MaterialGraphOutputSpecularColor) != 0u)
            lobes.dielectricF0 = min(iorF0 * saturate(graphOut.specularColor), float3(1.0));
        else if ((outputs & MaterialGraphOutputIOR) != 0u)
            lobes.dielectricF0 = min(iorF0 * material.specularColorFactor, float3(1.0));
        lobes.flags |= MaterialLobeSpecular;
    }

    if ((outputs & (MaterialGraphOutputClearCoat | MaterialGraphOutputClearCoatRoughness | MaterialGraphOutputClearCoatNormal)) != 0u)
    {
        if ((lobes.flags & MaterialLobeClearcoat) == 0u)
        {
            lobes.coat = material.clearcoatFactor;
            lobes.coatRoughness = material.clearcoatRoughnessFactor;
            lobes.coatNormal = backFace ? -geomNormal : geomNormal;
        }
        if ((outputs & MaterialGraphOutputClearCoat) != 0u)
            lobes.coat = saturate(graphOut.clearCoat);
        if ((outputs & MaterialGraphOutputClearCoatRoughness) != 0u)
            lobes.coatRoughness = saturate(graphOut.clearCoatRoughness);
        if ((outputs & MaterialGraphOutputClearCoatNormal) != 0u)
        {
            float3 cn = material_graph_world_normal(graphOut.clearCoatNormal, geomNormal, rawTangent, tangentSign);
            lobes.coatNormal = backFace ? -cn : cn;
        }
        if (lobes.coat > 0.0)
        {
            lobes.flags |= MaterialLobeClearcoat;
            const float cnDotV = dot(lobes.coatNormal, viewDir);
            if (cnDotV < 0.01)
                lobes.coatNormal = normalize(lobes.coatNormal + viewDir * (0.01 - cnDotV));
        }
        else
            lobes.flags &= ~MaterialLobeClearcoat;
    }

    if ((outputs & (MaterialGraphOutputSheenColor | MaterialGraphOutputSheenRoughness)) != 0u)
    {
        if ((lobes.flags & MaterialLobeSheen) == 0u)
        {
            lobes.sheenColor = material.sheenColorFactor;
            lobes.sheenRoughness = material.sheenRoughnessFactor;
        }
        if ((outputs & MaterialGraphOutputSheenColor) != 0u)
            lobes.sheenColor = saturate(graphOut.sheenColor);
        if ((outputs & MaterialGraphOutputSheenRoughness) != 0u)
            lobes.sheenRoughness = saturate(graphOut.sheenRoughness);
        if (max(max(lobes.sheenColor.r, lobes.sheenColor.g), lobes.sheenColor.b) > 0.0)
            lobes.flags |= MaterialLobeSheen;
        else
            lobes.flags &= ~MaterialLobeSheen;
    }

    if ((outputs & (MaterialGraphOutputIridescence | MaterialGraphOutputIridescenceThickness)) != 0u)
    {
        if ((lobes.flags & MaterialLobeIridescence) == 0u)
        {
            lobes.iridescence = material.iridescenceFactor;
            lobes.iridescenceIor = material.iridescenceIor > 0.0 ? material.iridescenceIor : 1.3;
            lobes.iridescenceThickness = material.iridescenceThicknessMaximum > 0.0 ? material.iridescenceThicknessMaximum : 400.0;
        }
        if ((outputs & MaterialGraphOutputIridescence) != 0u)
            lobes.iridescence = saturate(graphOut.iridescence);
        if ((outputs & MaterialGraphOutputIridescenceThickness) != 0u)
            lobes.iridescenceThickness = max(graphOut.iridescenceThickness, 0.0);
        if (lobes.iridescence > 0.0)
            lobes.flags |= MaterialLobeIridescence;
        else
            lobes.flags &= ~MaterialLobeIridescence;
    }

    if ((outputs & (MaterialGraphOutputAnisotropy | MaterialGraphOutputAnisotropyDirection)) != 0u)
    {
        const bool hasTangents = dot(rawTangent, rawTangent) > 1e-12;
        if ((outputs & MaterialGraphOutputAnisotropy) != 0u)
            lobes.anisotropy = hasTangents ? saturate(graphOut.anisotropy) : 0.0;
        else if ((lobes.flags & MaterialLobeAnisotropy) == 0u)
            lobes.anisotropy = hasTangents ? material.anisotropyStrength : 0.0;
        if (hasTangents && ((outputs & MaterialGraphOutputAnisotropyDirection) != 0u || (lobes.flags & MaterialLobeAnisotropy) == 0u))
        {
            const float2 rawDir = (outputs & MaterialGraphOutputAnisotropyDirection) != 0u ? graphOut.anisotropyDirection : material.anisotropyDirection;
            const float2 dir = dot(rawDir, rawDir) > 1e-8 ? normalize(rawDir) : float2(1.0, 0.0);
            const float3 T = normalize(rawTangent - geomNormal * dot(geomNormal, rawTangent));
            const float3 B = cross(geomNormal, T) * (tangentSign < 0.0 ? -1.0 : 1.0);
            anisotropicT = normalize(T * dir.x + B * dir.y);
            if (backFace)
                anisotropicT = -anisotropicT;
        }
        if (lobes.anisotropy > 0.0 && hasTangents)
            lobes.flags |= MaterialLobeAnisotropy;
        else
            lobes.flags &= ~MaterialLobeAnisotropy;
    }

    if ((outputs & MaterialGraphOutputDiffuseTransmission) != 0u)
        diffuseTransmission = saturate(graphOut.diffuseTransmission);
}

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
    const float scale = exp2(i.ubo.textureLodBias);
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
        localNormal = BufferRef<SkinnedVertex>(pushConstant.skinnedVertexReference)[instance.skinnedVertexOffset + localVertexIndex].normal.xyz;
    else if (instance.boneMatrixOffset != 0xFFFFFFFF && pushConstant.boneMatrixReference != 0)
    {
        BufferRef<float4x4> boneMatrices = BufferRef<float4x4>(pushConstant.boneMatrixReference);
        const uint baseOffset = instance.boneMatrixOffset;
        const float4x4 skinTransform = boneMatrices[baseOffset + vert.boneIDs.x] * vert.boneWeights.x + boneMatrices[baseOffset + vert.boneIDs.y] * vert.boneWeights.y +
                                       boneMatrices[baseOffset + vert.boneIDs.z] * vert.boneWeights.z + boneMatrices[baseOffset + vert.boneIDs.w] * vert.boneWeights.w;
        localNormal = mul((float3x3)skinTransform, localNormal);
    }
    return mul((float3x3)instance.normalMatrix, localNormal);
}

// A vertex's world tangent for a graph (Vertex Tangent / Bitangent), skinned as its normal is; w the bitangent's handedness. Along the
// surface: through the model matrix itself, not the normal matrix (as GBufferMaterial.slang).
float4 material_graph_vertex_tangent(InstanceData instance, Vertex vert, uint localVertexIndex, PushConstantMeshlets pushConstant)
{
    float3 localTangent = vert.tangent.xyz;
    if (instance.skinnedVertexOffset != NoSkinnedVertices && pushConstant.skinnedVertexReference != 0)
        localTangent = BufferRef<SkinnedVertex>(pushConstant.skinnedVertexReference)[instance.skinnedVertexOffset + localVertexIndex].tangent.xyz;
    else if (instance.boneMatrixOffset != 0xFFFFFFFF && pushConstant.boneMatrixReference != 0)
    {
        BufferRef<float4x4> boneMatrices = BufferRef<float4x4>(pushConstant.boneMatrixReference);
        const uint baseOffset = instance.boneMatrixOffset;
        const float4x4 skinTransform = boneMatrices[baseOffset + vert.boneIDs.x] * vert.boneWeights.x + boneMatrices[baseOffset + vert.boneIDs.y] * vert.boneWeights.y +
                                       boneMatrices[baseOffset + vert.boneIDs.z] * vert.boneWeights.z + boneMatrices[baseOffset + vert.boneIDs.w] * vert.boneWeights.w;
        localTangent = mul((float3x3)skinTransform, localTangent);
    }
    return float4(mul((float3x3)instance.modelMatrix, localTangent), vert.tangent.w);
}

// Whether the material's graph has any per-vertex outputs (step 4f: Vertex Normal, Vertex Color, or Vertex Interpolators).
bool material_graph_has_vertex_outputs(GpuMaterial material)
{
    return material.graphSlot != 0xFFFFFFFFu &&
           (material.graphOutputs & (MaterialGraphOutputVertexNormal | MaterialGraphOutputVertexColor | MaterialGraphOutputVertexInterpolators)) != 0u;
}

// Evaluates per-vertex outputs (Vertex Normal, Vertex Color, Vertex Interpolators) at one vertex.
MaterialGraphVertexOutputs material_graph_eval_vertex(
    GpuMaterial material, float3 renderPos, float3 worldNormal, float2 uv0, float2 uv1, float4 vertexColor, float time,
    UniformBufferObject ubo, float3 localPosition, float4 worldTangent,
    float4x4 objectToWorld, float4x4 objectNormalMatrix, float3 objectBoundsMin, float3 objectBoundsMax)
{
    MaterialGraphVertexOutputs vo = (MaterialGraphVertexOutputs)0;
    vo.normal = dot(worldNormal, worldNormal) > 1e-12 ? normalize(worldNormal) : float3(0.0, 0.0, 1.0);
    vo.color = vertexColor;
    if (!material_graph_has_vertex_outputs(material))
        return vo;
    MaterialGraphInputs i = (MaterialGraphInputs)0;
    i.uv0 = uv0;
    i.uv1 = uv1;
    i.position = renderPos + ubo.renderOrigin;
    i.normal = vo.normal;
    i.localPosition = localPosition;
    i.tangent = worldTangent;
    i.objectToWorld = objectToWorld;
    i.objectNormalMatrix = objectNormalMatrix;
    i.objectBoundsMin = objectBoundsMin;
    i.objectBoundsMax = objectBoundsMax;
    i.vertexColor = vertexColor;
    const float3 toCamera = ubo.cameraWorldPos.xyz - renderPos;
    i.view = dot(toCamera, toCamera) > 1e-12 ? normalize(toCamera) : float3(0.0, 0.0, 1.0);
    i.screenUV = float2(0.5);
    i.time = time;
    i.cameraPosition = ubo.cameraWorldPos.xyz + ubo.renderOrigin;
    i.material = material;
    i.mipFeedback = false;
    i.ubo = ubo;
    material_graph_vertex_outputs(material.graphSlot, i, vo);
    return vo;
}

// The raster passes' helpers (World Position Offset, the Opacity Mask's pixel test): not in the ray tracing variants, whose generated file
// (MaterialGraphMasks.slang) has the graphs' masks alone (NOX_MATERIAL_GRAPH_MASK_ONLY).
#ifndef NOX_MATERIAL_GRAPH_MASK_ONLY
// --- World Position Offset (MaterialGraphOutputWorldPositionOffset): one implementation for every pass that draws the geometry --
// the visibility buffer and the shadow maps (VisibilityBuffer.slang), the G-buffer resolve (GBufferMaterial.slang: the triangle it
// rebuilds must be the one rasterized, and last frame's for the motion vectors), the forward mesh stage (Material_PBR_Mesh.slang) and
// the editor's overlays -- so they all move the vertex the same way.

// Whether the material's graph offsets its vertices.
bool material_graph_has_offset(GpuMaterial material)
{
    return material.graphSlot != 0xFFFFFFFFu && (material.graphOutputs & MaterialGraphOutputWorldPositionOffset) != 0u;
}

// A vertex's offset (world units) at `time`: `renderPos` its position before the offset (render space), `worldNormal` its normal
// (world), its UVs and vertex colour. The graph's inputs have no screen derivatives here (a Texture Sample reads the finest mip; Unreal
// samples level 0 in vertex shaders too); Camera Vector and Camera Position are the main camera's in every pass (the shadow views use
// the same offset as the view). Clamped to the graph's Max World Position Offset Displacement when it has one (Unreal's).
float3 material_graph_offset(GpuMaterial material, float3 renderPos, float3 worldNormal, float2 uv0, float2 uv1, float4 vertexColor, float time,
                             UniformBufferObject ubo, float3 localPosition, float4 worldTangent,
                             float4x4 objectToWorld, float4x4 objectNormalMatrix, float3 objectBoundsMin, float3 objectBoundsMax)
{
    if (!material_graph_has_offset(material))
        return float3(0.0);
    MaterialGraphInputs i = (MaterialGraphInputs)0;
    i.uv0 = uv0;
    i.uv1 = uv1;
    i.position = renderPos + ubo.renderOrigin;
    i.normal = dot(worldNormal, worldNormal) > 1e-12 ? normalize(worldNormal) : float3(0.0, 0.0, 1.0);
    i.localPosition = localPosition;
    i.tangent = worldTangent;
    i.objectToWorld = objectToWorld;
    i.objectNormalMatrix = objectNormalMatrix;
    i.objectBoundsMin = objectBoundsMin;
    i.objectBoundsMax = objectBoundsMax;
    i.vertexColor = vertexColor;
    const float3 toCamera = ubo.cameraWorldPos.xyz - renderPos;
    i.view = dot(toCamera, toCamera) > 1e-12 ? normalize(toCamera) : float3(0.0, 0.0, 1.0);
    i.screenUV = float2(0.5); // (not available per vertex: the graph compiler refuses Screen Position there)
    i.time = time;
    i.cameraPosition = ubo.cameraWorldPos.xyz + ubo.renderOrigin;
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
float3 material_graph_vertex_offset(GpuMaterial material, InstanceData instance, Vertex vert, uint localVertexIndex, uint globalVertexIndex,
                                    float3 renderPos, UniformBufferObject ubo, PushConstantMeshlets pushConstant)
{
    if (!material_graph_has_offset(material))
        return float3(0.0);
    return material_graph_offset(material, renderPos, material_graph_vertex_normal(instance, vert, localVertexIndex, pushConstant), vert.uv0, vert.uv1,
                                 loadMeshVertexColor(ubo, globalVertexIndex, instance.vertexColors), ubo.timeSeconds, ubo, vert.pos,
                                 material_graph_vertex_tangent(instance, vert, localVertexIndex, pushConstant),
                                 instance.modelMatrix, instance.normalMatrix, instance.boundsMin, instance.boundsMax);
}

// Whether a Masked material whose graph connects Opacity Mask has this pixel cut away (its mask below its Opacity Mask Clip Value);
// `handled` false: the material has no graph mask (the pass tests its base colour's alpha as before). The graph's inputs from the
// interpolated vertex values (render-space `worldPos`), their derivatives from this pixel's quad; `viewProj`: the pass's projection
// (Screen Position). Called where every pixel of a quad runs (the derivatives), with a draw's material: the same for the whole quad.
bool material_graph_mask_cuts(float2 uv0, float2 uv1, float3 worldPos, float3 normal, float4 vertexColor, bool isFrontFace, float4x4 viewProj,
                              UniformBufferObject ubo, GpuMaterial material, InstanceData instance, out bool handled, float3 localPosition,
                              float4 tangent)
{
    handled = instance.alphaMode == 1 && material.graphSlot != 0xFFFFFFFFu && (material.graphOutputs & MaterialGraphOutputOpacityMask) != 0u;
    MaterialGraphInputs i;
    i.uv0 = uv0;
    i.uv0Dx = ddx(uv0);
    i.uv0Dy = ddy(uv0);
    i.uv1 = uv1;
    i.uv1Dx = ddx(uv1);
    i.uv1Dy = ddy(uv1);
    i.position = worldPos + ubo.renderOrigin;
    i.positionDx = ddx(worldPos);
    i.positionDy = ddy(worldPos);
    const float3 n = normalize(normal);
    i.normal = instance.doubleSided != 0 && !isFrontFace ? -n : n;
    i.localPosition = localPosition;
    i.localPositionDx = ddx(localPosition);
    i.localPositionDy = ddy(localPosition);
    i.tangent = tangent;
    i.objectToWorld = instance.modelMatrix;
    i.objectNormalMatrix = instance.normalMatrix;
    i.objectBoundsMin = instance.boundsMin;
    i.objectBoundsMax = instance.boundsMax;
    i.vertexColor = vertexColor;
    i.view = normalize(ubo.cameraWorldPos.xyz - worldPos);
    const float4 clip = mul(viewProj, float4(worldPos, 1.0));
    i.screenUV = clip.w > 1e-6 ? float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) : float2(0.5);
    i.screenUVDx = ddx(i.screenUV);
    i.screenUVDy = ddy(i.screenUV);
    i.time = ubo.timeSeconds;
    i.cameraPosition = ubo.cameraWorldPos.xyz + ubo.renderOrigin;
    i.material = material;
    i.mipFeedback = false; // (the G-buffer resolve reports the textures' mips)
    i.ubo = ubo;
    if (!handled)
        return false;
    return material_graph_opacity_mask(material.graphSlot, i) < instance.alphaMaskCutoff;
}

// Pixel Depth Offset (step R2, Unreal's Pixel Depth Offset): pushes the pixel further away along the view ray by >= 0 world units
// and returns the new Reverse-Z depth (<= baseDepth, compatible with SV_DepthLessEqual). Returns baseDepth unchanged when unused.
float material_graph_apply_pdo(float2 uv0, float2 uv1, float3 worldPos, float3 normal, float4 vertexColor, bool isFrontFace,
                               float4x4 viewProj, UniformBufferObject ubo, GpuMaterial material, InstanceData instance,
                               float3 localPosition, float4 tangent, float baseDepth)
{
    const bool hasPdo = material.graphSlot != 0xFFFFFFFFu && (material.graphOutputs2 & MaterialGraphOutput2PixelDepthOffset) != 0u;
    MaterialGraphInputs i = (MaterialGraphInputs)0;
    i.uv0 = uv0;
    i.uv0Dx = ddx(uv0);
    i.uv0Dy = ddy(uv0);
    i.uv1 = uv1;
    i.uv1Dx = ddx(uv1);
    i.uv1Dy = ddy(uv1);
    i.position = worldPos + ubo.renderOrigin;
    i.positionDx = ddx(worldPos);
    i.positionDy = ddy(worldPos);
    const float3 n = normalize(normal);
    i.normal = instance.doubleSided != 0 && !isFrontFace ? -n : n;
    i.pixelNormal = i.normal;
    i.localPosition = localPosition;
    i.localPositionDx = ddx(localPosition);
    i.localPositionDy = ddy(localPosition);
    i.tangent = tangent;
    i.objectToWorld = instance.modelMatrix;
    i.objectNormalMatrix = instance.normalMatrix;
    i.objectBoundsMin = instance.boundsMin;
    i.objectBoundsMax = instance.boundsMax;
    i.vertexColor = vertexColor;
    i.view = normalize(ubo.cameraWorldPos.xyz - worldPos);
    const float4 clip = mul(viewProj, float4(worldPos, 1.0));
    i.screenUV = clip.w > 1e-6 ? float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5) : float2(0.5);
    i.screenUVDx = ddx(i.screenUV);
    i.screenUVDy = ddy(i.screenUV);
    i.pixelDepth = abs(mul(ubo.view, float4(worldPos, 1.0)).z);
    i.sceneDepth = i.pixelDepth;
    i.time = ubo.timeSeconds;
    i.cameraPosition = ubo.cameraWorldPos.xyz + ubo.renderOrigin;
    i.material = material;
    i.mipFeedback = false;
    i.ubo = ubo;
    if (!hasPdo)
        return baseDepth;
    const float pdo = max(material_graph_pixel_depth_offset(material.graphSlot, i), 0.0);
    if (pdo <= 0.0)
        return baseDepth;
    const float3 pushedPos = worldPos - i.view * pdo;
    const float4 pushedClip = mul(viewProj, float4(pushedPos, 1.0));
    if (pushedClip.w <= 1e-6)
        return 0.0;
    return min(baseDepth, max(pushedClip.z / pushedClip.w, 0.0));
}

#endif // NOX_MATERIAL_GRAPH_MASK_ONLY

// --- The graphs at a ray hit: the Opacity Mask (NOX_MATERIAL_GRAPH_MASK: shaderIO.h's RayHitAlpha / RayHitCoverage in the ray tracing
// passes' graph variants, and the forward shader's ray-traced shadows) and the surface (NOX_MATERIAL_GRAPH_SURFACE: the passes that shade
// their hits). The graph's inputs rebuilt from the hit triangle -- its corners as the ray tracing scene holds them (LoadRayTracingVertex:
// skinned, unmoved by World Position Offset), the UVs, the vertex normal (world), the vertex colour, the world position through the
// instance's transform. No screen derivatives (a Texture Sample reads the finest mip), Camera Vector towards the main camera, Screen
// Position the view's centre.
#if defined(NOX_MATERIAL_GRAPH_MASK) || defined(NOX_MATERIAL_GRAPH_SURFACE)
MaterialGraphInputs material_graph_ray_inputs(UniformBufferObject ubo, GpuRayTracingInstance instance, GpuMaterial material, uint primitive,
                                              float2 barycentrics)
{
    const uint3 corners = RayTracingTriangle(instance, primitive);
    const Vertex v0 = LoadRayTracingVertex(instance, corners.x);
    const Vertex v1 = LoadRayTracingVertex(instance, corners.y);
    const Vertex v2 = LoadRayTracingVertex(instance, corners.z);
    const float3 w = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
    const float3 localPosition = v0.pos * w.x + v1.pos * w.y + v2.pos * w.z;
    const float4x4 world = BufferRef<GpuTransform>(ubo.sceneTransformsReference)[instance.instanceSlot].world;
    const float3 position = mul(world, float4(localPosition, 1.0)).xyz; // (render space)
    const float3 normal = mul((float3x3)instance.normalMatrix, v0.normal * w.x + v1.normal * w.y + v2.normal * w.z);
    MaterialGraphInputs i = (MaterialGraphInputs)0;
    i.uv0 = v0.uv0 * w.x + v1.uv0 * w.y + v2.uv0 * w.z;
    i.uv1 = v0.uv1 * w.x + v1.uv1 * w.y + v2.uv1 * w.z;
    i.position = position + ubo.renderOrigin;
    i.normal = dot(normal, normal) > 1e-12 ? normalize(normal) : float3(0.0, 0.0, 1.0);
    // Local Position: the mesh's own vertices (before skinning, morphs and World Position Offset), not the ray tracing scene's.
    BufferRef<PackedVertex> meshVertices = BufferRef<PackedVertex>(instance.vertexBufferAddress);
    i.localPosition = unpackVertex(meshVertices[corners.x]).pos * w.x + unpackVertex(meshVertices[corners.y]).pos * w.y +
                      unpackVertex(meshVertices[corners.z]).pos * w.z;
    // The tangent as the hit's vertices have it (a skinned or moved instance's from its deformed vertices), through the instance's
    // transform (a tangent runs along the surface: the model matrix, not the normal matrix).
    float3 localTangent = v0.tangent.xyz * w.x + v1.tangent.xyz * w.y + v2.tangent.xyz * w.z;
    if (instance.skinnedVertexBufferAddress != 0)
    {
        BufferRef<SkinnedVertex> deformed = BufferRef<SkinnedVertex>(instance.skinnedVertexBufferAddress);
        localTangent = deformed[corners.x].tangent.xyz * w.x + deformed[corners.y].tangent.xyz * w.y + deformed[corners.z].tangent.xyz * w.z;
    }
    i.tangent = float4(mul((float3x3)world, localTangent), v0.tangent.w);
    i.objectToWorld = world;
    i.objectNormalMatrix = instance.normalMatrix;
    const uint meshIndex = BufferRef<GpuInstance>(ubo.sceneInstancesReference)[instance.instanceSlot].meshIndex;
    const GpuMesh gpuMesh = BufferRef<GpuMesh>(ubo.sceneMeshesReference)[meshIndex];
    i.objectBoundsMin = gpuMesh.boundsMin;
    i.objectBoundsMax = gpuMesh.boundsMax;
    i.vertexColor = RayTracingVertexColor(instance, corners, barycentrics);
    if (material_graph_has_vertex_outputs(material))
    {
        const float3 p0 = mul(world, float4(v0.pos, 1.0)).xyz;
        const float3 p1 = mul(world, float4(v1.pos, 1.0)).xyz;
        const float3 p2 = mul(world, float4(v2.pos, 1.0)).xyz;
        const float3 n0 = mul((float3x3)instance.normalMatrix, v0.normal);
        const float3 n1 = mul((float3x3)instance.normalMatrix, v1.normal);
        const float3 n2 = mul((float3x3)instance.normalMatrix, v2.normal);
        const float4 t0 = float4(mul((float3x3)world, v0.tangent.xyz), v0.tangent.w);
        const float4 t1 = float4(mul((float3x3)world, v1.tangent.xyz), v1.tangent.w);
        const float4 t2 = float4(mul((float3x3)world, v2.tangent.xyz), v2.tangent.w);
        const float3 lp0 = unpackVertex(meshVertices[corners.x]).pos;
        const float3 lp1 = unpackVertex(meshVertices[corners.y]).pos;
        const float3 lp2 = unpackVertex(meshVertices[corners.z]).pos;
        const float4 c0 = RayTracingVertexColor(instance, uint3(corners.x, corners.x, corners.x), float2(0.0, 0.0));
        const float4 c1 = RayTracingVertexColor(instance, uint3(corners.y, corners.y, corners.y), float2(0.0, 0.0));
        const float4 c2 = RayTracingVertexColor(instance, uint3(corners.z, corners.z, corners.z), float2(0.0, 0.0));
        const MaterialGraphVertexOutputs vo0 = material_graph_eval_vertex(material, p0, n0, v0.uv0, v0.uv1, c0, ubo.timeSeconds, ubo, lp0, t0, world, instance.normalMatrix, gpuMesh.boundsMin, gpuMesh.boundsMax);
        const MaterialGraphVertexOutputs vo1 = material_graph_eval_vertex(material, p1, n1, v1.uv0, v1.uv1, c1, ubo.timeSeconds, ubo, lp1, t1, world, instance.normalMatrix, gpuMesh.boundsMin, gpuMesh.boundsMax);
        const MaterialGraphVertexOutputs vo2 = material_graph_eval_vertex(material, p2, n2, v2.uv0, v2.uv1, c2, ubo.timeSeconds, ubo, lp2, t2, world, instance.normalMatrix, gpuMesh.boundsMin, gpuMesh.boundsMax);
        if ((material.graphOutputs & MaterialGraphOutputVertexNormal) != 0u)
        {
            const float3 vn = vo0.normal * w.x + vo1.normal * w.y + vo2.normal * w.z;
            if (dot(vn, vn) > 1e-12)
                i.normal = normalize(vn);
        }
        if ((material.graphOutputs & MaterialGraphOutputVertexColor) != 0u)
            i.vertexColor = vo0.color * w.x + vo1.color * w.y + vo2.color * w.z;
        if ((material.graphOutputs & MaterialGraphOutputVertexInterpolators) != 0u)
        {
            [unroll]
            for (uint k = 0u; k < 4u; ++k)
                i.vertexInterp[k] = vo0.interp[k] * w.x + vo1.interp[k] * w.y + vo2.interp[k] * w.z;
        }
    }
    const float3 toCamera = ubo.cameraWorldPos.xyz - position;
    i.pixelNormal = i.normal;
    i.pixelDepth = abs(mul(ubo.view, float4(position, 1.0)).z);
    i.sceneDepth = i.pixelDepth;
    i.view = dot(toCamera, toCamera) > 1e-12 ? normalize(toCamera) : float3(0.0, 0.0, 1.0);
    i.screenUV = float2(0.5);
    i.time = ubo.timeSeconds;
    i.cameraPosition = ubo.cameraWorldPos.xyz + ubo.renderOrigin;
    i.material = material;
    i.mipFeedback = false;
    i.ubo = ubo;
    return i;
}
#endif

#ifdef NOX_MATERIAL_GRAPH_MASK
float material_graph_ray_mask(UniformBufferObject ubo, GpuRayTracingInstance instance, uint primitive, float2 barycentrics, out bool handled)
{
    handled = false;
    if (instance.alphaMode != 1)
        return 1.0;
    const GpuMaterial material = BufferRef<GpuMaterial>(ubo.sceneMaterialsReference)[instance.materialIndex];
    if (material.graphSlot == 0xFFFFFFFFu || (material.graphOutputs & MaterialGraphOutputOpacityMask) == 0u)
        return 1.0;
    handled = true;
    return material_graph_opacity_mask(material.graphSlot, material_graph_ray_inputs(ubo, instance, material, primitive, barycentrics));
}
#endif

#ifdef NOX_MATERIAL_GRAPH_SURFACE
// A ray hit's surface with its material's graph, as the G-buffer resolve applies it (GBufferMaterial.slang): `o` holds the hit's own values
// on entry (its material's factors and textures; normal (0, 0, 1)), the graph writes the Material Output pins it connects -- base colour,
// metallic, roughness and occlusion saturated, emissive at least 0, the normal in tangent space (the caller puts it in its frame:
// material_graph_world_normal). Returns which pins (MaterialGraphOutput* bits; 0: no graph, `o` as it was). Opacity is the forward
// shader's (a translucent hit keeps its material's).
uint material_graph_ray_surface(UniformBufferObject ubo, GpuRayTracingInstance instance, uint primitive, float2 barycentrics, inout MaterialGraphOutputs o)
{
    const GpuMaterial material = BufferRef<GpuMaterial>(ubo.sceneMaterialsReference)[instance.materialIndex];
    if (material.graphSlot == 0xFFFFFFFFu)
        return 0u;
    const uint outputs = material.graphOutputs & (MaterialGraphOutputBaseColor | MaterialGraphOutputMetallic | MaterialGraphOutputRoughness |
                                                  MaterialGraphOutputNormal | MaterialGraphOutputEmissive | MaterialGraphOutputOcclusion |
                                                  MaterialGraphOutputAllLobes);
    if (outputs == 0u)
        return 0u;
    MaterialGraphOutputs graph = o;
    material_graph_surface(material.graphSlot, material_graph_ray_inputs(ubo, instance, material, primitive, barycentrics), graph);
    if ((outputs & MaterialGraphOutputBaseColor) != 0u)
        o.baseColor = saturate(graph.baseColor);
    if ((outputs & MaterialGraphOutputMetallic) != 0u)
        o.metallic = saturate(graph.metallic);
    if ((outputs & MaterialGraphOutputRoughness) != 0u)
        o.roughness = saturate(graph.roughness);
    if ((outputs & MaterialGraphOutputNormal) != 0u)
        o.normal = graph.normal;
    if ((outputs & MaterialGraphOutputEmissive) != 0u)
        o.emissive = max(graph.emissive, float3(0.0));
    if ((outputs & MaterialGraphOutputOcclusion) != 0u)
        o.occlusion = saturate(graph.occlusion);
    if ((outputs & MaterialGraphOutputSpecular) != 0u) o.specular = saturate(graph.specular);
    if ((outputs & MaterialGraphOutputSpecularColor) != 0u) o.specularColor = saturate(graph.specularColor);
    if ((outputs & MaterialGraphOutputClearCoat) != 0u) o.clearCoat = saturate(graph.clearCoat);
    if ((outputs & MaterialGraphOutputClearCoatRoughness) != 0u) o.clearCoatRoughness = saturate(graph.clearCoatRoughness);
    if ((outputs & MaterialGraphOutputClearCoatNormal) != 0u) o.clearCoatNormal = graph.clearCoatNormal;
    if ((outputs & MaterialGraphOutputSheenColor) != 0u) o.sheenColor = saturate(graph.sheenColor);
    if ((outputs & MaterialGraphOutputSheenRoughness) != 0u) o.sheenRoughness = saturate(graph.sheenRoughness);
    if ((outputs & MaterialGraphOutputAnisotropy) != 0u) o.anisotropy = saturate(graph.anisotropy);
    if ((outputs & MaterialGraphOutputAnisotropyDirection) != 0u) o.anisotropyDirection = graph.anisotropyDirection;
    if ((outputs & MaterialGraphOutputTransmission) != 0u) o.transmission = saturate(graph.transmission);
    if ((outputs & MaterialGraphOutputIOR) != 0u) o.ior = max(graph.ior, 1.0);
    if ((outputs & MaterialGraphOutputThickness) != 0u) o.thickness = max(graph.thickness, 0.0);
    if ((outputs & MaterialGraphOutputAttenuationColor) != 0u) o.attenuationColor = saturate(graph.attenuationColor);
    if ((outputs & MaterialGraphOutputAttenuationDistance) != 0u) o.attenuationDistance = max(graph.attenuationDistance, 0.0);
    if ((outputs & MaterialGraphOutputDiffuseTransmission) != 0u) o.diffuseTransmission = saturate(graph.diffuseTransmission);
    if ((outputs & MaterialGraphOutputIridescence) != 0u) o.iridescence = saturate(graph.iridescence);
    if ((outputs & MaterialGraphOutputIridescenceThickness) != 0u) o.iridescenceThickness = max(graph.iridescenceThickness, 0.0);
    if ((outputs & MaterialGraphOutputDispersion) != 0u) o.dispersion = max(graph.dispersion, 0.0);
    return outputs;
}

// The hit's own values in, for material_graph_ray_surface.
MaterialGraphOutputs material_graph_ray_defaults(float3 baseColor, float metallic, float roughness, float3 emissive)
{
    MaterialGraphOutputs o = (MaterialGraphOutputs)0;
    o.baseColor = baseColor;
    o.metallic = metallic;
    o.roughness = roughness;
    o.normal = float3(0.0, 0.0, 1.0);
    o.emissive = emissive;
    o.occlusion = 1.0;
    o.opacity = 1.0;
    o.specular = 1.0;
    o.specularColor = float3(1.0);
    o.clearCoatNormal = float3(0.0, 0.0, 1.0);
    o.anisotropyDirection = float2(1.0, 0.0);
    o.ior = 1.5;
    o.attenuationColor = float3(1.0);
    return o;
}
#endif

// Linearizes a Reverse-Z depth buffer sample into view-space distance (world units) for Scene Depth / Depth Fade (step 4h).
// Works for both perspective and orthographic projections; returns far sky distance when rawDepth <= 0.
float material_graph_linear_depth(UniformBufferObject ubo, float2 screenUV, float rawDepth)
{
    if (rawDepth <= 0.0)
        return 1e6 * ubo.unitsPerMeter;
    const float2 ndc = float2(screenUV.x * 2.0 - 1.0, 1.0 - screenUV.y * 2.0);
    const float4 worldH = mul(ubo.invViewProj, float4(ndc, rawDepth, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;
    const float3 viewPos = mul(ubo.view, float4(worldPos, 1.0)).xyz;
    return abs(viewPos.z);
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

// --- Object Position, Object Bounds, Space Transform & Matrices (step 4e part 2) ---

float3 material_graph_object_position(MaterialGraphInputs i)
{
    return mul(i.objectToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz + i.ubo.renderOrigin;
}

float3 material_graph_object_bounds_center(MaterialGraphInputs i)
{
    const float3 localCenter = 0.5 * (i.objectBoundsMin + i.objectBoundsMax);
    return mul(i.objectToWorld, float4(localCenter, 1.0)).xyz + i.ubo.renderOrigin;
}

float3 material_graph_object_bounds_extent(MaterialGraphInputs i)
{
    const float3 localExtent = 0.5 * (i.objectBoundsMax - i.objectBoundsMin);
    const float3x3 absM = float3x3(abs(i.objectToWorld[0].xyz), abs(i.objectToWorld[1].xyz), abs(i.objectToWorld[2].xyz));
    return mul(absM, localExtent);
}

// Spaces: 0 = Local, 1 = World, 2 = View, 3 = Clip.
float3 material_graph_transform(MaterialGraphInputs i, float3 v, uint fromSpace, uint toSpace, bool isPosition)
{
    if (fromSpace == toSpace)
        return v;

    // 1. Convert from `fromSpace` into render space.
    float3 r = v;
    if (fromSpace == 0u) // Local -> Render
        r = isPosition ? mul(i.objectToWorld, float4(v, 1.0)).xyz : mul((float3x3)i.objectToWorld, v);
    else if (fromSpace == 1u) // World -> Render
        r = isPosition ? (v - i.ubo.renderOrigin) : v;
    else if (fromSpace == 2u) // View -> Render
    {
        const float3x3 invView3 = transpose((float3x3)i.ubo.view);
        r = isPosition ? (mul(invView3, v) + i.ubo.cameraWorldPos.xyz) : mul(invView3, v);
    }
    else if (fromSpace == 3u) // Clip -> Render
    {
        if (isPosition)
        {
            const float4 w = mul(i.ubo.invViewProj, float4(v, 1.0));
            r = w.xyz / (abs(w.w) > 1e-6 ? w.w : 1.0);
        }
        else
            r = mul((float3x3)i.ubo.invViewProj, v);
    }

    // 2. Convert from render space into `toSpace`.
    if (toSpace == 0u) // Render -> Local
    {
        const float3x3 worldToLocal = transpose((float3x3)i.objectNormalMatrix);
        if (isPosition)
        {
            const float3 objOrigin = mul(i.objectToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz;
            return mul(worldToLocal, r - objOrigin);
        }
        return mul(worldToLocal, r);
    }
    if (toSpace == 1u) // Render -> World
        return isPosition ? (r + i.ubo.renderOrigin) : r;
    if (toSpace == 2u) // Render -> View
        return isPosition ? mul(i.ubo.view, float4(r, 1.0)).xyz : mul((float3x3)i.ubo.view, r);
    if (toSpace == 3u) // Render -> Clip
    {
        const float4x4 vp = mul(i.ubo.proj, i.ubo.view);
        if (isPosition)
        {
            const float4 c = mul(vp, float4(r, 1.0));
            return c.xyz / (abs(c.w) > 1e-6 ? c.w : 1.0);
        }
        return mul((float3x3)vp, r);
    }
    return r;
}

// Matrix kinds:
// 0 = Model (Local -> Render/World), 1 = Inverse Model, 2 = View, 3 = Inverse View,
// 4 = Projection, 5 = Inverse Projection, 6 = View Projection, 7 = Inverse View Projection.
float4x4 material_graph_matrix(MaterialGraphInputs i, uint kind)
{
    if (kind == 0u)
        return i.objectToWorld;
    if (kind == 1u)
    {
        const float3x3 inv3 = transpose((float3x3)i.objectNormalMatrix);
        const float3 objOrigin = mul(i.objectToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz;
        const float3 invT = -mul(inv3, objOrigin);
        return float4x4(
            float4(inv3[0], invT.x),
            float4(inv3[1], invT.y),
            float4(inv3[2], invT.z),
            float4(0.0, 0.0, 0.0, 1.0));
    }
    if (kind == 2u)
        return i.ubo.view;
    if (kind == 3u)
    {
        const float3x3 inv3 = transpose((float3x3)i.ubo.view);
        const float3 cam = i.ubo.cameraWorldPos.xyz;
        return float4x4(
            float4(inv3[0], cam.x),
            float4(inv3[1], cam.y),
            float4(inv3[2], cam.z),
            float4(0.0, 0.0, 0.0, 1.0));
    }
    if (kind == 4u)
        return i.ubo.proj;
    if (kind == 5u)
        return mul(i.ubo.view, i.ubo.invViewProj);
    if (kind == 6u)
        return mul(i.ubo.proj, i.ubo.view);
    if (kind == 7u)
        return i.ubo.invViewProj;
    return float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
}
