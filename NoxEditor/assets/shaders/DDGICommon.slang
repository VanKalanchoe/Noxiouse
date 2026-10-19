#pragma once

#include "shaderIO.h"

// =========================================================================
// Octahedral Coordinate Mapping (Unit Direction <-> Unit Square [-1, 1])
// =========================================================================
float2 oct_sign_not_zero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float2 oct_encode(float3 v)
{
    float l1norm = abs(v.x) + abs(v.y) + abs(v.z);
    float2 result = v.xy * (1.0 / max(l1norm, 1e-6));
    if (v.z < 0.0)
    {
        result = (1.0 - abs(result.yx)) * oct_sign_not_zero(result);
    }
    return result;
}

float3 oct_decode(float2 f)
{
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// =========================================================================
// Spherical Fibonacci Direction Sampling
// Generates uniformly distributed ray directions across a sphere.
// =========================================================================
float3 spherical_fibonacci(uint i, uint numRays)
{
    const float phi = 1.618033988749895; // Golden ratio
    float z = 1.0 - (2.0 * float(i) + 1.0) / float(numRays);
    float radius = sqrt(max(0.0, 1.0 - z * z));
    float theta = 2.0 * 3.14159265359 * float(i) / phi;
    return float3(cos(theta) * radius, sin(theta) * radius, z);
}

// 3D rotation via Rodrigues' rotation formula
float3 rotate_vector(float3 v, float3 axis, float angle)
{
    return v * cos(angle) + cross(axis, v) * sin(angle) + axis * dot(axis, v) * (1.0 - cos(angle));
}

// =========================================================================
// Grid Coordinate Unpacking
// =========================================================================
uint3 get_probe_coords(uint probeIndex, uint countX, uint countY)
{
    uint pz = probeIndex / (countX * countY);
    uint rem = probeIndex % (countX * countY);
    uint py = rem / countX;
    uint px = rem % countX;
    return uint3(px, py, pz);
}

uint get_probe_index(uint3 coords, uint countX, uint countY)
{
    return coords.x + coords.y * countX + coords.z * (countX * countY);
}

float3 get_probe_world_pos(uint probeIndex, float3 origin, float3 spacing, uint countX, uint countY)
{
    uint3 coords = get_probe_coords(probeIndex, countX, countY);
    return origin + float3(coords) * spacing;
}

// =========================================================================
// Cascades and volume addressing
// ubo->ddgiCascadeCount nested volumes around the camera, each with the same probe counts and a wider spacing than the one inside
// it. Probes are stored cascade after cascade; within a cascade a probe's atlas slot is its grid coordinate in the world modulo the
// counts (toroidal addressing), so a cascade that follows the camera keeps every probe's history where it is while it scrolls. A
// probe's place in its cascade ("local" coordinate, 0 at the cascade's origin) is its slot minus the scroll, modulo the counts.
// =========================================================================
uint3 ddgi_counts(UniformBufferObject* ubo)
{
    return uint3(uint(ubo->ddgiGridOrigin.w), uint(ubo->ddgiGridSpacing.w), uint(ubo->ddgiGridParams.x));
}

uint ddgi_probes_per_cascade(UniformBufferObject* ubo)
{
    const uint3 counts = ddgi_counts(ubo);
    return counts.x * counts.y * counts.z;
}

uint ddgi_cascade_of(UniformBufferObject* ubo, uint storageIndex)
{
    return min(storageIndex / max(ddgi_probes_per_cascade(ubo), 1u), max(ubo->ddgiCascadeCount, 1u) - 1u);
}

float3 ddgi_cascade_origin(UniformBufferObject* ubo, uint cascade)
{
    const DDGICascade c = ubo->ddgiCascades[cascade];
    return float3(c.originX, c.originY, c.originZ);
}

float3 ddgi_cascade_spacing(UniformBufferObject* ubo, uint cascade)
{
    const DDGICascade c = ubo->ddgiCascades[cascade];
    return float3(c.spacingX, c.spacingY, c.spacingZ);
}

uint ddgi_storage_index(UniformBufferObject* ubo, uint cascade, uint3 localCoord)
{
    const uint3 counts = ddgi_counts(ubo);
    const DDGICascade c = ubo->ddgiCascades[cascade];
    const uint3 slot = (localCoord + uint3(c.scrollX, c.scrollY, c.scrollZ)) % counts;
    return cascade * ddgi_probes_per_cascade(ubo) + get_probe_index(slot, counts.x, counts.y);
}

uint3 ddgi_local_coord(UniformBufferObject* ubo, uint storageIndex)
{
    const uint3 counts = ddgi_counts(ubo);
    const uint cascade = ddgi_cascade_of(ubo, storageIndex);
    const DDGICascade c = ubo->ddgiCascades[cascade];
    const uint3 slot = get_probe_coords(storageIndex - cascade * ddgi_probes_per_cascade(ubo), counts.x, counts.y);
    const uint3 scroll = uint3(c.scrollX, c.scrollY, c.scrollZ) % counts;
    return (slot + counts - scroll) % counts;
}

float3 ddgi_probe_position(UniformBufferObject* ubo, uint storageIndex)
{
    const uint cascade = ddgi_cascade_of(ubo, storageIndex);
    return ddgi_cascade_origin(ubo, cascade) + float3(ddgi_local_coord(ubo, storageIndex)) * ddgi_cascade_spacing(ubo, cascade);
}

// The probe came into its cascade this frame (the cascade moved): what its slot holds belongs to a probe that left, so it starts over.
bool ddgi_probe_entered(UniformBufferObject* ubo, uint storageIndex)
{
    const int3 counts = int3(ddgi_counts(ubo));
    const int3 local = int3(ddgi_local_coord(ubo, storageIndex));
    const DDGICascade c = ubo->ddgiCascades[ddgi_cascade_of(ubo, storageIndex)];
    const int3 shift = int3(c.shiftX, c.shiftY, c.shiftZ);
    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(shift[axis]) >= counts[axis])
            return true;
        if (shift[axis] > 0 && local[axis] >= counts[axis] - shift[axis])
            return true;
        if (shift[axis] < 0 && local[axis] < -shift[axis])
            return true;
    }
    return false;
}

// Updated this frame: the first cascade's probes every ddgiUpdateStride frames in turn, each further cascade half as often (its
// probes are farther away, where lighting changes show less), and always the ones that just came in.
bool ddgi_probe_updates(UniformBufferObject* ubo, uint storageIndex, uint frameIndex)
{
    const uint stride = min(max(ubo->ddgiUpdateStride, 1u) << ddgi_cascade_of(ubo, storageIndex), 16u);
    return stride == 1 || ((storageIndex + frameIndex) % stride) == 0 || ddgi_probe_entered(ubo, storageIndex);
}

// How far a probe's distance moments look: rays are clamped to this (about one and a half of its cascade's cells).
float ddgi_max_distance(UniformBufferObject* ubo, uint storageIndex)
{
    return 1.5 * length(ddgi_cascade_spacing(ubo, ddgi_cascade_of(ubo, storageIndex)));
}

// =========================================================================
// Atlas Coordinate Helpers
// Converts probe ID and direction to UV coordinate inside octahedral atlas.
//
// probeSide: inner texels per probe (8 for irradiance, 16 for distance)
// totalProbeSide: probeSide + 2 border pixels (10 for irradiance, 18 for distance)
// =========================================================================
float2 get_probe_atlas_uv(uint probeIndex, float3 dir, uint probeSide, uint probesPerRow, float2 atlasSize)
{
    uint probeX = probeIndex % probesPerRow;
    uint probeY = probeIndex / probesPerRow;

    // Encode direction into normalized [-1, 1] square
    float2 oct = oct_encode(dir);

    // Map [-1, 1] to [0, 1] within the inner probe square
    float2 normalizedOct = oct * 0.5 + 0.5;

    // Each probe slot has 1 border texel on each side:
    // Inner region spans [1, probeSide + 1] out of (probeSide + 2)
    float totalSide = float(probeSide + 2);
    float2 probeTopLeft = float2(float(probeX) * totalSide, float(probeY) * totalSide);

    // Inset by 1 texel border, then offset by normalizedOct * probeSide
    float2 pixelPos = probeTopLeft + 1.0 + normalizedOct * float(probeSide);

    // Normalize to atlas texture UV [0, 1]
    return pixelPos / atlasSize;
}

// =========================================================================
// Chebyshev Visibility Test (Variance Shadow Mapping for DDGI Probes)
// Eliminates light leaking through walls by evaluating mean & variance distance.
// =========================================================================
float chebyshev_visibility(float distToProbe, float meanDist, float meanDistSq, float varianceFloor)
{
    float d = distToProbe - meanDist;
    if (d <= 0.0)
    {
        return 1.0;
    }

    // Variance floor (meters squared, like the moments): prevents division by zero while maintaining sharp shadows
    float variance = max(abs(meanDistSq - (meanDist * meanDist)), varianceFloor);
    float pMax = variance / (variance + d * d);

    // Aggressive leak suppression threshold and power 4 falloff (matching RTXGI standard)
    pMax = saturate((pMax - 0.15) / 0.85);
    return pMax * pMax * pMax * pMax;
}

// =========================================================================
// Multi-Probe Trilinear & Chebyshev Interpolation for Surface Shading
// Samples the 8 nearest probes of a cascade, weighting by trilinear position, surface orientation cosine, Chebyshev depth
// visibility (no light leaking through walls) and classification (probes buried in geometry are off).
// =========================================================================

// How far inside the cascade the point is: 1 one cell or more in from its faces, falling to 0 at them (outside: 0).
float ddgi_cascade_weight(UniformBufferObject* ubo, uint cascade, float3 worldPos)
{
    const uint3 counts = ddgi_counts(ubo);
    const float3 spacing = ddgi_cascade_spacing(ubo, cascade);
    const float3 extent = float3(counts - 1) * spacing;
    const float3 fromOrigin = worldPos - ddgi_cascade_origin(ubo, cascade);
    const float3 cellsInside = min(fromOrigin, extent - fromOrigin) / spacing;
    return saturate(min(cellsInside.x, min(cellsInside.y, cellsInside.z)));
}

float3 sample_ddgi_cascade(
    UniformBufferObject* ubo,
    uint cascade,
    float3 worldPos,
    float3 normal,
    Texture2D irradianceAtlas,
    Texture2D distanceAtlas,
    SamplerState atlasSampler
)
{
    const uint3 counts = ddgi_counts(ubo);
    const float3 origin = ddgi_cascade_origin(ubo, cascade);
    const float3 spacing = ddgi_cascade_spacing(ubo, cascade);
    const float normalBias = ubo->ddgiGridParams.w;
    // The distance moments are in meters (DDGIBlendDistance.slang): (2 cm)^2.
    const float varianceFloor = 0.0004;

    // Shift sampling position along surface normal (isotropic, relative to the cascade's cells)
    const float minSpacing = min(spacing.x, min(spacing.y, spacing.z));
    const float3 biasedPos = worldPos + normal * (normalBias * minSpacing);

    const float3 relativePos = (biasedPos - origin) / spacing;
    const int3 maxIndices = int3(counts) - 2;
    const int3 baseCoord = clamp(int3(floor(relativePos)), int3(0, 0, 0), maxIndices);
    const float3 fracCoord = saturate(relativePos - float3(baseCoord));

    const uint probesPerRowIrr = uint(ubo->ddgiAtlasParams.x) / 10;
    const uint probesPerRowDist = uint(ubo->ddgiAtlasParams.z) / 18;

    float3 totalIrradiance = float3(0.0);
    float totalWeight = 0.0;

    for (int dz = 0; dz <= 1; dz++)
    {
        for (int dy = 0; dy <= 1; dy++)
        {
            for (int dx = 0; dx <= 1; dx++)
            {
                const uint3 probeCoord = uint3(baseCoord + int3(dx, dy, dz));
                const uint probeIndex = ddgi_storage_index(ubo, cascade, probeCoord);
                const float3 probePos = origin + float3(probeCoord) * spacing;

                const float3 toProbeVec = probePos - worldPos;
                const float distToProbe = length(toProbeVec);
                const float3 toProbeDir = distToProbe > 1e-4 ? toProbeVec / distToProbe : normal;

                // 1. Normal orientation test: Probes behind the surface tangent plane MUST NOT contribute
                const float NdotL = dot(normal, toProbeDir);
                if (NdotL <= 0.0)
                {
                    continue;
                }
                const float normalWeight = NdotL * NdotL;

                // 2. Trilinear interpolation factor
                const float trilinear = (dx != 0 ? fracCoord.x : (1.0 - fracCoord.x)) *
                                        (dy != 0 ? fracCoord.y : (1.0 - fracCoord.y)) *
                                        (dz != 0 ? fracCoord.z : (1.0 - fracCoord.z));

                // 3. Chebyshev visibility test: does the probe have line of sight to the surface?
                const float2 distUV = get_probe_atlas_uv(probeIndex, -toProbeDir, 16, probesPerRowDist, ubo->ddgiAtlasParams.zw);
                const float2 moments = distanceAtlas.SampleLevel(atlasSampler, distUV, 0).rg;
                const float visibility = chebyshev_visibility(distToProbe / ubo->unitsPerMeter, moments.x, moments.y, varianceFloor);

                float weight = trilinear * normalWeight * visibility;
                if (weight < 1e-5)
                {
                    continue;
                }

                // 4. Fetch probe irradiance along the surface normal; alpha 0 is a probe buried in geometry (classification), skipped
                const float2 irrUV = get_probe_atlas_uv(probeIndex, normal, 8, probesPerRowIrr, ubo->ddgiAtlasParams.xy);
                const float4 probeIrr = irradianceAtlas.SampleLevel(atlasSampler, irrUV, 0);
                weight *= saturate(probeIrr.a);
                if (weight < 1e-5)
                {
                    continue;
                }

                totalIrradiance += probeIrr.rgb * weight;
                totalWeight += weight;
            }
        }
    }

    return totalWeight > 1e-4 ? totalIrradiance / totalWeight : float3(0.0);
}

// The diffuse irradiance (/ pi, as the lighting reads it) at a surface: from the finest cascade around it, handing over to the next
// coarser one over its last cell, and to the environment's irradiance (what the IBL path uses) beyond the coarsest.
float3 sample_ddgi_diffuse(
    UniformBufferObject* ubo,
    float3 worldPos,
    float3 normal,
    Texture2D irradianceAtlas,
    Texture2D distanceAtlas,
    SamplerState atlasSampler
)
{
    float3 environment = float3(0.0);
    if (ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        TextureCube irradianceTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState samplerIrradiance = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        environment = irradianceTex.SampleLevel(samplerIrradiance, normal, 0).rgb * ubo->scaleIBLAmbient;
    }

    const uint cascadeCount = clamp(ubo->ddgiCascadeCount, 1u, 4u);
    uint finest = cascadeCount;
    float finestWeight = 0.0;
    for (uint cascade = 0; cascade < cascadeCount; ++cascade)
    {
        finestWeight = ddgi_cascade_weight(ubo, cascade, worldPos);
        if (finestWeight > 0.0)
        {
            finest = cascade;
            break;
        }
    }
    if (finest == cascadeCount)
        return environment;

    float3 result = sample_ddgi_cascade(ubo, finest, worldPos, normal, irradianceAtlas, distanceAtlas, atlasSampler);
    if (finestWeight < 1.0)
    {
        // Near the cascade's faces: what the next one out says (itself fading into the environment at its own faces).
        float3 outer = environment;
        const uint next = finest + 1;
        if (next < cascadeCount)
        {
            const float nextWeight = ddgi_cascade_weight(ubo, next, worldPos);
            if (nextWeight > 0.0)
                outer = lerp(environment, sample_ddgi_cascade(ubo, next, worldPos, normal, irradianceAtlas, distanceAtlas, atlasSampler), nextWeight);
        }
        result = lerp(outer, result, finestWeight);
    }
    return result;
}
