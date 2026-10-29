#include "shaderIO.h"
#include "DDGICommon.slang"

// The distance atlas: one workgroup per probe (RTXGI's blending), 16 x 16 threads -- one per interior texel. The probe's rays (their
// direction and clamped distance) are read into shared memory once and every texel blends from there; then the border texels copy
// the interior texels they mirror. Reading every ray from the ray data texture for every texel (as a full screen pass did) made the
// pass bound by those reads: 324 texels x 128 rays a probe.

#define DDGI_DISTANCE_SIDE 16
#define DDGI_DISTANCE_TOTAL 18
#define DDGI_MAX_RAYS 256

groupshared float3 gRayDirections[DDGI_MAX_RAYS];
groupshared float gRayDistances[DDGI_MAX_RAYS];
groupshared float2 gTexels[DDGI_DISTANCE_SIDE * DDGI_DISTANCE_SIDE];

// The interior texel a border texel of the octahedral layout repeats (a coordinate in [0, 17]).
uint2 ddgi_distance_border_source(uint2 localCoord)
{
    const uint probeSide = DDGI_DISTANCE_SIDE;
    const uint totalSide = DDGI_DISTANCE_TOTAL;
    uint2 interiorCoord = localCoord;
    if (localCoord.x == 0) interiorCoord.x = probeSide - localCoord.y + 1;
    else if (localCoord.x == totalSide - 1) interiorCoord.x = probeSide - localCoord.y + 1;

    if (localCoord.y == 0) interiorCoord.y = probeSide - localCoord.x + 1;
    else if (localCoord.y == totalSide - 1) interiorCoord.y = probeSide - localCoord.x + 1;

    return clamp(interiorCoord, uint2(1, 1), uint2(probeSide, probeSide));
}

// Border texel `index` (0..67) of a probe: the top and bottom rows, then the left and right columns between them.
uint2 ddgi_distance_border_texel(uint index)
{
    const uint totalSide = DDGI_DISTANCE_TOTAL;
    if (index < totalSide) return uint2(index, 0);
    index -= totalSide;
    if (index < totalSide) return uint2(index, totalSide - 1);
    index -= totalSide;
    if (index < totalSide - 2) return uint2(0, index + 1);
    index -= totalSide - 2;
    return uint2(totalSide - 1, index + 1);
}

[shader("compute")]
[numthreads(DDGI_DISTANCE_SIDE, DDGI_DISTANCE_SIDE, 1)]
void computeMain(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint threadIndex : SV_GroupIndex,
                 uniform PushConstantDDGIBlend pushConstant)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const uint probeIndex = group.x;
    if (probeIndex >= pushConstant.probeCountTotal)
        return;

    const uint totalSide = DDGI_DISTANCE_TOTAL;
    const uint threadCount = DDGI_DISTANCE_SIDE * DDGI_DISTANCE_SIDE;
    const uint2 origin = uint2(probeIndex % pushConstant.probesPerRow, probeIndex / pushConstant.probesPerRow) * totalSide;
    RWTexture2D<float2> atlas = RWTexture2D<float2>.Handle(uint2(pushConstant.outputStorageIndex, 0));
    const bool hasPrevious = pushConstant.firstFrame == 0 && pushConstant.prevAtlasTextureIndex != 0xFFFFFFFF;

    // Not updated this frame (ddgiUpdateStride): kept as it was.
    const bool updates = ddgi_probe_updates(ubo, probeIndex, pushConstant.frameIndex);
    const bool entered = ddgi_probe_entered(ubo, probeIndex);
    if (!updates && hasPrevious)
    {
        Texture2D keptAtlas = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.prevAtlasTextureIndex, 0));
        for (uint texel = threadIndex; texel < totalSide * totalSide; texel += threadCount)
        {
            const uint2 pixel = origin + uint2(texel % totalSide, texel / totalSide);
            atlas[pixel] = keptAtlas.Load(int3(pixel, 0)).rg;
        }
        return;
    }

    // The probe's rays, once: their directions (DDGIRadiance.slang's) and distances.
    Texture2D rayDataTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.rayDataTextureIndex, 0));
    const uint rayCount = min(pushConstant.raysPerProbe, DDGI_MAX_RAYS);
    float rotAngle = float((pushConstant.frameIndex + probeIndex * 17) % 128) * (2.0 * 3.14159265359 / 128.0);
    float3 rotAxis = normalize(float3(0.57735, 0.57735, 0.57735));
    for (uint r = threadIndex; r < rayCount; r += threadCount)
    {
        float4 raySample = rayDataTex.Load(int3(r, probeIndex, 0));
        gRayDirections[r] = rotate_vector(spherical_fibonacci(r, pushConstant.raysPerProbe), rotAxis, rotAngle);
        // Clamped to about a cell and a half (in world units): misses and far hits only need to read as "open". A back face
        // (negative, DDGIRadiance.slang) counts as a fifth of its distance, so what lies behind it reads as occluded.
        // Stored in meters: the atlas is 16-bit float, and squared centimeters overflow it.
        gRayDistances[r] = min(raySample.w < 0.0 ? -raySample.w * 0.2 : raySample.w, ddgi_max_distance(ubo, probeIndex)) / ubo->unitsPerMeter;
    }
    GroupMemoryBarrierWithGroupSync();

    // This thread's interior texel: its direction (octahedral UV in [-1, 1]).
    const uint2 interiorCoord = thread.xy + 1;
    float2 normalizedUV = (float2(interiorCoord - 1) + 0.5) / float(DDGI_DISTANCE_SIDE);
    float3 dir = oct_decode(normalizedUV * 2.0 - 1.0);

    float meanDist = 0.0;
    float meanDistSq = 0.0;
    float totalWeight = 0.0;
    for (uint r = 0; r < rayCount; r++)
    {
        float cosTheta = max(0.0, dot(dir, gRayDirections[r]));
        float weight = pow(cosTheta, 16.0); // Sharper directional weighting for distance moments
        float dist = gRayDistances[r];
        meanDist += dist * weight;
        meanDistSq += (dist * dist) * weight;
        totalWeight += weight;
    }

    float2 result = float2(meanDist, meanDistSq) / max(totalWeight, 1e-4);
    // Temporal Hysteresis Blending (a probe that just came into the volume starts over)
    const uint2 pixel = origin + interiorCoord;
    if (hasPrevious && !entered)
    {
        Texture2D prevAtlasTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.prevAtlasTextureIndex, 0));
        result = lerp(result, prevAtlasTex.Load(int3(pixel, 0)).rg, pushConstant.hysteresis);
    }
    atlas[pixel] = result;
    gTexels[threadIndex] = result;
    GroupMemoryBarrierWithGroupSync();

    // The border: each border texel repeats the interior texel across its octahedral seam (what it blended to on its own).
    for (uint border = threadIndex; border < 4 * (totalSide - 1); border += threadCount)
    {
        const uint2 localCoord = ddgi_distance_border_texel(border);
        const uint2 source = ddgi_distance_border_source(localCoord) - 1;
        atlas[origin + localCoord] = gTexels[source.x + source.y * DDGI_DISTANCE_SIDE];
    }
}
