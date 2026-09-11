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
float chebyshev_visibility(float distToProbe, float meanDist, float meanDistSq)
{
    float d = distToProbe - meanDist;
    if (d <= 0.0)
    {
        return 1.0;
    }

    // Variance floor: prevents division by zero while maintaining sharp shadows
    float variance = max(abs(meanDistSq - (meanDist * meanDist)), 0.0005);
    float pMax = variance / (variance + d * d);

    // Aggressive leak suppression threshold and power 4 falloff (matching RTXGI standard)
    pMax = saturate((pMax - 0.15) / 0.85);
    return pMax * pMax * pMax * pMax;
}

// =========================================================================
// Multi-Probe Trilinear & Chebyshev Interpolation for Surface Shading
// Samples the 8 nearest probes, weighting by trilinear position, surface
// orientation cosine, and Chebyshev depth visibility to completely eliminate
// light leaking through walls.
// =========================================================================
float3 sample_ddgi_diffuse(
    UniformBufferObject* ubo,
    float3 worldPos,
    float3 normal,
    Texture2D irradianceAtlas,
    Texture2D distanceAtlas,
    SamplerState atlasSampler
)
{
    float3 origin = ubo->ddgiGridOrigin.xyz;
    uint countX = uint(ubo->ddgiGridOrigin.w);
    float3 spacing = ubo->ddgiGridSpacing.xyz;
    uint countY = uint(ubo->ddgiGridSpacing.w);
    uint countZ = uint(ubo->ddgiGridParams.x);
    float normalBias = ubo->ddgiGridParams.w;

    // Shift sampling position along surface normal (isotropic in world units)
    float minSpacing = min(spacing.x, min(spacing.y, spacing.z));
    float3 biasedPos = worldPos + normal * (normalBias * minSpacing);

    float3 relativePos = (biasedPos - origin) / spacing;
    int3 maxIndices = int3(int(countX) - 2, int(countY) - 2, int(countZ) - 2);
    int3 baseCoord = clamp(int3(floor(relativePos)), int3(0, 0, 0), maxIndices);
    float3 fracCoord = saturate(relativePos - float3(baseCoord));

    uint probesPerRowIrr = uint(ubo->ddgiAtlasParams.x) / 10;
    uint probesPerRowDist = uint(ubo->ddgiAtlasParams.z) / 18;

    float3 totalIrradiance = float3(0.0);
    float totalWeight = 0.0;

    for (int dz = 0; dz <= 1; dz++)
    {
        for (int dy = 0; dy <= 1; dy++)
        {
            for (int dx = 0; dx <= 1; dx++)
            {
                uint3 probeCoord = uint3(baseCoord + int3(dx, dy, dz));
                uint probeIndex = get_probe_index(probeCoord, countX, countY);
                float3 probePos = get_probe_world_pos(probeIndex, origin, spacing, countX, countY);

                float3 toProbeVec = probePos - worldPos;
                float distToProbe = length(toProbeVec);
                float3 toProbeDir = distToProbe > 1e-4 ? toProbeVec / distToProbe : normal;

                // 1. Normal orientation test: Probes behind the surface tangent plane MUST NOT contribute
                float NdotL = dot(normal, toProbeDir);
                if (NdotL <= 0.0)
                {
                    continue;
                }
                float normalWeight = NdotL * NdotL;

                // 2. Trilinear interpolation factor
                float trilinear = (dx != 0 ? fracCoord.x : (1.0 - fracCoord.x)) *
                                  (dy != 0 ? fracCoord.y : (1.0 - fracCoord.y)) *
                                  (dz != 0 ? fracCoord.z : (1.0 - fracCoord.z));

                // 3. Chebyshev visibility test: does the probe have line of sight to the surface?
                float2 distUV = get_probe_atlas_uv(probeIndex, -toProbeDir, 16, probesPerRowDist, ubo->ddgiAtlasParams.zw);
                float2 moments = distanceAtlas.SampleLevel(atlasSampler, distUV, 0).rg;
                float visibility = chebyshev_visibility(distToProbe, moments.x, moments.y);

                float weight = trilinear * normalWeight * visibility;
                if (weight < 1e-5)
                {
                    continue;
                }

                // 4. Fetch probe irradiance along the surface normal
                float2 irrUV = get_probe_atlas_uv(probeIndex, normal, 8, probesPerRowIrr, ubo->ddgiAtlasParams.xy);
                float3 probeIrr = irradianceAtlas.SampleLevel(atlasSampler, irrUV, 0).rgb;

                totalIrradiance += probeIrr * weight;
                totalWeight += weight;
            }
        }
    }

    if (totalWeight > 1e-4)
    {
        return totalIrradiance / totalWeight;
    }

    return float3(0.0);
}

