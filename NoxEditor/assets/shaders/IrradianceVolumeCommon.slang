// The irradiance volume's lookup (RS5a, Diffuse GI: Raster Probe Volume; GpuIrradianceVolume in shaderIO.h): the eight probes around a
// point, trilinear, each also weighted by whether it lies in front of the surface (DDGI's wrap term: less light through thin walls),
// their L1 spherical harmonics evaluated as irradiance around the normal -- divided by pi, as the other GI terms store it (diffuse
// colour x this = the diffuse light).
#pragma once

// The eight probes around a point, blended (trilinear x facing x visibility, the probes not captured yet left out): their radiance
// SH (R, G, B), their sky visibility SH, the fade towards the box's sides (weight) and the radiance scale (extents.w).
struct IrradianceVolumeSample
{
    float4 shR;
    float4 shG;
    float4 shB;
    float4 shSky;
    float weight;
    float scale;
};

// false outside the volume's box, or with no probe captured around (the caller keeps its other ambient).
bool irradiance_volume_gather(UniformBufferObject* ubo, float3 worldPos, float3 n, out IrradianceVolumeSample result)
{
    result.shR = result.shG = result.shB = result.shSky = float4(0.0);
    result.weight = 0.0;
    result.scale = 1.0;
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
    float weight = edge.x * edge.y * edge.z;

    const float3 grid = clamp((local + extents) / cell, float3(0.0), float3(counts - 1u));
    const uint3 base = min(uint3(floor(grid)), counts - 2u);
    const float3 f = grid - float3(base);
    float4* sh = (float4*)volume->probeSHReference;
    float4 shR = float4(0.0), shG = float4(0.0), shB = float4(0.0), shSky = float4(0.0);
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
        const uint index = ((probe.z * counts.y + probe.y) * counts.x + probe.x) * IrradianceProbeSHStride;
        // A probe not captured yet (the volume is still filling) is left out.
        const float captured = saturate(sh[index + 3].x);
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
                const float mean = max(dot(sh[index + 4], basisD), 0.0);
                const float meanSquared = max(dot(sh[index + 5], basisD), 0.0);
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
        shR += sh[index + 0] * w;
        shG += sh[index + 1] * w;
        shB += sh[index + 2] * w;
        shSky += sh[index + 6] * w;
        total += w;
        coverage += trilinearWeight * captured;
    }
    // Where the probes around are not captured yet, the other ambient shows (no black while it fills).
    weight *= saturate(coverage);
    if (total <= 0.0)
        return false;
    result.shR = shR / total;
    result.shG = shG / total;
    result.shB = shB / total;
    result.shSky = shSky / total;
    result.weight = weight;
    result.scale = volume->extents.w;
    return true;
}

// E(d) / pi for L1: Y00 L00 + (2/3) Y1m L1m(d) (Ramamoorthi & Hanrahan's cosine lobe), a world direction's basis.
float4 irradiance_volume_cosine_basis(float3 d)
{
    return float4(0.282095, (2.0 / 3.0) * 0.488603 * d.y, (2.0 / 3.0) * 0.488603 * d.z, (2.0 / 3.0) * 0.488603 * d.x);
}

// The light around direction d (E(d) / pi): around the normal, the diffuse light; around a reflection, what the room sends back
// from about there -- the blurry reflection of the surroundings the probes saw.
float3 irradiance_volume_radiance(IrradianceVolumeSample volumeSample, float3 d)
{
    const float4 basis = irradiance_volume_cosine_basis(d);
    return max(float3(dot(volumeSample.shR, basis), dot(volumeSample.shG, basis), dot(volumeSample.shB, basis)), float3(0.0)) *
           volumeSample.scale;
}

// How much of the sky the probes see around direction d, 0..1 (the cosine-weighted share of the half of the directions around d
// that reached the sky when they were captured): the sky's reflection held back by it -- Unreal's skylight occlusion, from the
// probes' own captures. Indoors, through a window only.
float irradiance_volume_sky_visibility(IrradianceVolumeSample volumeSample, float3 d)
{
    return saturate(dot(volumeSample.shSky, irradiance_volume_cosine_basis(d)));
}

// The reflection along r, from the volume: the sky as far as it is seen around r (skyVisibility), the room's light from about r
// elsewhere (occludedRadiance), faded out towards the box's sides (skyVisibility -> 1). false outside the volume.
bool irradiance_volume_reflection(UniformBufferObject* ubo, float3 worldPos, float3 n, float3 r, out float skyVisibility, out float3 occludedRadiance)
{
    skyVisibility = 1.0;
    occludedRadiance = float3(0.0);
    IrradianceVolumeSample volumeSample;
    if (!irradiance_volume_gather(ubo, worldPos, n, volumeSample))
        return false;
    skyVisibility = lerp(1.0, irradiance_volume_sky_visibility(volumeSample, r), volumeSample.weight);
    occludedRadiance = irradiance_volume_radiance(volumeSample, r);
    return true;
}

// false outside the volume's box (the caller keeps its other ambient); `irradiance` = E(n) / pi.
bool irradiance_volume_lookup(UniformBufferObject* ubo, float3 worldPos, float3 n, out float3 irradiance, out float weight)
{
    IrradianceVolumeSample volumeSample;
    irradiance = float3(0.0);
    weight = 0.0;
    if (!irradiance_volume_gather(ubo, worldPos, n, volumeSample))
        return false;
    irradiance = irradiance_volume_radiance(volumeSample, n);
    weight = volumeSample.weight;
    return true;
}
