// Shared PBR BRDF/IBL math - the actual functions DeferredLighting.slang uses to light the
// opaque G-buffer. Factored out here so a forward pass (which has no G-buffer to defer through -
// transparent objects) can light itself with the exact same math instead of a second, separate
// implementation. Include this from any shader that needs it; don't copy these functions again.

#include "shaderIO.h"

struct PBRInfo
{
    float NdotL;
    float NdotV;
    float NdotH;
    float LdotH;
    float VdotH;
    float perceptualRoughness;
    float metalness;
    float3 reflectance0;
    float3 reflectance90;
    float alphaRoughness;
    float3 diffuseColor;
    float3 specularColor;
};

struct IBLResult
{
    float3 diffuse;
    float3 specular;
};

static const float M_PI = 3.141592653589793;
static const float c_MinRoughness = 0.04;

float3 diffuse(PBRInfo pbrInputs)
{
    return pbrInputs.diffuseColor / M_PI;
}

// Fresnel reflectance (Schlick)
float3 specularReflection(PBRInfo pbrInputs)
{
    return pbrInputs.reflectance0 + (pbrInputs.reflectance90 - pbrInputs.reflectance0) * pow(clamp(1.0 - pbrInputs.VdotH, 0.0, 1.0), 5.0);
}

// Geometric occlusion (Smith GGX correlated)
float geometricOcclusion(PBRInfo pbrInputs)
{
    float NdotL = pbrInputs.NdotL;
    float NdotV = pbrInputs.NdotV;
    float r = pbrInputs.alphaRoughness;

    float attenuationL = 2.0 * NdotL / (NdotL + sqrt(r * r + (1.0 - r * r) * (NdotL * NdotL)));
    float attenuationV = 2.0 * NdotV / (NdotV + sqrt(r * r + (1.0 - r * r) * (NdotV * NdotV)));
    return attenuationL * attenuationV;
}

// Normal distribution (GGX Trowbridge-Reitz)
float microfacetDistribution(PBRInfo pbrInputs)
{
    float roughnessSq = pbrInputs.alphaRoughness * pbrInputs.alphaRoughness;
    float f = (pbrInputs.NdotH * roughnessSq - pbrInputs.NdotH) * pbrInputs.NdotH + 1.0;
    return roughnessSq / (M_PI * f * f);
}

// One analytic light's contribution (direct diffuse + specular). `lightRadiance` already has
// attenuation/shadow/color folded in by the caller. Exactly the per-light body
// DeferredLighting.slang's light loop evaluates, factored out so every lit pass (deferred opaque,
// forward transparent) uses the identical math instead of each having their own copy.
float3 evaluateDirectLight(PBRInfo pbrInputs, float3 lightRadiance)
{
    float3 F = specularReflection(pbrInputs);
    float G = geometricOcclusion(pbrInputs);
    float D = microfacetDistribution(pbrInputs);

    float3 diffuseContrib = (1.0 - F) * diffuse(pbrInputs);
    float3 specContrib = F * G * D / (4.0 * pbrInputs.NdotL * pbrInputs.NdotV);

    return pbrInputs.NdotL * lightRadiance * (diffuseContrib + specContrib);
}

// Image-based lighting: diffuse from the irradiance cubemap, specular from the prefiltered
// cubemap + split-sum BRDF LUT. Exactly DeferredLighting.slang's own getIBLContribution.
IBLResult getIBLContribution(UniformBufferObject* ubo, PBRInfo pbrInputs, float3 n, float3 reflection)
{
    if (ubo->irradianceMapIndex == 0xFFFFFFFF || ubo->prefilteredMapIndex == 0xFFFFFFFF || ubo->brdfLutIndex == 0xFFFFFFFF)
    {
        IBLResult empty;
        empty.diffuse = float3(0.0);
        empty.specular = float3(0.0);
        return empty;
    }
    float lod = (pbrInputs.perceptualRoughness * ubo->prefilteredCubeMipLevels);

    SamplerState samplerBrdf = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_BRDFLUT, 0));
    SamplerState samplerIrradiance = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
    SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));

    Texture2D brdfTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->brdfLutIndex, 0));
    float3 brdf = brdfTex.Sample(samplerBrdf, float2(pbrInputs.NdotV, 1.0 - pbrInputs.perceptualRoughness)).rgb;

    TextureCube irradianceTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
    float4 irradianceSample = irradianceTex.Sample(samplerIrradiance, n);
    float3 diffuseLight = irradianceSample.rgb;

    TextureCube prefilteredTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
    float4 prefilteredSample = prefilteredTex.SampleLevel(samplerPrefiltered, reflection, lod);
    float3 specularLight = prefilteredSample.rgb * ubo->scaleIBLAmbient;

    IBLResult result;
    result.diffuse = diffuseLight * pbrInputs.diffuseColor * ubo->scaleIBLAmbient;
    result.specular = specularLight * (pbrInputs.specularColor * brdf.x + brdf.y);
    return result;
}
