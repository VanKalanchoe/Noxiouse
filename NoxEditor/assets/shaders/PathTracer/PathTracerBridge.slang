#pragma once

#include "../shaderIO.h"
#include "StandardBSDF.slang"
#include "Ray.slang"
#include "RTXPT/EnvMap.hlsli"
#include "RTXPTSampling.slang"

namespace Bridge
{
    EnvMapSampler environmentSampler(UniformBufferObject* ubo, uint skyIndex, uint importanceIndex)
    {
        EnvMapSceneParams scene = (EnvMapSceneParams)0;
        scene.Transform = float3x4(1,0,0,0, 0,1,0,0, 0,0,1,0);
        scene.InvTransform = scene.Transform;
        scene.ColorMultiplier = float3(ubo->scaleIBLAmbient);
        scene.Enabled = 1;
        EnvMapImportanceSamplingParams sampling;
        sampling.ImportanceInvDim = float2(1.0/512.0);
        sampling.ImportanceBaseMip = 9;
        sampling.padding0 = 0;
        return EnvMapSampler::make(
            SamplerState.Handle(uint2(SamplerIndex::SAMPLER_NEAREST_CLAMP,0)),
            Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset+importanceIndex,0)), sampling,
            TextureCube<float4>.Handle(uint2(ubo->imageHeapIndexOffset+skyIndex,0)),
            SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT,0)), scene);
    }
    struct SurfaceData
    {
        ShadingData shadingData;
        StandardBSDF bsdf;
        float3 emission;
        float ior;
        bool isOpaque;
        float3 rrDiffuseAlbedo;
        float3 rrSpecularAlbedo;
        float rrRoughness;
    };

    // RTXPT's stateless uniform generator advances with Hash32. Effect-specific seed construction is
    // performed by RTXPTSampling::uniformState at every vertex.
    float rand(inout uint state)
    {
        state = RTXPTSampling::hash32(state);
        return RTXPTSampling::toFloat(state);
    }

    uint initRNG(uint2 pixelPos, uint frameIndex)
    {
        uint packedPixel = pixelPos.x | (pixelPos.y << 16);
        return RTXPTSampling::uniformState(packedPixel, 0u, frameIndex, RTXPTSampling::EffectBase);
    }

    // Duff et al. orthonormal basis construction
    void buildOrthonormalBasis(float3 n, out float3 b1, out float3 b2)
    {
        float sign = n.z >= 0.0 ? 1.0 : -1.0;
        float a = -1.0 / (sign + n.z);
        float b = n.x * n.y * a;
        b1 = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
        b2 = float3(b, sign + n.y * n.y * a, -n.y);
    }

    float textureLod(Texture2D texture, float rayConeWidth, float uvPerWorld)
    {
        uint width, height, levels;
        texture.GetDimensions(0, width, height, levels);
        float footprint = rayConeWidth * uvPerWorld * float(max(width, height));
        // RTXPT realtime default TexLODBias is -1: retain one additional mip of material detail.
        return clamp(log2(max(footprint, 1.0)) - 1.0, 0.0, float(max(levels, 1u) - 1u));
    }

    // Reverse-Z camera ray generation
    Ray computeCameraRay(uint2 pixelPos, inout uint rngState, PushConstantPathTracer pc, UniformBufferObject* ubo, out float pixelSpreadAngle)
    {
        // The projection already carries RTXPT's default R2 jitter. Match its additional per-pixel policy:
        // 0.1 for RR, zero for DLSS-SR, and a full pixel for native progressive accumulation.
        float2 jitter = (RTXPTSampling::generate4(pixelPos.x | (pixelPos.y << 16), 0u, pc.frameIndex,
                                                  RTXPTSampling::EffectBase).xy - 0.5) * pc.cameraRayJitterScale;
        float2 uv = (float2(pixelPos) + 0.5 + jitter) / pc.viewportSize;
        float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

        // Near plane in Reverse-Z is 1.0
        // A point 1000x the near distance away along the ray (reverse-Z depth 0.001, not the near plane): subtracting the
        // camera position from a near-plane point (0.01 units away) cancelled away most float digits, so the ray direction
        // wobbled by up to a pixel between frames while the camera moved.
        float4 nearPosH = mul(pc.invViewProj, float4(ndc_xy, 0.001, 1.0));
        float3 nearPos = nearPosH.xyz / nearPosH.w;

        float3 rayOrigin = ubo->cameraWorldPos.xyz;
        float3 rayDir = normalize(nearPos - rayOrigin);

        float2 adjacentNdc = ndc_xy + float2(2.0 / pc.viewportSize.x, 0.0);
        float4 adjacentH = mul(pc.invViewProj, float4(adjacentNdc, 0.001, 1.0));
        float3 adjacentDir = normalize(adjacentH.xyz / adjacentH.w - rayOrigin);
        pixelSpreadAngle = acos(clamp(dot(rayDir, adjacentDir), -1.0, 1.0));

        return Ray::make(rayOrigin, rayDir, 0.001, 1e9);
    }

    // Loads surface geometry and material data at hit point
    SurfaceData loadSurface(
        uint instanceID,
        uint primitiveIndex,
        float2 barycentrics,
        float3 rayDir,
        float rayConeWidth,
        UniformBufferObject* ubo
    )
    {
        SurfaceData surface;
        GpuRayTracingInstance* lutBuffer = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
        GpuRayTracingInstance lut = lutBuffer[instanceID];

        // 1. Fetch triangle indices and vertices
        uint3 idx;
        if (lut.indexBufferAddress != 0)
            idx = ((uint3*)lut.indexBufferAddress)[primitiveIndex];
        else
            idx = uint3(primitiveIndex * 3 + 0, primitiveIndex * 3 + 1, primitiveIndex * 3 + 2);

        Vertex v0 = LoadRayTracingVertex(lut, idx.x);
        Vertex v1 = LoadRayTracingVertex(lut, idx.y);
        Vertex v2 = LoadRayTracingVertex(lut, idx.z);

        // 2. Interpolate attributes with barycentrics
        float3 bary = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        float3 localPos = v0.pos * bary.x + v1.pos * bary.y + v2.pos * bary.z;
        float3 localNormal = normalize(v0.normal * bary.x + v1.normal * bary.y + v2.normal * bary.z);
        float2 uv = v0.uv0 * bary.x + v1.uv0 * bary.y + v2.uv0 * bary.z;

        // 3. Transform to world space using DXR intrinsics
        float3x4 objToWorld = ObjectToWorld3x4();
        float3 posW = mul(objToWorld, float4(localPos, 1.0));
        float3 normalW = normalize(mul((float3x3)lut.normalMatrix, localNormal));

        bool frontFace = dot(normalW, -rayDir) >= 0.0;
        if (!frontFace)
        {
            normalW = -normalW;
        }

        // 4. Setup ShadingData
        surface.shadingData.posW = posW;
        surface.shadingData.N = normalW;
        surface.shadingData.V = -rayDir;
        buildOrthonormalBasis(normalW, surface.shadingData.T, surface.shadingData.B);
        // KHR_materials_transmission without a positive KHR_materials_volume thickness is an
        // infinitely thin sheet. RTXPT's Bistro override marks the painting/window cover glass
        // (LMBR_0000103_Paris_Glass_01) ThinSurface=true. Treating it as a solid volume bends the
        // camera ray without a matching exit interface and replaces the painting with a dark reflection.
        surface.shadingData.mtl.thinSurface = lut.transmissionFactor <= 0.0 || lut.thickness <= 0.0;

        float3 edge1 = mul((float3x3)objToWorld, v1.pos - v0.pos);
        float3 edge2 = mul((float3x3)objToWorld, v2.pos - v0.pos);
        float2 duv1 = v1.uv0 - v0.uv0, duv2 = v2.uv0 - v0.uv0;
        float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
        float worldDoubleArea = length(cross(edge1, edge2));
        float uvPerWorld = sqrt(abs(determinant) / max(worldDoubleArea, 1e-12));
        if (abs(determinant) > 1e-10)
        {
            float3 tangent = (edge1 * duv2.y - edge2 * duv1.y) / determinant;
            tangent -= normalW * dot(normalW, tangent);
            if (dot(tangent, tangent) > 1e-12)
            {
                surface.shadingData.T = normalize(tangent);
                float3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) / determinant;
                surface.shadingData.B = normalize(cross(normalW, surface.shadingData.T));
                if (dot(surface.shadingData.B, bitangent) < 0) surface.shadingData.B *= -1;
            }
        }

        // 5. Sample bindless textures
        SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

        float4 baseColor = lut.workflow == 1.0 ? lut.diffuseFactor : lut.baseColorFactor;
        if (lut.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D albedoTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.baseColorTextureIndex, 0));
            baseColor *= albedoTex.SampleLevel(textureSampler, uv, textureLod(albedoTex, rayConeWidth, uvPerWorld));
        }

        float metallic = lut.metallicFactor;
        float roughness = lut.roughnessFactor;
        float ior = lut.ior > 0.0 ? lut.ior : 1.5;
        float dielectricF0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
        float3 specularF0 = lerp(float3(dielectricF0), baseColor.rgb, saturate(metallic));
        float3 diffuseAlbedo = baseColor.rgb * (1.0 - saturate(metallic));
        if (lut.metallicRoughnessTextureIndex != 0xFFFFFFFF)
        {
            Texture2D mrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.metallicRoughnessTextureIndex, 0));
            float4 mrSample = mrTex.SampleLevel(textureSampler, uv, textureLod(mrTex, rayConeWidth, uvPerWorld));
            if (lut.workflow == 1.0)
            {
                float3 specularColor = lut.specularFactor.rgb * mrSample.rgb;
                roughness = 1.0 - lut.specularFactor.a * mrSample.a;
                diffuseAlbedo = baseColor.rgb * (1.0 - max(specularColor.r, max(specularColor.g, specularColor.b)));
                specularF0 = specularColor;
                metallic = 0.0;
            }
            else
            {
                roughness *= mrSample.g;
                metallic *= mrSample.b;
                diffuseAlbedo = baseColor.rgb * (1.0 - saturate(metallic));
                specularF0 = lerp(float3(dielectricF0), baseColor.rgb, saturate(metallic));
            }
        }
        else if (lut.workflow == 1.0)
        {
            float3 specularColor = lut.specularFactor.rgb;
            roughness = 1.0 - lut.specularFactor.a;
            diffuseAlbedo = baseColor.rgb * (1.0 - max(specularColor.r, max(specularColor.g, specularColor.b)));
            specularF0 = specularColor;
            metallic = 0.0;
        }

        // Normal mapping
        if (lut.normalTextureIndex != 0xFFFFFFFF)
        {
            Texture2D normTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.normalTextureIndex, 0));
            float3 tangentNormal = normTex.SampleLevel(textureSampler, uv, textureLod(normTex, rayConeWidth, uvPerWorld)).xyz;
            // Match RTXPT's ApplyNormalMapRTXPT. Some Bistro DDNA maps omit Z and expect it to be
            // reconstructed; treating their stored zero as -1 bends highlights into the surface.
            tangentNormal.xy = tangentNormal.xy * 2.0 - 1.0;
            tangentNormal.z = tangentNormal.z <= 0.0
                ? sqrt(saturate(1.0 - dot(tangentNormal.xy, tangentNormal.xy)))
                : abs(tangentNormal.z * 2.0 - 1.0);
            tangentNormal = dot(tangentNormal, tangentNormal) > 1e-12 ? normalize(tangentNormal) : float3(0.0, 0.0, 1.0);
            surface.shadingData.N = normalize(
                surface.shadingData.T * tangentNormal.x +
                surface.shadingData.B * tangentNormal.y +
                surface.shadingData.N * tangentNormal.z
            );
            buildOrthonormalBasis(surface.shadingData.N, surface.shadingData.T, surface.shadingData.B);
        }

        float transmission = lut.transmissionFactor;
        if (lut.transmissionTextureIndex != 0xFFFFFFFF)
        {
            Texture2D transTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.transmissionTextureIndex, 0));
            transmission *= transTex.SampleLevel(textureSampler, uv, textureLod(transTex, rayConeWidth, uvPerWorld)).r;
        }

        // 6. Setup BSDF data
        float eta = frontFace ? 1.0 / ior : ior;
        // KHR_materials_transmission applies only to the dielectric portion; transparent metals are undefined.
        // This is the exact factor used by RTXPT's PathTracerBridgeDonut.
        transmission *= 1.0 - saturate(metallic);
        StandardBSDFData bsdfData = StandardBSDFData::make(
            diffuseAlbedo,
            specularF0,
            saturate(roughness), saturate(metallic), eta, baseColor.rgb, 0.0, saturate(transmission));

        surface.bsdf = StandardBSDF::make(bsdfData);
        float specularTransmission = saturate(transmission);
        surface.rrRoughness = saturate(roughness);
        // Match RTXPT StandardBSDF::estimateSpecDiffBSDF exactly: transmission belongs to the specular guide and the
        // reflection integral is evaluated after its (1 - transmission) mixture weight is applied.
        float3 specularReflectionAlbedo = (1.0 - specularTransmission) * specularF0;
        float noV = saturate(dot(surface.shadingData.N, surface.shadingData.V));
        float3 reflectionAlbedo = approxSpecularIntegralGGX(
            specularReflectionAlbedo, surface.rrRoughness * surface.rrRoughness, noV);
        // RTXPT stable planes clamp both reflectance estimates to 0.04 before exposing them to a denoiser. A zero
        // guide makes RR divide by an effectively absent material response and is unstable around glass/metal edges.
        surface.rrDiffuseAlbedo = max(diffuseAlbedo * (1.0 - specularTransmission), float3(0.04));
        surface.rrSpecularAlbedo = max(reflectionAlbedo + specularTransmission * baseColor.rgb, float3(0.04));
        surface.emission = lut.emissiveFactor.rgb * lut.emissiveFactor.a;
        if (lut.emissiveTextureIndex != 0xFFFFFFFF)
        {
            Texture2D emissiveTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.emissiveTextureIndex, 0));
            surface.emission *= emissiveTex.SampleLevel(textureSampler, uv, textureLod(emissiveTex, rayConeWidth, uvPerWorld)).rgb;
        }
        surface.ior = ior;
        surface.isOpaque = (lut.alphaMode == 0);

        return surface;
    }

    // Alpha test for AnyHit shader
    bool AlphaTest(uint instanceID, uint primitiveIndex, float2 barycentrics, UniformBufferObject* ubo, float opacitySample = 0.5)
    {
        GpuRayTracingInstance* lutBuffer = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
        GpuRayTracingInstance lut = lutBuffer[instanceID];

        if (lut.alphaMode == 0)
        {
            return true;
        }

        uint3 idx;
        if (lut.indexBufferAddress != 0)
            idx = ((uint3*)lut.indexBufferAddress)[primitiveIndex];
        else
            idx = uint3(primitiveIndex * 3 + 0, primitiveIndex * 3 + 1, primitiveIndex * 3 + 2);

        Vertex v0 = LoadRayTracingVertex(lut, idx.x);
        Vertex v1 = LoadRayTracingVertex(lut, idx.y);
        Vertex v2 = LoadRayTracingVertex(lut, idx.z);

        float3 bary = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
        float2 uv = v0.uv0 * bary.x + v1.uv0 * bary.y + v2.uv0 * bary.z;

        SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
        float alpha = lut.baseColorFactor.a;
        if (lut.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D albedoTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.baseColorTextureIndex, 0));
            alpha *= albedoTex.SampleLevel(textureSampler, uv, 0.0).a;
        }

        return lut.alphaMode == 2 ? opacitySample < saturate(alpha) : alpha >= lut.alphaCutoff;
    }

    // Environment map / skybox sampling
    float3 sampleSkybox(float3 dir, UniformBufferObject* ubo, uint skyboxTextureIndex, float lod = 0.0)
    {
        if (skyboxTextureIndex == 0xFFFFFFFF)
        {
            return float3(0.0);
        }

        SamplerState sampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
        TextureCube skybox = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + skyboxTextureIndex, 0));
        return skybox.SampleLevel(sampler, dir, lod).rgb * ubo->scaleIBLAmbient;
    }
}
