#include "shaderIO.h"
#include "DDGICommon.slang"

// The irradiance atlas: one workgroup per probe (RTXGI's blending), 8 x 8 threads -- one per interior texel. The probe's rays are
// read into shared memory once and every texel blends from there; then the border texels copy the interior texels they mirror
// (DDGIBlendDistance.slang does the same for the distances).

#define DDGI_IRRADIANCE_SIDE 8
#define DDGI_IRRADIANCE_TOTAL 10
#define DDGI_MAX_RAYS 256

groupshared float3 gRayDirections[DDGI_MAX_RAYS];
groupshared float4 gRaySamples[DDGI_MAX_RAYS];
groupshared float4 gTexels[DDGI_IRRADIANCE_SIDE * DDGI_IRRADIANCE_SIDE];

uint2 ddgi_irradiance_border_source(uint2 localCoord)
{
    const uint probeSide = DDGI_IRRADIANCE_SIDE;
    const uint totalSide = DDGI_IRRADIANCE_TOTAL;
    uint2 interiorCoord = localCoord;
    // Border wrapping: flip coordinates diagonally across octahedral seams
    if (localCoord.x == 0) interiorCoord.x = probeSide - localCoord.y + 1;
    else if (localCoord.x == totalSide - 1) interiorCoord.x = probeSide - localCoord.y + 1;

    if (localCoord.y == 0) interiorCoord.y = probeSide - localCoord.x + 1;
    else if (localCoord.y == totalSide - 1) interiorCoord.y = probeSide - localCoord.x + 1;

    return clamp(interiorCoord, uint2(1, 1), uint2(probeSide, probeSide));
}

uint2 ddgi_irradiance_border_texel(uint index)
{
    const uint totalSide = DDGI_IRRADIANCE_TOTAL;
    if (index < totalSide) return uint2(index, 0);
    index -= totalSide;
    if (index < totalSide) return uint2(index, totalSide - 1);
    index -= totalSide;
    if (index < totalSide - 2) return uint2(0, index + 1);
    index -= totalSide - 2;
    return uint2(totalSide - 1, index + 1);
}

[shader("compute")]
[numthreads(DDGI_IRRADIANCE_SIDE, DDGI_IRRADIANCE_SIDE, 1)]
void computeMain(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint threadIndex : SV_GroupIndex,
                 uniform PushConstantDDGIBlend pushConstant)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const uint probeIndex = group.x;
    if (probeIndex >= pushConstant.probeCountTotal)
        return;

    const uint totalSide = DDGI_IRRADIANCE_TOTAL;
    const uint threadCount = DDGI_IRRADIANCE_SIDE * DDGI_IRRADIANCE_SIDE;
    const uint2 origin = uint2(probeIndex % pushConstant.probesPerRow, probeIndex / pushConstant.probesPerRow) * totalSide;
    RWTexture2D<float4> atlas = RWTexture2D<float4>.Handle(uint2(pushConstant.outputStorageIndex, 0));
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
            atlas[pixel] = keptAtlas.Load(int3(pixel, 0));
        }
        return;
    }

    // The probe's rays, once: their directions (DDGIRadiance.slang's) and what they brought back.
    Texture2D rayDataTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.rayDataTextureIndex, 0));
    const uint rayCount = min(pushConstant.raysPerProbe, DDGI_MAX_RAYS);
    float rotAngle = float((pushConstant.frameIndex + probeIndex * 17) % 128) * (2.0 * 3.14159265359 / 128.0);
    float3 rotAxis = normalize(float3(0.57735, 0.57735, 0.57735));
    for (uint r = threadIndex; r < rayCount; r += threadCount)
    {
        gRaySamples[r] = rayDataTex.Load(int3(r, probeIndex, 0));
        gRayDirections[r] = rotate_vector(spherical_fibonacci(r, pushConstant.raysPerProbe), rotAxis, rotAngle);
    }
    GroupMemoryBarrierWithGroupSync();

    // This thread's interior texel: its direction (octahedral UV in [-1, 1]).
    const uint2 interiorCoord = thread.xy + 1;
    float2 normalizedUV = (float2(interiorCoord - 1) + 0.5) / float(DDGI_IRRADIANCE_SIDE);
    float3 dir = oct_decode(normalizedUV * 2.0 - 1.0);

    float3 accumulatedRadiance = float3(0.0);
    float totalWeight = 0.0;
    uint backFaces = 0;
    for (uint r = 0; r < rayCount; r++)
    {
        float4 raySample = gRaySamples[r];
        // A back face (DDGIRadiance.slang): no light from inside the geometry.
        if (raySample.w < 0.0)
        {
            ++backFaces;
            continue;
        }
        float weight = max(0.0, dot(dir, gRayDirections[r]));
        accumulatedRadiance += raySample.rgb * weight;
        totalWeight += weight;
    }

    float3 currentIrradiance = accumulatedRadiance / max(totalWeight, 1e-4);
    // Classification (alpha): a probe that sees mostly back faces is buried in geometry and is switched off -- the surfaces around it
    // take their light from the probes outside instead of going dark next to every wall. Blended over time like the light, so a
    // probe near the threshold (thin curtains, window frames) fades instead of flipping on and off.
    float active = float(backFaces) > 0.25 * float(pushConstant.raysPerProbe) ? 0.0 : 1.0;

    // Temporal Hysteresis Blending (a probe that just came into the volume starts over)
    const uint2 pixel = origin + interiorCoord;
    float4 result = float4(currentIrradiance, active);
    if (hasPrevious && !entered)
    {
        Texture2D prevAtlasTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.prevAtlasTextureIndex, 0));
        const float4 previous = prevAtlasTex.Load(int3(pixel, 0));
        result = float4(lerp(currentIrradiance, previous.rgb, pushConstant.hysteresis), lerp(active, previous.a, pushConstant.hysteresis));
    }
    atlas[pixel] = result;
    gTexels[threadIndex] = result;
    GroupMemoryBarrierWithGroupSync();

    // The border: each border texel repeats the interior texel across its octahedral seam (what it blended to on its own).
    for (uint border = threadIndex; border < 4 * (totalSide - 1); border += threadCount)
    {
        const uint2 localCoord = ddgi_irradiance_border_texel(border);
        const uint2 source = ddgi_irradiance_border_source(localCoord) - 1;
        atlas[origin + localCoord] = gTexels[source.x + source.y * DDGI_IRRADIANCE_SIDE];
    }
}
