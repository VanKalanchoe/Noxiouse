#include "shaderIO.h"
#include "DDGICommon.slang"

struct MSOutput
{
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    nointerpolation uint probeID : PROBE_ID;
};

static const uint SPHERE_LATS = 8;
static const uint SPHERE_LONS = 16;
static const uint SPHERE_VERT_COUNT = 114; // 1 + (8 - 1) * 16 + 1
static const uint SPHERE_TRI_COUNT = 224;  // 16 + (8 - 2) * 16 * 2 + 16

float3 get_sphere_vertex(uint v)
{
    if (v == 0)
    {
        return float3(0.0, 1.0, 0.0);
    }
    if (v == SPHERE_VERT_COUNT - 1)
    {
        return float3(0.0, -1.0, 0.0);
    }

    uint ringIdx = (v - 1) / SPHERE_LONS;
    uint lonIdx = (v - 1) % SPHERE_LONS;

    float theta = float(ringIdx + 1) * 3.14159265359 / float(SPHERE_LATS);
    float phi = float(lonIdx) * (2.0 * 3.14159265359) / float(SPHERE_LONS);

    float sinTheta = sin(theta);
    float cosTheta = cos(theta);
    float sinPhi = sin(phi);
    float cosPhi = cos(phi);

    return float3(cosPhi * sinTheta, cosTheta, sinPhi * sinTheta);
}

uint3 get_sphere_triangle(uint t)
{
    // 1. Top Cap (16 triangles): t in [0, 15]
    if (t < SPHERE_LONS)
    {
        uint lon = t;
        uint nextLon = (lon + 1) % SPHERE_LONS;
        uint vNorth = 0;
        uint vCurr = 1 + lon;
        uint vNext = 1 + nextLon;
        return uint3(vNorth, vCurr, vNext);
    }

    // 2. Bottom Cap (16 triangles): t in [208, 223]
    uint bottomCapStart = SPHERE_TRI_COUNT - SPHERE_LONS;
    if (t >= bottomCapStart)
    {
        uint lon = t - bottomCapStart;
        uint nextLon = (lon + 1) % SPHERE_LONS;
        uint vSouth = SPHERE_VERT_COUNT - 1;
        uint ringBase = 1 + (SPHERE_LATS - 2) * SPHERE_LONS;
        uint vCurr = ringBase + lon;
        uint vNext = ringBase + nextLon;
        return uint3(vSouth, vNext, vCurr);
    }

    // 3. Middle Bands (6 bands * 32 = 192 triangles): t in [16, 207]
    uint midT = t - SPHERE_LONS;
    uint bandIdx = midT / (SPHERE_LONS * 2);
    uint inBand = midT % (SPHERE_LONS * 2);
    uint lon = inBand / 2;
    uint isSecond = inBand % 2;

    uint nextLon = (lon + 1) % SPHERE_LONS;
    uint v0 = 1 + bandIdx * SPHERE_LONS + lon;
    uint v1 = 1 + bandIdx * SPHERE_LONS + nextLon;
    uint v2 = 1 + (bandIdx + 1) * SPHERE_LONS + lon;
    uint v3 = 1 + (bandIdx + 1) * SPHERE_LONS + nextLon;

    if (isSecond == 0)
    {
        return uint3(v0, v2, v1);
    }
    else
    {
        return uint3(v1, v2, v3);
    }
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void meshMain(
    out indices uint3 triangles[224],
    out vertices MSOutput vertices[114],
    uint threadID : SV_GroupIndex,
    uint3 groupID : SV_GroupID,
    uniform PushConstantDDGIDebug pushConstant
)
{
    uint probeIndex = groupID.x;
    if (probeIndex >= pushConstant.probeCountTotal)
    {
        SetMeshOutputCounts(0, 0);
        return;
    }

    SetMeshOutputCounts(114, 224);

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

    uint countX = uint(ubo->ddgiGridOrigin.w);
    uint countY = uint(ubo->ddgiGridSpacing.w);
    float3 gridOrigin = ubo->ddgiGridOrigin.xyz;
    float3 gridSpacing = ubo->ddgiGridSpacing.xyz;

    float3 probeWorldPos = get_probe_world_pos(probeIndex, gridOrigin, gridSpacing, countX, countY);

    for (uint v = threadID; v < 114; v += 32)
    {
        float3 localPos = get_sphere_vertex(v);
        float3 worldPos = probeWorldPos + localPos * pushConstant.sphereRadius;

        vertices[v].pos = mul(ubo->proj, mul(ubo->view, float4(worldPos, 1.0)));
        vertices[v].normal = localPos;
        vertices[v].probeID = probeIndex;
    }

    for (uint t = threadID; t < 224; t += 32)
    {
        triangles[t] = get_sphere_triangle(t);
    }
}

struct FragOutput
{
    float4 color : SV_Target0;
    int entityID : SV_Target1;
};

[shader("fragment")]
FragOutput fragMain(
    MSOutput input,
    uniform PushConstantDDGIDebug pushConstant
)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    float3 normal = normalize(input.normal);

    uint probesPerRowIrr = uint(ubo->ddgiAtlasParams.x) / 10;
    float2 irrUV = get_probe_atlas_uv(input.probeID, normal, 8, probesPerRowIrr, ubo->ddgiAtlasParams.xy);

    Texture2D irrAtlas = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.irradianceAtlasIndex, 0));
    SamplerState atlasSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));

    float3 probeColor = irrAtlas.SampleLevel(atlasSampler, irrUV, 0).rgb;

    FragOutput output;
    // Boost brightness slightly and add subtle ambient floor so spheres in shadow never vanish
    output.color = float4(probeColor * 1.5 + float3(0.015), 1.0);
    output.entityID = -1;
    return output;
}
