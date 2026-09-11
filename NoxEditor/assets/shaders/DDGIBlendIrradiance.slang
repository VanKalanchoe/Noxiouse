#include "shaderIO.h"
#include "DDGICommon.slang"

struct DDGIBlendPayload
{
    uint dummy;
};
groupshared DDGIBlendPayload gPayload;

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
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    uint2 pixel = uint2(pixelCoord.xy);

    const uint probeSide = 8;
    const uint totalSide = 10;

    uint probeX = pixel.x / totalSide;
    uint probeY = pixel.y / totalSide;
    uint probeIndex = probeX + probeY * pushConstant.probesPerRow;

    if (probeIndex >= pushConstant.probeCountTotal)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    uint2 localCoord = uint2(pixel.x % totalSide, pixel.y % totalSide);

    // Resolve border texels by wrapping to octahedral interior
    bool isBorder = (localCoord.x == 0 || localCoord.x == totalSide - 1 ||
                     localCoord.y == 0 || localCoord.y == totalSide - 1);

    uint2 interiorCoord = localCoord;
    if (isBorder)
    {
        // Border wrapping: flip coordinates diagonally across octahedral seams
        if (localCoord.x == 0) interiorCoord.x = probeSide - localCoord.y + 1;
        else if (localCoord.x == totalSide - 1) interiorCoord.x = probeSide - localCoord.y + 1;

        if (localCoord.y == 0) interiorCoord.y = probeSide - localCoord.x + 1;
        else if (localCoord.y == totalSide - 1) interiorCoord.y = probeSide - localCoord.x + 1;

        interiorCoord = clamp(interiorCoord, uint2(1, 1), uint2(probeSide, probeSide));
    }

    // Map interior coord [1, 8] to normalized octahedral UV [-1, 1]
    float2 normalizedUV = (float2(interiorCoord - 1) + 0.5) / float(probeSide);
    float3 dir = oct_decode(normalizedUV * 2.0 - 1.0);

    // Read rays for this probe and evaluate cosine-weighted irradiance
    Texture2D rayDataTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.rayDataTextureIndex, 0));

    float3 accumulatedRadiance = float3(0.0);
    float totalWeight = 0.0;

    float rotAngle = float((pushConstant.frameIndex + probeIndex * 17) % 128) * (2.0 * 3.14159265359 / 128.0);
    float3 rotAxis = normalize(float3(0.57735, 0.57735, 0.57735));

    for (uint r = 0; r < pushConstant.raysPerProbe; r++)
    {
        float4 raySample = rayDataTex.Load(int3(r, probeIndex, 0));
        float3 rayDir = rotate_vector(spherical_fibonacci(r, pushConstant.raysPerProbe), rotAxis, rotAngle);

        float weight = max(0.0, dot(dir, rayDir));
        accumulatedRadiance += raySample.rgb * weight;
        totalWeight += weight;
    }

    float3 currentIrradiance = accumulatedRadiance / max(totalWeight, 1e-4);

    // Temporal Hysteresis Blending
    if (pushConstant.firstFrame != 0 || pushConstant.prevAtlasTextureIndex == 0xFFFFFFFF)
    {
        return float4(currentIrradiance, 1.0);
    }

    Texture2D prevAtlasTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.prevAtlasTextureIndex, 0));
    float3 prevIrradiance = prevAtlasTex.Load(int3(pixel, 0)).rgb;

    float3 blended = lerp(currentIrradiance, prevIrradiance, pushConstant.hysteresis);
    return float4(blended, 1.0);
}
