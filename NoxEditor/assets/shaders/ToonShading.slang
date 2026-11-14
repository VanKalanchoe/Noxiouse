// The Toon shading model (MaterialShadingModel::Toon; docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step 1): one
// directional / point / spot light on a toon surface -- the deferred lighting pass (DeferredLighting.slang, settings from the
// G-buffer: gbuffer_decode_toon) and the forward transparent one (Material_TransparentLit_Mesh.slang, settings from the material:
// toon_settings) light it with the same functions.
//
// - Diffuse: the light amount (N.L times the light's shadow -- shadows are banded with the light, not a soft gradient over it) in
//   `bands` steps (1: lit / unlit, two tones), each edge `bandSoftness` wide; or, with a ramp texture, the ramp's colour at that
//   amount (left: unlit, right: fully lit). Within a band the surface is flat: its colour times the light, as a Default Lit surface
//   facing the light.
// - Highlight: a sharp spot where N.H is within `specularSize` of the reflection, `specularSoftness` its edge; white on dielectrics,
//   the base colour on metals; only where the light reaches.
// - Rim: a band at grazing angles (1 - N.V within `rimWidth` of the silhouette) on the side the light reaches, in the rim colour.
//
// The ambient (GI, sky) and the reflections stay as for Default Lit (the caller's): a toon material with roughness 1 has no mirror
// reflections. Rect and disc lights light it as Default Lit (the caller's).
#pragma once
#include "shaderIO.h"

static const float ToonInvPi = 0.31830988618379067;

// The material's toon settings, as the G-buffer carries them (the forward pass reads the material directly).
ToonSettings toon_settings(GpuMaterial* material)
{
    ToonSettings toon;
    toon.rampTexture = material.toonRampTextureIndex;
    toon.bands = clamp(material.toonBands, 1u, 8u);
    toon.bandSoftness = saturate(material.toonBandSoftness);
    toon.specularSize = saturate(material.toonSpecularSize);
    toon.specularSoftness = saturate(material.toonSpecularSoftness);
    toon.specularIntensity = max(material.toonSpecularIntensity, 0.0);
    toon.rimColor = saturate(material.toonRimColor);
    toon.rimWidth = saturate(material.toonRimWidth);
    toon.rimIntensity = max(material.toonRimIntensity, 0.0);
    return toon;
}

// The banded light amount, 0 (unlit) .. 1 (fully lit), from the light amount `x` (N.L x shadow, 0..1). Band k starts at k / bands:
// the first at the terminator itself, so the unlit side meets the first tone without a jump.
float toon_bands(ToonSettings toon, float x)
{
    const float bands = float(toon.bands);
    const float edge = max(toon.bandSoftness, 1e-3) / bands;
    float level = 0.0;
    for (uint k = 0u; k < toon.bands; ++k)
    {
        const float start = float(k) / bands;
        level += smoothstep(start, start + edge, x);
    }
    return level / bands;
}

// The diffuse tone at light amount `x`: the bands' level (grey) or the ramp's colour.
float3 toon_tone(UniformBufferObject* ubo, ToonSettings toon, float x)
{
    if (toon.rampTexture != 0xFFFFFFFFu)
    {
        SamplerState rampSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
        return SampleMaterialTextureLevel(ubo, toon.rampTexture, rampSampler, float2(saturate(x), 0.5), 0.0).rgb;
    }
    return float3(toon_bands(toon, x));
}

// One light on a toon surface: diffuse tone + highlight + rim, in the units Default Lit's evaluateDirectLight returns.
// `diffuseColor`: the surface's diffuse albedo (the metallic part removed); `baseColor` and `metallic` colour the highlight;
// `NdotL` > 0 (the caller skips the unlit side), `shadow` the light's visibility (0..1), `radiance` its unshadowed radiance.
float3 toon_direct_light(UniformBufferObject* ubo, ToonSettings toon, float3 diffuseColor, float3 baseColor, float metallic,
                         float3 n, float3 v, float3 l, float NdotL, float shadow, float3 radiance)
{
    const float x = saturate(NdotL) * saturate(shadow);
    const float3 tone = toon_tone(ubo, toon, x);
    // Where the light reaches at all (the first band's edge): the highlight and the rim only there.
    const float reached = smoothstep(0.0, max(toon.bandSoftness, 1e-3) / float(toon.bands), x);
    float3 color = diffuseColor * ToonInvPi * tone;

    if (toon.specularSize > 0.0 && toon.specularIntensity > 0.0)
    {
        const float3 h = normalize(l + v);
        const float NdotH = saturate(dot(n, h));
        // Size 1 reaches out to N.H 0.5 (60 degrees), size 0.1 to about 0.995.
        const float threshold = 1.0 - 0.5 * toon.specularSize * toon.specularSize;
        const float width = max(toon.specularSoftness * (1.0 - threshold), 1e-5);
        const float spot = smoothstep(threshold - width, threshold + width, NdotH);
        const float3 tint = lerp(float3(1.0), baseColor, metallic);
        color += tint * (toon.specularIntensity * spot * reached * ToonInvPi);
    }

    if (toon.rimIntensity > 0.0 && toon.rimWidth > 0.0)
    {
        const float grazing = 1.0 - saturate(dot(n, v));
        const float softness = max(toon.bandSoftness, 0.02);
        const float rim = smoothstep(1.0 - toon.rimWidth - softness, 1.0 - toon.rimWidth + softness, grazing);
        color += toon.rimColor * (toon.rimIntensity * rim * reached * ToonInvPi);
    }
    return color * radiance;
}
