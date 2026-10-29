#include "shaderIO.h"
#include "DDGICommon.slang"

struct DDGIRayDirectionsPayload
{
    uint dummy;
};
groupshared DDGIRayDirectionsPayload gPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gPayload);
}

struct MSOutput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

static const float2 uvs[3] = {
    float2(0.0, 0.0),
    float2(2.0, 0.0),
    float2(0.0, 2.0)
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
        vertices[threadID].uv = uvs[threadID];
    }
}

[shader("fragment")]
float4 fragMain(
    MSOutput input,
    float4 pixelCoord : SV_Position,
    uniform PushConstantDDGIBlend pushConstant
) : SV_Target0
{
    // The probes' ray directions, once a frame for all of them: a probe's rays are the spherical Fibonacci set turned by one of 128
    // angles ((frame + probe * 17) % 128, DDGIRadiance.slang), so this table holds every direction any probe uses -- x the ray, y the
    // angle. The blend passes read it instead of evaluating the same trigonometry for every texel of every probe and every ray.
    const uint ray = uint(pixelCoord.x);
    const uint rotation = uint(pixelCoord.y);
    float rotAngle = float(rotation) * (2.0 * 3.14159265359 / 128.0);
    float3 rotAxis = normalize(float3(0.57735, 0.57735, 0.57735));
    return float4(rotate_vector(spherical_fibonacci(ray, pushConstant.raysPerProbe), rotAxis, rotAngle), 0.0);
}
