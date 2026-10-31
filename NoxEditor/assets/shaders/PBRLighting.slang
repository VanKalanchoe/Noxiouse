// Shared PBR BRDF/IBL math - the actual functions DeferredLighting.slang uses to light the
// opaque G-buffer. Factored out here so a forward pass (which has no G-buffer to defer through -
// transparent objects) can light itself with the exact same math instead of a second, separate
// implementation. Include this from any shader that needs it; don't copy these functions again.

#ifndef PBR_LIGHTING_SLANG
#define PBR_LIGHTING_SLANG

#include "shaderIO.h"
#include "MaterialLobeMath.slang"

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
    // The split sum with the material's F90 (F0 x scale + F90 x bias), as the direct light's Fresnel has it: a reflection weaker than
    // full (KHR_materials_specular) has no grazing rim either.
    result.specular = specularLight * (pbrInputs.specularColor * brdf.x + pbrInputs.reflectance90 * brdf.y) *
                      iblSpecularOcclusion(pbrInputs.NdotV, ao, pbrInputs.perceptualRoughness);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The lobes past the core model (GBufferLobes, shaderIO.h): one slab as UE5 Substrate's, each lobe as its glTF extension's spec
// gives it (KHR_materials_clearcoat / sheen / specular / iridescence / anisotropy, the Khronos glTF Sample Viewer's evaluation).
// A pixel without lobes never reaches these (the callers test GBufferLobes::flags) and shades exactly as before.
// ---------------------------------------------------------------------------------------------------------------------------

float max3(float3 v)
{
    return max(max(v.x, v.y), v.z);
}

float3 F_schlick(float3 f0, float3 f90, float cosTheta)
{
    return f0 + (f90 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// The base reflectance with KHR_materials_specular / KHR_materials_ior: the dielectric F0 scaled by the specular weight, F90 the
// weight (metals: their base color, 1). Without the lobe the core model's 4% and its F90 (DeferredLighting.slang).
void lobe_base_reflectance(GBufferLobes lobes, float3 baseColor, float metallic, out float3 diffuseColor, out float3 specularColor, out float3 f90)
{
    if ((lobes.flags & MaterialLobeSpecular) != 0u)
    {
        const float3 dielectricF0 = lobes.dielectricF0 * lobes.specularWeight;
        diffuseColor = baseColor * (1.0 - max3(dielectricF0)) * (1.0 - metallic);
        specularColor = lerp(dielectricF0, baseColor, metallic);
        f90 = float3(lerp(lobes.specularWeight, 1.0, metallic));
        return;
    }
    const float3 f0 = float3(0.04);
    diffuseColor = baseColor * (float3(1.0) - f0) * (1.0 - metallic);
    specularColor = lerp(f0, baseColor, metallic);
    f90 = float3(clamp(max3(specularColor) * 25.0, 0.0, 1.0));
}

// The direction a stretched highlight runs along (KHR_materials_anisotropy's anisotropicT) from its stored angle (gbuffer_normal_basis),
// made perpendicular to the shading normal.
float3 lobe_anisotropy_direction(float3 storedNormal, float angle, float3 n)
{
    float3 t0, b0;
    gbuffer_normal_basis(storedNormal, t0, b0);
    const float3 t = cos(angle) * t0 + sin(angle) * b0;
    return normalize(t - n * dot(n, t));
}

// The sheen BRDF's directional albedo E (its integral over the hemisphere for one view angle): the BRDF LUT's blue (BRDFIntegration.slang).
float sheen_albedo(UniformBufferObject* ubo, float cosTheta, float sheenRoughness)
{
#ifdef NOX_SHADER_CPU_TEST
    // The CPU test of this math (slangc -target cpp) has no textures: it integrates the albedo directly.
    return sheen_albedo_direct(cosTheta, sheenRoughness);
#else
    if (ubo->brdfLutIndex == 0xFFFFFFFF)
        return 0.0;
    SamplerState samplerBrdf = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_BRDFLUT, 0));
    Texture2D brdfTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->brdfLutIndex, 0));
    return brdfTex.SampleLevel(samplerBrdf, float2(saturate(cosTheta), 1.0 - sheenRoughness), 0).b;
#endif
}

// The F0 a Schlick curve needs to reach F at this angle (the split-sum lookup's F0 for an iridescent surface).
float3 schlick_to_f0(float3 F, float cosTheta)
{
    const float x = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return clamp((F - x) / max(1.0 - x, 1e-4), 0.0, 1.0);
}

// What a pixel's lobes need once, before its lights: the coat's Fresnel weight, the sheen's albedo scaling of what is below it,
// the film's Fresnel at the view angle, the anisotropic frame.
struct LobeShading
{
    GBufferLobes lobes;
    float coatNdotV;
    float coatWeight;          // coat factor x the coat's Fresnel (IOR 1.5) at the view angle: how much of the view the coat reflects
    float sheenScalingV;       // 1 - max(sheen color) x E(NdotV): what of the layers below the sheen lets through
    float3 iridescenceFresnel; // the film's Fresnel at NdotV, the metal's and the dielectric's mixed by metallic
    float3 anisotropicT;
    float3 anisotropicB;
    float alphaT;
    float alphaB;
};

// `anisotropicT`: the direction highlights stretch along (with anisotropy; lobe_anisotropy_direction from the G-buffer's angle).
LobeShading lobe_prepare(UniformBufferObject* ubo, GBufferLobes lobes, float3 anisotropicT, float3 n, float3 v, float NdotV,
                         float3 baseColor, float metallic, float alphaRoughness)
{
    LobeShading shading;
    shading.lobes = lobes;
    shading.coatNdotV = clamp(dot(lobes.coatNormal, v), 0.001, 1.0);
    shading.coatWeight = lobes.coat * F_schlick(float3(0.04), float3(1.0), shading.coatNdotV).x;
    shading.sheenScalingV = (lobes.flags & MaterialLobeSheen) != 0u
                                ? 1.0 - max3(lobes.sheenColor) * sheen_albedo(ubo, NdotV, lobes.sheenRoughness)
                                : 1.0;
    shading.iridescenceFresnel = float3(0.0);
    if (lobes.iridescenceThickness <= 0.0)
        shading.lobes.iridescence = 0.0; // no film (the reference renderer's rule)
    if (shading.lobes.iridescence > 0.0)
    {
        const float3 dielectricF0 = lobes.dielectricF0 * lobes.specularWeight;
        const float3 dielectric = iridescence_fresnel(1.0, lobes.iridescenceIor, dielectricF0, lobes.iridescenceThickness, NdotV);
        const float3 metal = iridescence_fresnel(1.0, lobes.iridescenceIor, baseColor, lobes.iridescenceThickness, NdotV);
        shading.iridescenceFresnel = lerp(dielectric, metal, metallic);
    }
    shading.anisotropicT = float3(1.0, 0.0, 0.0);
    shading.anisotropicB = float3(0.0, 1.0, 0.0);
    shading.alphaT = shading.alphaB = alphaRoughness;
    if (lobes.anisotropy > 0.0)
    {
        shading.anisotropicT = normalize(anisotropicT - n * dot(n, anisotropicT));
        shading.anisotropicB = normalize(cross(n, shading.anisotropicT));
        shading.alphaT = lerp(alphaRoughness, 1.0, lobes.anisotropy * lobes.anisotropy);
    }
    return shading;
}

// One analytic light on a surface with lobes: evaluateDirectLight's base with the film's Fresnel and the anisotropic GGX, under the
// sheen (albedo scaling) and the coat (fresnel_coat). `pbrInputs` as for evaluateDirectLight; `l` the direction to the light.
float3 evaluateDirectLightLobes(UniformBufferObject* ubo, PBRInfo pbrInputs, LobeShading shading, float3 n, float3 v, float3 l, float3 lightRadiance)
{
    const GBufferLobes lobes = shading.lobes;
    float3 F = specularReflection(pbrInputs);
    float3 diffuseWeight = 1.0 - F;
    if (lobes.iridescence > 0.0)
    {
        // The film's Fresnel in place of the base's; what is below it weighted by its largest channel (the spec's rgb_mix: no
        // complementary tint in the diffuse).
        F = lerp(F, shading.iridescenceFresnel, lobes.iridescence);
        diffuseWeight = lerp(diffuseWeight, float3(1.0 - max3(shading.iridescenceFresnel)), lobes.iridescence);
    }

    float specular;
    if (lobes.anisotropy > 0.0)
    {
        const float3 h = normalize(l + v);
        const float3 T = shading.anisotropicT;
        const float3 B = shading.anisotropicB;
        specular = V_GGX_anisotropic(pbrInputs.NdotL, pbrInputs.NdotV, dot(B, v), dot(T, v), dot(T, l), dot(B, l), shading.alphaT, shading.alphaB) *
                   D_GGX_anisotropic(pbrInputs.NdotH, dot(T, h), dot(B, h), shading.alphaT, shading.alphaB);
    }
    else
    {
        specular = geometricOcclusion(pbrInputs) * microfacetDistribution(pbrInputs) / (4.0 * pbrInputs.NdotL * pbrInputs.NdotV);
    }
    float3 color = pbrInputs.NdotL * lightRadiance * (diffuseWeight * diffuse(pbrInputs) + F * specular);

    if ((lobes.flags & MaterialLobeSheen) != 0u)
    {
        const float scalingL = 1.0 - max3(lobes.sheenColor) * sheen_albedo(ubo, pbrInputs.NdotL, lobes.sheenRoughness);
        const float3 sheen = lobes.sheenColor * D_charlie(lobes.sheenRoughness, pbrInputs.NdotH) * V_charlie(pbrInputs.NdotL, pbrInputs.NdotV, lobes.sheenRoughness);
        color = color * min(shading.sheenScalingV, scalingL) + pbrInputs.NdotL * lightRadiance * sheen;
    }

    if (lobes.coat > 0.0)
    {
        const float3 nc = lobes.coatNormal;
        const float coatNdotL = dot(nc, l);
        float3 coat = float3(0.0);
        if (coatNdotL > 0.0)
        {
            const float3 h = normalize(l + v);
            PBRInfo coatInputs = pbrInputs;
            coatInputs.NdotL = clamp(coatNdotL, 0.001, 1.0);
            coatInputs.NdotV = shading.coatNdotV;
            coatInputs.NdotH = clamp(dot(nc, h), 0.0, 1.0);
            const float coatRoughness = clamp(lobes.coatRoughness, c_MinRoughness, 1.0);
            coatInputs.alphaRoughness = coatRoughness * coatRoughness;
            coat = coatInputs.NdotL * lightRadiance * geometricOcclusion(coatInputs) * microfacetDistribution(coatInputs) /
                   (4.0 * coatInputs.NdotL * coatInputs.NdotV);
        }
        color = color * (1.0 - shading.coatWeight) + shading.coatWeight * coat;
    }
    return color;
}

// A rect or disc light on a surface with lobes: evaluateRectLight's base (isotropic, without the film) under the sheen's albedo
// scaling and the coat, whose own highlight the same representative point gives with the coat's normal and roughness.
float3 evaluateRectLightLobes(PBRInfo pbrInputs, LobeShading shading, LightData light, float3 position, float3 n, float3 v)
{
    const GBufferLobes lobes = shading.lobes;
    float3 color = evaluateRectLight(pbrInputs, light, position, n, v) * shading.sheenScalingV;
    if (lobes.coat > 0.0)
    {
        // The coat's specular alone: no diffuse, a Fresnel of 1 (its weight is the view-angle Fresnel, as for every light).
        PBRInfo coatInputs = pbrInputs;
        coatInputs.NdotV = shading.coatNdotV;
        const float coatRoughness = clamp(lobes.coatRoughness, c_MinRoughness, 1.0);
        coatInputs.perceptualRoughness = coatRoughness;
        coatInputs.alphaRoughness = coatRoughness * coatRoughness;
        coatInputs.diffuseColor = float3(0.0);
        coatInputs.reflectance0 = coatInputs.reflectance90 = coatInputs.specularColor = float3(1.0);
        color = color * (1.0 - shading.coatWeight) + shading.coatWeight * evaluateRectLight(coatInputs, light, position, lobes.coatNormal, v);
    }
    return color;
}

// The reflection direction an anisotropic surface's environment lookup takes (the spec's bent normal); the plain reflection otherwise.
float3 lobe_reflection(LobeShading shading, float3 n, float3 v, float perceptualRoughness)
{
    if (shading.lobes.anisotropy <= 0.0)
        return normalize(reflect(-v, n));
    float3 bentNormal = cross(shading.anisotropicB, v);
    bentNormal = normalize(cross(bentNormal, shading.anisotropicB));
    const float a = pow(1.0 - shading.lobes.anisotropy * (1.0 - perceptualRoughness), 4.0);
    bentNormal = normalize(lerp(bentNormal, n, a));
    const float3 r = reflect(-v, bentNormal);
    return normalize(lerp(r, bentNormal, perceptualRoughness * perceptualRoughness));
}

// The prefiltered environment's radiance along a direction at a roughness (getIBLContribution's specular lookup, before its BRDF).
float3 prefilteredRadiance(UniformBufferObject* ubo, float3 direction, float perceptualRoughness)
{
    if (ubo->prefilteredMapIndex == 0xFFFFFFFF)
        return float3(0.0);
    SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
    TextureCube prefilteredTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
    return prefilteredTex.SampleLevel(samplerPrefiltered, direction, perceptualRoughness * ubo->prefilteredCubeMipLevels).rgb * ubo->scaleIBLAmbient;
}

// The split-sum's scale and bias (the BRDF LUT's red and green) at a view angle and roughness.
float2 brdfScaleBias(UniformBufferObject* ubo, float NdotV, float perceptualRoughness)
{
    if (ubo->brdfLutIndex == 0xFFFFFFFF)
        return float2(1.0, 0.0);
    SamplerState samplerBrdf = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_BRDFLUT, 0));
    Texture2D brdfTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->brdfLutIndex, 0));
    return brdfTex.SampleLevel(samplerBrdf, float2(NdotV, 1.0 - perceptualRoughness), 0).rg;
}

// The ambient light of the lobes on top of the base, given the base's ambient diffuse + specular (environment, GI, reflections:
// whatever the caller has) and the environment radiance each lobe sees (`coatRadiance` along the coat's reflection at its
// roughness, `sheenRadiance` at the sheen's): the sheen's albedo-scaled layer, then the coat over everything (fresnel_coat).
float3 lobe_ambient(UniformBufferObject* ubo, LobeShading shading, float3 baseAmbient, float3 coatRadiance, float3 sheenRadiance, float NdotV)
{
    const GBufferLobes lobes = shading.lobes;
    float3 color = baseAmbient;
    if ((lobes.flags & MaterialLobeSheen) != 0u)
        color = color * shading.sheenScalingV + sheenRadiance * lobes.sheenColor * sheen_albedo(ubo, NdotV, lobes.sheenRoughness);
    if (lobes.coat > 0.0)
    {
        // The coat's Fresnel is its weight (fresnel_coat), so its environment term is the split sum with F = 1: scale + bias.
        const float2 coatBrdf = brdfScaleBias(ubo, shading.coatNdotV, clamp(lobes.coatRoughness, c_MinRoughness, 1.0));
        color = color * (1.0 - shading.coatWeight) + shading.coatWeight * coatRadiance * (coatBrdf.x + coatBrdf.y);
    }
    return color;
}

#endif
