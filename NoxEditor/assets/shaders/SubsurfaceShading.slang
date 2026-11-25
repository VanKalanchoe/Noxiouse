// The Subsurface shading model (MaterialShadingModel::Subsurface; docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step R1):
// light scattered beneath a translucent solid (skin, wax, marble, jade) -- shared between DeferredLighting.slang (settings decoded from
// the G-buffer lobe target: gbuffer_decode_subsurface) and Material_TransparentLit_Mesh.slang (from GpuMaterial: subsurface_settings).
//
// - Specular: standard GGX microfacet specular (PBRLighting.slang), keeping crisp dielectric highlights on skin/wax/marble.
// - Wrap diffuse: energy-conserving wrap lighting controlled by `radius` that softens the light terminator and shifts the wrap band
//   toward `color` (classic warm subsurface terminator bleed).
// - Backlit transmission scatter (Barre-Brisebois / Unreal Subsurface): view-dependent forward scattering when light passes from behind
//   the surface towards the viewer, controlled by `1 - opacity` and tinted by `color`.
#pragma once
#include "shaderIO.h"
#include "PBRLighting.slang"

static const float SubsurfaceInvPi = 0.31830988618379067;

// The material's subsurface settings (the forward pass reads the material directly; G-buffer resolve packs them into output.lobes).
SubsurfaceSettings subsurface_settings(GpuMaterial material, float3 colorOverride)
{
    SubsurfaceSettings sss;
    sss.color = saturate(colorOverride);
    sss.opacity = saturate(material.subsurfaceOpacity);
    sss.radius = clamp(material.subsurfaceRadius, 0.0, 2.0);
    return sss;
}

// One directional / point / spot light on a Subsurface surface:
// `NdotL_raw` may be <= 0 (the caller passes lights behind the surface too so wrap + backlit scatter glow).
// `shadow` is the direct visibility (0..1); `unshadowedRadiance` is light.color.rgb * light.color.w * attenuation.
float3 subsurface_direct_light(SubsurfaceSettings sss, PBRInfo pbrInputs, float3 n, float3 v, float3 l,
                               float NdotL_raw, float shadow, float3 unshadowedRadiance)
{
    const float thickness = saturate(sss.opacity);
    const float transmission = 1.0 - thickness;
    const float wrap = saturate(sss.radius * 0.5);

    // 1. Standard GGX specular + front diffuse on the lit side.
    float3 result = float3(0.0);
    if (NdotL_raw > 0.0 && shadow > 0.0)
    {
        const float3 F = specularReflection(pbrInputs);
        const float G = geometricOcclusion(pbrInputs);
        const float D = microfacetDistribution(pbrInputs);
        const float3 specContrib = F * G * D / max(4.0 * pbrInputs.NdotL * pbrInputs.NdotV, 0.001);
        result += pbrInputs.NdotL * specContrib * (unshadowedRadiance * shadow);
    }

    // 2. Energy-conserving wrap diffuse with subsurface colour shift across the terminator.
    const float wrapDenom = (1.0 + wrap) * (1.0 + wrap);
    const float wrapNdotL = saturate((NdotL_raw + wrap) / wrapDenom);
    const float directNdotL = saturate(NdotL_raw);
    // In the wrap band (past the sharp Lambertian terminator), light travels through the medium and picks up sss.color.
    const float scatterBand = saturate(wrapNdotL - directNdotL * shadow);
    const float scatterShadow = lerp(0.18 * transmission, 1.0, shadow);
    const float3 frontDiffuse = pbrInputs.diffuseColor * SubsurfaceInvPi * (directNdotL * shadow);
    const float3 wrapScatter = sss.color * SubsurfaceInvPi * (scatterBand * scatterShadow * (0.65 + 0.35 * sss.radius));
    result += (frontDiffuse + wrapScatter) * unshadowedRadiance;

    // 3. View-dependent backlit transmission scatter (light shining through thin edges / back of the object toward the camera).
    if (transmission > 0.001)
    {
        const float distortion = 0.35;
        const float power = lerp(3.0, 6.0, thickness);
        const float3 backHalf = normalize(l + n * distortion);
        const float backDot = pow(saturate(dot(v, -backHalf)), power);
        // Stronger on the unlit/grazing side and attenuated gently by shadow so thin backlit silhouettes glow warmly.
        const float backShadow = lerp(0.35, 1.0, shadow);
        const float3 backScatter = sss.color * (backDot * transmission * backShadow * (0.8 + 0.4 * sss.radius) * SubsurfaceInvPi);
        result += backScatter * unshadowedRadiance;
    }

    return result;
}

// Soft subsurface ambient fill: lets indirect / sky irradiance bleed subtly in the subsurface tint.
float3 subsurface_ambient_scatter(SubsurfaceSettings sss, float3 ambientDiffuse, float occlusion)
{
    const float transmission = 1.0 - saturate(sss.opacity);
    return ambientDiffuse * sss.color * (0.22 * transmission * saturate(sss.radius * 0.5 + 0.5) * occlusion);
}
