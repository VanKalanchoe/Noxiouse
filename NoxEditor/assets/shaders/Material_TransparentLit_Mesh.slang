// Fragment stage for lit, alpha-blended (AlphaMode::Blend, non-Unlit) transparent objects.
// Mesh/Task stages are shared with the opaque forward-unlit pipeline (Material_PBR_MeshTask.slang
// / Material_PBR_Mesh.slang's meshMain, unchanged) - only this fragment stage is new.
//
// Deferred rendering (vis-buffer + G-buffer + DeferredLighting.slang, the real lighting path this
// engine uses) can't light transparent objects - a G-buffer only holds one surface per pixel, and
// blended layers need to draw after the opaque scene is already lit. That's not specific to this
// engine, every deferred renderer needs a forward pass for transparency. This shader IS that
// forward pass, but it does NOT reimplement lighting - it #includes PBRLighting.slang, the exact
// same BRDF/IBL functions DeferredLighting.slang itself uses, so there is one lighting
// implementation shared between the deferred opaque path and this forward one, not two. Without
// this, Blend objects fell back to Material_Unlit_Mesh.slang's flat baseColor pass-through (zero
// lighting math), which made them look like they were glowing/self-lit next to properly shaded
// Opaque/Mask geometry.
//
// Lights here are unshadowed (no per-object G-buffer to run the RT shadow pass against) - direct
// lights + IBL/DDGI ambient only. Plain alpha blending, no transmission.

#include "shaderIO.h"
#include "DDGICommon.slang"
#include "PBRLighting.slang"

import Math;

// Matches the vertex output produced by Material_PBR_Mesh.slang's meshMain.
struct VSOutput
{
    float4 pos : SV_Position;
    float3 worldPos;
    float3 normal;
    float2 uv0;
    float2 uv1;
    float4 color;
    nointerpolation uint drawID;
    nointerpolation int entityID;
};

static const float PBR_WORKFLOW_METALLIC_ROUGHNESS = 0.0;
static const float PBR_WORKFLOW_SPECULAR_GLOSSINESS = 1.0;

float3 getNormal(UniformBufferObject* ubo, InstanceData instance, VSOutput vertIn, SamplerState samplerState)
{
    Texture2D normalTexture = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.normalTextureIndex, 0));
    float3 tangentNormal = normalTexture.Sample(samplerState, instance.normalTextureSet == 0 ? vertIn.uv0 : vertIn.uv1).xyz * 2.0 - 1.0;

    float3 q1 = ddx(vertIn.worldPos);
    float3 q2 = ddy(vertIn.worldPos);
    float2 st1 = ddx(vertIn.uv0);
    float2 st2 = ddy(vertIn.uv0);

    float3 N = normalize(vertIn.normal);
    float3 T = normalize(q1 * st2.y - q2 * st1.y);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);

    return normalize(mul(tangentNormal, TBN));
}

float convertMetallic(float3 diffuseCol, float3 specularCol, float maxSpecular)
{
    float perceivedDiffuse = sqrt(0.299 * diffuseCol.r * diffuseCol.r + 0.587 * diffuseCol.g * diffuseCol.g + 0.114 * diffuseCol.b * diffuseCol.b);
    float perceivedSpecular = sqrt(0.299 * specularCol.r * specularCol.r + 0.587 * specularCol.g * specularCol.g + 0.114 * specularCol.b * specularCol.b);
    if (perceivedSpecular < c_MinRoughness)
    {
        return 0.0;
    }
    float a = c_MinRoughness;
    float b = perceivedDiffuse * (1.0 - maxSpecular) / (1.0 - c_MinRoughness) + perceivedSpecular - 2.0 * c_MinRoughness;
    float c = c_MinRoughness - perceivedSpecular;
    float D = max(b * b - 4.0 * a * c, 0.0);
    return clamp((-b + sqrt(D)) / (2.0 * a), 0.0, 1.0);
}

struct FSOutput
{
    float4 color : SV_TARGET0;
    int entityID : SV_TARGET1;
};

[shader("fragment")]
FSOutput fragMain(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace, uniform PushConstantMeshlets pushConstant)
{
    FSOutput output;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    SceneInstances instances = SceneInstances(ubo);
    InstanceData instance = instances[vertIn.drawID];

    SamplerState sampLinearHandle = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

    float perceptualRoughness = 1.0;
    float metallic = 0.0;
    float4 baseColor = float4(1.0);
    float3 f0 = float3(0.04);

    if (instance.alphaMode == 1) // Mask
    {
        float4 maskBaseColor = instance.baseColorFactor;
        if (instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            maskBaseColor = baseColorTex.Sample(sampLinearHandle, instance.baseColorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1) * instance.baseColorFactor;
        }
        if (maskBaseColor.a < instance.alphaMaskCutoff)
        {
            discard;
        }
    }

    if (instance.workflow == PBR_WORKFLOW_METALLIC_ROUGHNESS)
    {
        perceptualRoughness = instance.roughnessFactor;
        metallic = instance.metallicFactor;
        if (instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
        {
            Texture2D mrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
            float4 mrSample = mrTex.Sample(sampLinearHandle, instance.physicalDescriptorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1);
            perceptualRoughness *= mrSample.g;
            metallic *= mrSample.b;
        }
        perceptualRoughness = clamp(perceptualRoughness, c_MinRoughness, 1.0);
        metallic = clamp(metallic, 0.0, 1.0);

        if (instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            baseColor = baseColorTex.Sample(sampLinearHandle, instance.baseColorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1) * instance.baseColorFactor;
        }
        else
        {
            baseColor = instance.baseColorFactor;
        }
    }
    else // PBR_WORKFLOW_SPECULAR_GLOSSINESS
    {
        perceptualRoughness = 0.0;
        if (instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
        {
            Texture2D specGlossTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
            perceptualRoughness = 1.0 - specGlossTex.Sample(sampLinearHandle, instance.physicalDescriptorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1).a;
        }

        const float epsilon = 1e-6;
        float4 diffuseSample = instance.diffuseFactor;
        if (instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D colorMap = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            diffuseSample = SRGBtoLINEAR(colorMap.Sample(sampLinearHandle, vertIn.uv0)) * instance.diffuseFactor;
        }

        float3 specular = instance.specularFactor.rgb;
        if (instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
        {
            Texture2D specGlossTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
            specular *= SRGBtoLINEAR(specGlossTex.Sample(sampLinearHandle, vertIn.uv0)).rgb;
        }

        float maxSpecular = max(max(specular.r, specular.g), specular.b);
        metallic = convertMetallic(diffuseSample.rgb, specular, maxSpecular);

        float3 baseColorDiffusePart = diffuseSample.rgb * ((1.0 - maxSpecular) / (1.0 - c_MinRoughness) / max(1.0 - metallic, epsilon));
        float3 baseColorSpecularPart = specular - (float3(c_MinRoughness) * (1.0 - metallic) * (1.0 / max(metallic, epsilon)));
        baseColor = float4(lerp(baseColorDiffusePart, baseColorSpecularPart, metallic * metallic), diffuseSample.a);
    }

    float3 diffuseColor = baseColor.rgb * (1.0 - metallic);
    float3 specularColor = lerp(f0, baseColor.rgb, metallic);
    float alphaRoughness = perceptualRoughness * perceptualRoughness;
    float reflectance = max(max(specularColor.r, specularColor.g), specularColor.b);
    float3 specularEnvironmentR0 = specularColor;
    float3 specularEnvironmentR90 = clamp(float3(reflectance * 25.0), 0.0, 1.0);

    float3 n = (instance.normalTextureIndex != 0xFFFFFFFF) ? getNormal(ubo, instance, vertIn, sampLinearHandle) : normalize(vertIn.normal);
    if (instance.doubleSided != 0 && !isFrontFace)
    {
        n = -n;
    }
    float3 v = normalize(ubo->cameraWorldPos.xyz - vertIn.worldPos);
    float3 reflection = normalize(reflect(-v, n));
    float NdotV = clamp(dot(n, v), 0.001, 1.0);

    // Direct analytic lights - unshadowed (see file header: no G-buffer here for the RT shadow
    // pass to run against). Same evaluateDirectLight the deferred opaque path uses.
    float3 directLighting = float3(0.0);
    LightData* lights = (LightData*)ubo->lightDataReference;

    for (uint i = 0; i < ubo->lightCount; i++)
    {
        LightData light = lights[i];
        uint lightType = (uint)light.position.w;

        float3 l;
        float attenuation = 1.0;

        if (lightType == 0)
        {
            l = normalize(light.direction.xyz);
        }
        else
        {
            float3 lightVec = light.position.xyz - vertIn.worldPos;
            float distance = length(lightVec);
            l = distance > 0.0001 ? lightVec / distance : float3(0.0, 1.0, 0.0);

            float range = light.direction.w;
            attenuation = range > 0.0
                ? max(1.0 - pow(distance / range, 4.0), 0.0) / (distance * distance)
                : 1.0 / (distance * distance);

            if (lightType == 2)
            {
                float cd = dot(normalize(light.direction.xyz), -l);
                float angularAttenuation = saturate(cd * light.spotParams.x + light.spotParams.y);
                attenuation *= angularAttenuation * angularAttenuation;
            }
        }

        if (attenuation <= 0.0)
        {
            continue;
        }

        float NdotL = clamp(dot(n, l), 0.001, 1.0);
        if (dot(n, l) <= 0.0)
        {
            continue;
        }

        float3 h = normalize(l + v);
        PBRInfo pbrInputs = PBRInfo(
            NdotL, NdotV, clamp(dot(n, h), 0.0, 1.0), clamp(dot(l, h), 0.0, 1.0), clamp(dot(v, h), 0.0, 1.0),
            perceptualRoughness, metallic,
            specularEnvironmentR0, specularEnvironmentR90, alphaRoughness,
            diffuseColor, specularColor
        );

        float3 lightRadiance = light.color.rgb * light.color.w * attenuation;
        directLighting += evaluateDirectLight(pbrInputs, lightRadiance);
    }

    PBRInfo iblInputs = PBRInfo(
        1.0, NdotV, 1.0, 1.0, 1.0,
        perceptualRoughness, metallic,
        specularEnvironmentR0, specularEnvironmentR90, alphaRoughness,
        diffuseColor, specularColor
    );
    IBLResult ibl = getIBLContribution(ubo, iblInputs, n, reflection);
    float3 ambientDiffuse = ibl.diffuse;

    if (ubo->enableDDGI != 0 && ubo->ddgiIrradianceTextureIndex != 0xFFFFFFFF && ubo->ddgiDistanceTextureIndex != 0xFFFFFFFF)
    {
        Texture2D ddgiIrrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiIrradianceTextureIndex, 0));
        Texture2D ddgiDistTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiDistanceTextureIndex, 0));
        SamplerState ddgiSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
        float3 ddgiIrradiance = sample_ddgi_diffuse(ubo, vertIn.worldPos, n, ddgiIrrTex, ddgiDistTex, ddgiSampler);
        ambientDiffuse = ddgiIrradiance * diffuseColor;
    }

    float3 color = directLighting + ambientDiffuse + ibl.specular;

    if (instance.occlusionTextureIndex != 0xFFFFFFFF)
    {
        Texture2D occlusionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.occlusionTextureIndex, 0));
        float ao = occlusionTex.Sample(sampLinearHandle, instance.occlusionTextureSet == 0 ? vertIn.uv0 : vertIn.uv1).r;
        color *= ao;
    }

    float3 emissive = instance.emissiveFactor.rgb * instance.emissiveStrength;
    if (instance.emissiveTextureIndex != 0xFFFFFFFF)
    {
        Texture2D emissiveTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.emissiveTextureIndex, 0));
        emissive *= emissiveTex.Sample(sampLinearHandle, instance.emissiveTextureSet == 0 ? vertIn.uv0 : vertIn.uv1).rgb;
    }
    color += emissive;

    // Linear HDR, not tonemapped - this pipeline writes into the same m_hdrSceneResource as
    // everything else in the forward pass, which is only tonemapped once at the end of the frame.
    output.color = float4(color, baseColor.a);
    output.entityID = vertIn.entityID;

    return output;
}
