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

// A probe's relocation (DDGIRelocation.slang) in world units, from an offsets texture (0xFFFFFFFF: none). Bounded to the 45 % of a
// cell relocation keeps to, so a texture not written yet (the first frames) cannot throw a probe out of its cell.
float3 ddgi_probe_offset(UniformBufferObject* ubo, uint storageIndex, uint offsetsTextureIndex)
{
    if (offsetsTextureIndex == 0xFFFFFFFF)
        return float3(0.0);
    Texture2D offsetsTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + offsetsTextureIndex, 0));
    const uint probesPerRow = max(uint(ubo->ddgiAtlasParams.x) / 10, 1u);
    float3 cells = offsetsTex.Load(int3(int(storageIndex % probesPerRow), int(storageIndex / probesPerRow), 0)).xyz;
    if (any(isnan(cells)) || any(isinf(cells)))
        cells = float3(0.0);
    return clamp(cells, -0.45, 0.45) * ddgi_cascade_spacing(ubo, ddgi_cascade_of(ubo, storageIndex));
}

// Where a probe is: its grid point plus its relocation of this frame's atlases (the uniforms').
float3 ddgi_probe_position(UniformBufferObject* ubo, uint storageIndex)
{
    const uint cascade = ddgi_cascade_of(ubo, storageIndex);
    return ddgi_cascade_origin(ubo, cascade) + float3(ddgi_local_coord(ubo, storageIndex)) * ddgi_cascade_spacing(ubo, cascade) +
           ddgi_probe_offset(ubo, storageIndex, ubo->ddgiProbeOffsetsTextureIndex);
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
// Multi-Probe Trilinear & Chebyshev Interpolation for Surface Shading (RTXGI's weighting)
// The 8 nearest probes of a cascade, weighted by trilinear position, a smooth backface term, Chebyshev depth visibility (no
// light leaking through walls) and classification (probes buried in geometry fade out). The offset and the backface term use
// the surface's geometric normal: a normal map moves the shading normal from pixel to pixel, and weights that follow it make
// the probes' light wiggle with the bumps. The shading normal only picks the direction the irradiance is read in.
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
    float3 shadingNormal,
    float3 geometricNormal,
    float3 toViewer,
    Texture2D irradianceAtlas,
    Texture2D distanceAtlas,
    SamplerState atlasSampler,
    uint offsetsTextureIndex // the probe offsets that go with these atlases (0xFFFFFFFF: the grid points)
)
{
    const uint3 counts = ddgi_counts(ubo);
    const float3 origin = ddgi_cascade_origin(ubo, cascade);
    const float3 spacing = ddgi_cascade_spacing(ubo, cascade);

    // The sampling point moves off the surface, mostly towards the viewer (RTXGI's view bias: the surface's own probes see it
    // from the front), by Normal Bias x 1.5 cells (0.2: 0.3 cells).
    const float minSpacing = min(spacing.x, min(spacing.y, spacing.z));
    const float3 biasedPos = worldPos + (geometricNormal * 0.2 + toViewer * 0.8) * (ubo->ddgiGridParams.w * 1.5 * minSpacing);

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
                const float3 probePos = origin + float3(probeCoord) * spacing + ddgi_probe_offset(ubo, probeIndex, offsetsTextureIndex);

                // 1. Backface term: probes behind the surface fade smoothly instead of dropping out at once.
                const float3 trueToProbe = probePos - worldPos;
                const float3 trueDir = dot(trueToProbe, trueToProbe) > 1e-8 ? normalize(trueToProbe) : geometricNormal;
                const float wrap = (dot(trueDir, geometricNormal) + 1.0) * 0.5;
                float weight = wrap * wrap + 0.2;

                // 2. Chebyshev visibility from the biased point (moments in meters, DDGIBlendDistance.slang).
                const float3 biasedToProbe = probePos - biasedPos;
                const float biasedDistance = length(biasedToProbe);
                const float3 biasedDir = biasedDistance > 1e-6 ? biasedToProbe / biasedDistance : geometricNormal;
                const float2 distUV = get_probe_atlas_uv(probeIndex, -biasedDir, 16, probesPerRowDist, ubo->ddgiAtlasParams.zw);
                const float2 moments = distanceAtlas.SampleLevel(atlasSampler, distUV, 0).rg;
                const float distanceMeters = biasedDistance / ubo->unitsPerMeter;
                float chebyshev = 1.0;
                if (distanceMeters > moments.x)
                {
                    const float variance = abs(moments.y - moments.x * moments.x);
                    const float d = distanceMeters - moments.x;
                    chebyshev = variance / (variance + d * d);
                    chebyshev = chebyshev * chebyshev * chebyshev;
                }
                weight *= max(0.05, chebyshev);

                // 3. Very small weights crushed further (keeps a barely visible probe from tinting the surface), then trilinear.
                weight = max(1e-6, weight);
                const float crushThreshold = 0.2;
                if (weight < crushThreshold)
                    weight *= (weight * weight) / (crushThreshold * crushThreshold);
                weight *= (dx != 0 ? fracCoord.x : (1.0 - fracCoord.x)) *
                          (dy != 0 ? fracCoord.y : (1.0 - fracCoord.y)) *
                          (dz != 0 ? fracCoord.z : (1.0 - fracCoord.z));

                // 4. Irradiance along the shading normal; alpha is the probe's classification (0: buried in geometry).
                const float2 irrUV = get_probe_atlas_uv(probeIndex, shadingNormal, 8, probesPerRowIrr, ubo->ddgiAtlasParams.xy);
                const float4 probeIrr = irradianceAtlas.SampleLevel(atlasSampler, irrUV, 0);
                weight *= saturate(probeIrr.a);

                totalIrradiance += probeIrr.rgb * weight;
                totalWeight += weight;
            }
        }
    }

    return totalWeight > 1e-6 ? totalIrradiance / totalWeight : float3(0.0);
}

// A surface's flat (geometric) normal from the depth buffer: the cross product of the position differences to the neighbours on
// the nearer side in x and in y, so a silhouette does not bend it; facing the shading normal's side. For DeferredLighting, whose
// G-buffer holds only the normal-mapped normal.
float3 ddgi_geometric_normal(Texture2D<float> depthTex, int2 pixel, float2 viewportSize, float4x4 invViewProj, float3 worldPos, float3 shadingNormal)
{
    const int2 maxPixel = int2(viewportSize) - 1;
    float3 positions[4];
    const int2 offsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    for (uint i = 0; i < 4; ++i)
    {
        const int2 neighbour = clamp(pixel + offsets[i], int2(0, 0), maxPixel);
        const float depth = depthTex.Load(int3(neighbour, 0)).r;
        const float2 uv = (float2(neighbour) + 0.5) / viewportSize;
        const float4 h = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
        positions[i] = depth > 0.0 ? h.xyz / h.w : worldPos + float3(1e6, 1e6, 1e6); // the sky: never the nearer side
    }
    const float3 left = worldPos - positions[0];
    const float3 right = positions[1] - worldPos;
    const float3 up = worldPos - positions[2];
    const float3 down = positions[3] - worldPos;
    const float3 dx = dot(left, left) < dot(right, right) ? left : right;
    const float3 dy = dot(up, up) < dot(down, down) ? up : down;
    float3 n = cross(dx, dy);
    const float lengthSq = dot(n, n);
    if (lengthSq < 1e-20 || dot(dx, dx) > 1e10 || dot(dy, dy) > 1e10)
        return shadingNormal;
    n *= rsqrt(lengthSq);
    return dot(n, shadingNormal) < 0.0 ? -n : n;
}

// The diffuse irradiance (/ pi, as the lighting reads it) at a surface: from the finest cascade around it, handing over to the next
// coarser one over its last cell, and to the environment's irradiance (what the IBL path uses) beyond the coarsest.
// toViewer: unit vector from the surface towards whoever sees it (the camera; a probe ray's origin for its hits).
// offsetsTextureIndex: the probe offsets that go with the atlases (0xFFFFFFFF: the grid points).
float3 sample_ddgi_diffuse_surface(
    UniformBufferObject* ubo,
    float3 worldPos,
    float3 shadingNormal,
    float3 geometricNormal,
    float3 toViewer,
    Texture2D irradianceAtlas,
    Texture2D distanceAtlas,
    SamplerState atlasSampler,
    uint offsetsTextureIndex
)
{
    float3 environment = float3(0.0);
    if (ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        TextureCube irradianceTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState samplerIrradiance = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        environment = irradianceTex.SampleLevel(samplerIrradiance, shadingNormal, 0).rgb * ubo->scaleIBLAmbient;
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

    float3 result = sample_ddgi_cascade(ubo, finest, worldPos, shadingNormal, geometricNormal, toViewer, irradianceAtlas, distanceAtlas, atlasSampler,
                                        offsetsTextureIndex);
    if (finestWeight < 1.0)
    {
        // Near the cascade's faces: what the next one out says (itself fading into the environment at its own faces).
        float3 outer = environment;
        const uint next = finest + 1;
        if (next < cascadeCount)
        {
            const float nextWeight = ddgi_cascade_weight(ubo, next, worldPos);
            if (nextWeight > 0.0)
                outer = lerp(environment, sample_ddgi_cascade(ubo, next, worldPos, shadingNormal, geometricNormal, toViewer, irradianceAtlas,
                                                              distanceAtlas, atlasSampler, offsetsTextureIndex), nextWeight);
        }
        result = lerp(outer, result, finestWeight);
    }
    return result;
}

// This frame's atlases (the uniforms'), with their probe offsets.
float3 sample_ddgi_diffuse_surface(
    UniformBufferObject* ubo,
    float3 worldPos,
    float3 shadingNormal,
    float3 geometricNormal,
    float3 toViewer,
    Texture2D irradianceAtlas,
    Texture2D distanceAtlas,
    SamplerState atlasSampler
)
{
    return sample_ddgi_diffuse_surface(ubo, worldPos, shadingNormal, geometricNormal, toViewer, irradianceAtlas, distanceAtlas, atlasSampler,
                                       ubo->ddgiProbeOffsetsTextureIndex);
}

// The common case: one normal for both, seen from the camera.
float3 sample_ddgi_diffuse(
    UniformBufferObject* ubo,
    float3 worldPos,
    float3 normal,
    Texture2D irradianceAtlas,
    Texture2D distanceAtlas,
    SamplerState atlasSampler
)
{
    const float3 toCamera = ubo->cameraWorldPos.xyz - worldPos;
    const float3 toViewer = dot(toCamera, toCamera) > 1e-8 ? normalize(toCamera) : normal;
    return sample_ddgi_diffuse_surface(ubo, worldPos, normal, normal, toViewer, irradianceAtlas, distanceAtlas, atlasSampler);
}
