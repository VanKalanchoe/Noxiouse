// The irradiance volume's lookup (RS5a, Diffuse GI: Raster Probe Volume; GpuIrradianceVolume in shaderIO.h): the eight probes around a
// point, trilinear, each also weighted by whether it lies in front of the surface (DDGI's wrap term: less light through thin walls),
// their L1 spherical harmonics evaluated as irradiance around the normal -- divided by pi, as the other GI terms store it (diffuse
// colour x this = the diffuse light).
#pragma once

#include "ShadowMaps.slang"

// The sun at a surface through the irradiance volume's own sun shadow map (one orthographic view over the whole box, cached:
// Renderer::addIrradianceVolumePasses) -- what the volume's probes are lit by, and the screen probes' hits in their Surface Cache hit
// lighting (no shadow ray). One bilinear comparison: GI light is soft anyway. Normal offset and a step towards the sun of a texel or
// so against an averaged or coarse surface shadowing itself. false: no map, or the point is outside it (the caller decides).
bool irradiance_volume_sun_visibility(UniformBufferObject* ubo, float3 position, float3 n, float3 L, out float visibility)
{
    visibility = 1.0;
    if (ubo->irradianceVolumeReference == 0)
        return false;
    GpuIrradianceVolume* volume = (GpuIrradianceVolume*)ubo->irradianceVolumeReference;
    if (volume->sunShadowTextureIndex == 0xFFFFFFFF)
        return false;
    const float texel = volume->sunTexelWorld;
    const float NoL = saturate(dot(n, L));
    const float sinTheta = sqrt(saturate(1.0 - NoL * NoL));
    const float3 receiver = position + n * (2.0 * texel * sinTheta) + L * texel;
    const float4 clip = mul(volume->sunViewProj, float4(receiver, 1.0));
    const float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    if (any(uv <= 0.0) || any(uv >= 1.0) || clip.z <= 0.0 || clip.z > 1.0)
        return false;
    Texture2D<float> map = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + volume->sunShadowTextureIndex, 0));
    SamplerState pointClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_NEAREST_CLAMP, 0));
    const float slope = min(sinTheta / max(NoL, 0.05), 4.0);
    const float bias = (1.0 + slope) * texel / max(volume->sunDepthRange, 1e-6);
    visibility = shadow_map_bilinear(map, pointClamp, uv, clip.z + bias, float(volume->sunShadowResolution));
    return true;
}

// false outside the volume's box (the caller keeps its other ambient); `irradiance` = E(n) / pi.
bool irradiance_volume_lookup(UniformBufferObject* ubo, float3 worldPos, float3 n, out float3 irradiance, out float weight)
{
    irradiance = float3(0.0);
    weight = 0.0;
    if (ubo->irradianceVolumeReference == 0)
        return false;
    GpuIrradianceVolume* volume = (GpuIrradianceVolume*)ubo->irradianceVolumeReference;
    const float3 extents = volume->extents.xyz;
    const float3 localNormal = normalize(mul(volume->worldToLocal, float4(n, 0.0)).xyz);
    const float3 local = mul(volume->worldToLocal, float4(worldPos, 1.0)).xyz + localNormal * volume->normalBias;
    const float3 inside = extents - abs(local);
    if (any(inside <= 0.0))
        return false;
    const uint3 counts = uint3(volume->countX, volume->countY, volume->countZ);
    const float3 cell = 2.0 * extents / float3(max(counts, uint3(2)) - 1u);
    // Fades over the outermost cell towards the box's sides (into the other ambient).
    const float3 edge = saturate(inside / cell);
    weight = edge.x * edge.y * edge.z;

    const float3 grid = clamp((local + extents) / cell, float3(0.0), float3(counts - 1u));
    const uint3 base = min(uint3(floor(grid)), counts - 2u);
    const float3 f = grid - float3(base);
    float4* sh = (float4*)volume->probeSHReference;
    float4 shR = float4(0.0), shG = float4(0.0), shB = float4(0.0);
    float total = 0.0;
    float coverage = 0.0;
    for (uint corner = 0; corner < 8; ++corner)
    {
        const uint3 offset = uint3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        const uint3 probe = base + offset;
        const float3 trilinear = lerp(1.0 - f, f, float3(offset));
        // In front of the surface counts more (wrap shading, as DDGI's probe weight).
        const float3 toProbe = normalize(float3(probe) * cell - extents - local + localNormal * 1e-4);
        const float facing = 0.5 * (dot(toProbe, localNormal) + 1.0);
        const uint index = (probe.z * counts.y + probe.y) * counts.x + probe.x;
        // A probe not captured yet (the volume is still filling) is left out.
        const float captured = saturate(sh[index * 6 + 3].x);
        const float trilinearWeight = trilinear.x * trilinear.y * trilinear.z;
        // Visibility (DDGI's Chebyshev test): how far this probe sees towards the point (its distance SH, mean and square); a point
        // beyond that is likely behind a wall from it.
        float visibility = 1.0;
        {
            const float3 probeToPoint = mul(volume->localToWorld, float4(local - (float3(probe) * cell - extents), 0.0)).xyz;
            const float distance = length(probeToPoint);
            if (distance > 1e-4 && captured > 0.0)
            {
                const float3 d = probeToPoint / distance;
                const float4 basisD = float4(0.282095, 0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x);
                const float mean = max(dot(sh[index * 6 + 4], basisD), 0.0);
                const float meanSquared = max(dot(sh[index * 6 + 5], basisD), 0.0);
                if (distance > mean)
                {
                    const float variance = max(abs(meanSquared - mean * mean), 1e-4 * mean * mean + 1e-6);
                    const float excess = distance - mean;
                    const float chebyshev = variance / (variance + excess * excess);
                    visibility = max(chebyshev * chebyshev * chebyshev, 0.02);
                }
            }
        }
        const float w = trilinearWeight * (facing * facing + 0.05) * visibility * captured;
        shR += sh[index * 6 + 0] * w;
        shG += sh[index * 6 + 1] * w;
        shB += sh[index * 6 + 2] * w;
        total += w;
        coverage += trilinearWeight * captured;
    }
    // Where the probes around are not captured yet, the other ambient shows (no black while it fills).
    weight *= saturate(coverage);
    if (total <= 0.0)
        return false;
    shR /= total;
    shG /= total;
    shB /= total;
    // E(n) / pi for L1: Y00 L00 + (2/3) Y1m L1m(n) (Ramamoorthi & Hanrahan's cosine lobe), the world normal's basis.
    const float4 basis = float4(0.282095, (2.0 / 3.0) * 0.488603 * n.y, (2.0 / 3.0) * 0.488603 * n.z, (2.0 / 3.0) * 0.488603 * n.x);
    irradiance = max(float3(dot(shR, basis), dot(shG, basis), dot(shB, basis)), float3(0.0)) * volume->extents.w;
    return true;
}
