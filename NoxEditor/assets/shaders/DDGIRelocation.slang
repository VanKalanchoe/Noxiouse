#include "shaderIO.h"
#include "DDGICommon.slang"

// DDGI probe relocation (RTXGI's): a probe on the grid can end up inside a wall or right against a surface, where it sees back
// faces or nothing but the surface, and the light next to it goes dark or leaks. Each updated probe looks at the rays it traced
// this frame (DDGIRadiance.slang: negative distance = a back face) and moves, at most 45 % of a cell from its grid point:
//   - out through the nearest back face when a quarter of its rays or more hit back faces (it is inside something);
//   - away from a front face closer than a fifth of a cell, towards its farthest free direction;
//   - otherwise back towards its grid point, as far as the free space in front of it allows.
// One texel per probe (DDGI_PROBES_PER_ROW a row): its offset in cells of its cascade. The atlases and the lighting read it
// (DDGICommon.slang ddgi_probe_offset); the next frame's rays start there.

struct DDGIRelocationPayload
{
    uint dummy;
};
groupshared DDGIRelocationPayload gPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gPayload);
}

struct MSOutput
{
    float4 pos : SV_Position;
};

static const float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void meshMain(
    out indices uint3 triangles[1],
    out vertices MSOutput vertices[3],
    uint threadID : SV_GroupIndex
)
{
    SetMeshOutputCounts(3, 1);

    if (threadID < 3)
    {
        triangles[0] = uint3(0, 1, 2);
        vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
    }
}

[shader("fragment")]
float4 fragMain(MSOutput input, uniform PushConstantDDGIBlend pushConstant) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const uint2 pixel = uint2(input.pos.xy);
    const uint probeIndex = pixel.x + pixel.y * pushConstant.probesPerRow;
    if (probeIndex >= pushConstant.probeCountTotal || ddgi_probe_entered(ubo, probeIndex))
        return float4(0.0); // on its grid point

    float3 previous = float3(0.0);
    if (pushConstant.firstFrame == 0 && pushConstant.prevAtlasTextureIndex != 0xFFFFFFFF)
    {
        Texture2D previousTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.prevAtlasTextureIndex, 0));
        previous = previousTex.Load(int3(pixel, 0)).xyz;
        if (any(isnan(previous)) || any(isinf(previous)))
            previous = float3(0.0);
        previous = clamp(previous, -0.45, 0.45);
    }
    if (!ddgi_probe_updates(ubo, probeIndex, pushConstant.frameIndex))
        return float4(previous, 0.0); // traced nothing this frame

    const float3 spacing = ddgi_cascade_spacing(ubo, ddgi_cascade_of(ubo, probeIndex));
    const float minFrontfaceDistance = 0.2 * min(spacing.x, min(spacing.y, spacing.z));
    const float3 offset = previous * spacing;

    Texture2D rayDataTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.rayDataTextureIndex, 0));
    const float rotAngle = float((pushConstant.frameIndex + probeIndex * 17) % 128) * (2.0 * 3.14159265359 / 128.0);
    const float3 rotAxis = normalize(float3(0.57735, 0.57735, 0.57735));

    uint backfaces = 0;
    float closestBackfaceDistance = 1e27;
    float3 closestBackfaceDirection = float3(0.0);
    float closestFrontfaceDistance = 1e27;
    float3 closestFrontfaceDirection = float3(0.0);
    float farthestFrontfaceDistance = 0.0;
    float3 farthestFrontfaceDirection = float3(0.0);
    for (uint r = 0; r < pushConstant.raysPerProbe; ++r)
    {
        const float distance = rayDataTex.Load(int3(r, probeIndex, 0)).w; // a miss is far (DDGIRadiance.slang)
        const float3 direction = rotate_vector(spherical_fibonacci(r, pushConstant.raysPerProbe), rotAxis, rotAngle);
        if (distance < 0.0)
        {
            ++backfaces;
            if (-distance < closestBackfaceDistance)
            {
                closestBackfaceDistance = -distance;
                closestBackfaceDirection = direction;
            }
            continue;
        }
        if (distance < closestFrontfaceDistance)
        {
            closestFrontfaceDistance = distance;
            closestFrontfaceDirection = direction;
        }
        if (distance > farthestFrontfaceDistance)
        {
            farthestFrontfaceDistance = distance;
            farthestFrontfaceDirection = direction;
        }
    }

    float3 moved = offset;
    if (float(backfaces) >= 0.25 * float(pushConstant.raysPerProbe))
    {
        // Inside geometry: through the nearest back face, a little past it.
        moved += closestBackfaceDirection * (closestBackfaceDistance + 0.5 * minFrontfaceDistance);
    }
    else if (closestFrontfaceDistance < minFrontfaceDistance)
    {
        // Against a surface: away from it, unless the free side is the same way (a corner: stay).
        if (dot(closestFrontfaceDirection, farthestFrontfaceDirection) <= 0.0)
            moved += farthestFrontfaceDirection * min(farthestFrontfaceDistance, minFrontfaceDistance);
    }
    else if (closestFrontfaceDistance > minFrontfaceDistance * 1.05)
    {
        // Free all around: back towards its grid point as far as the room in front allows.
        const float offsetLength = length(offset);
        if (offsetLength > 1e-6)
            moved -= offset / offsetLength * min(closestFrontfaceDistance - minFrontfaceDistance, offsetLength);
    }

    const float3 movedCells = moved / spacing;
    if (all(abs(movedCells) <= 0.45))
        return float4(movedCells, 0.0);
    return float4(previous, 0.0);
}
