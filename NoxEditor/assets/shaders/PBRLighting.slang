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

// A rect or disc light (LightType::Rect / Disc, shaderIO.h) at a surface, unshadowed (multiply by its visibility). Diffuse exactly: the light's
// radiance times the cosine-weighted solid angle it fills above the surface's horizon. Specular from the point of the rectangle
// nearest the reflected ray, as a point light as strong as the whole rectangle seen from here, its energy spread over the
// wider lobe a light of that size makes (Karis 2013's "representative point", with a sphere of the same area's normalization).
// `pbrInputs` needs NdotV, the roughness and the colors; the per-light terms are set here.
float3 evaluateRectLight(PBRInfo pbrInputs, LightData light, float3 position, float3 n, float3 v)
{
    const float formFactor = rect_light_form_factor(light, position, n);
    if (formFactor <= 0.0)
        return float3(0.0);
    const float3 toCentre = light.position.xyz - position;
    const float centreDistance = length(toCentre);
    const float3 radiance = light.color.rgb * light.color.w * rect_light_window(light, centreDistance);
    const float3 diffuseLight = diffuse(pbrInputs) * radiance * formFactor;

    // The reflected ray against the light's plane, clamped into the rectangle (or the disc).
    const float3 front = normalize(light.direction.xyz);
    const float3 r = reflect(-v, n);
    const float facing = dot(r, front);
    float3 target = position + r * centreDistance;
    if (facing < -1e-4)
        target = position + r * max(dot(toCentre, front) / facing, 0.0);
    const float3 closest = area_light_clamp(light, target - light.position.xyz);

    const float3 toPoint = closest - position;
    const float pointDistance = length(toPoint);
    const float3 l = toPoint / max(pointDistance, 1e-6);
    const float NdotL = dot(n, l);
    if (NdotL <= 0.0)
        return diffuseLight;

    // The whole rectangle's light as from its solid angle (at most a hemisphere), the lobe widened by its size.
    const float area = rect_light_area(light);
    const float solidAngle = min(area * max(dot(front, -toCentre / max(centreDistance, 1e-6)), 0.0) / max(centreDistance * centreDistance, 1e-12),
                                 6.28318530718);
    const float alpha = max(pbrInputs.alphaRoughness, 1e-3);
    const float alphaWide = saturate(alpha + sqrt(area / M_PI) / (2.0 * max(centreDistance, 1e-6)));
    const float normalization = (alpha * alpha) / max(alphaWide * alphaWide, 1e-8);

    PBRInfo lit = pbrInputs;
    const float3 h = normalize(l + v);
    lit.NdotL = clamp(NdotL, 0.001, 1.0);
    lit.NdotH = clamp(dot(n, h), 0.0, 1.0);
    lit.LdotH = clamp(dot(l, h), 0.0, 1.0);
    lit.VdotH = clamp(dot(v, h), 0.0, 1.0);
    const float3 F = specularReflection(lit);
    const float G = geometricOcclusion(lit);
    const float D = microfacetDistribution(lit);
    const float3 specular = F * G * D / (4.0 * lit.NdotL * lit.NdotV) * lit.NdotL * radiance * solidAngle * normalization;
    return diffuseLight + specular;
}

// Specular occlusion (Lagarde & de Rousiers, "Moving Frostbite to PBR", 2014): a flat AO value describes how much
// of the sky a point sees in EVERY direction, but specular reflection only cares about one -- the reflection
// vector. Multiplying specular by the same diffuse `ao` either over-darkens a rough surface (whose wide lobe
// really does see roughly what diffuse AO measures) or under-darkens a smooth one at a grazing angle (a narrow
// lobe pointed past whatever is occluding the wider diffuse hemisphere). This derives a separate curve from the
// same `ao` input, view angle and roughness instead of reusing diffuse's own attenuation as-is.
float iblSpecularOcclusion(float NdotV, float ao, float roughness)
{
    return saturate(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}

// Image-based lighting: diffuse from the irradiance cubemap, specular from the prefiltered cubemap + split-sum BRDF
// LUT. Exactly DeferredLighting.slang's own getIBLContribution. `ao` is whatever ambient occlusion the caller
// already has for this pixel (SSAO/RTAO/a baked texture, or 1.0 -- fully visible -- when none ran this frame);
// only the specular term uses it here, through iblSpecularOcclusion above -- diffuse AO is applied once, by the
// caller, the same way it always was (baked-texture AO folded into occlusion further up, multiplied into the
// pixel's whole lit result at the very end).
IBLResult getIBLContribution(UniformBufferObject* ubo, PBRInfo pbrInputs, float3 n, float3 reflection, float ao)
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
    result.specular = specularLight * (pbrInputs.specularColor * brdf.x + brdf.y) *
                      iblSpecularOcclusion(pbrInputs.NdotV, ao, pbrInputs.perceptualRoughness);
    return result;
}
