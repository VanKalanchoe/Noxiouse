#include "shaderIO.h"

// Depth-aware box blur over SSAO.slang's raw, hash-dithered output: a plain box blur would bleed occlusion across
// depth discontinuities (a foreground railing darkening the wall behind it); weighting samples out once their depth
// is too far from the center pixel's keeps the blur inside one actual surface.

struct SSAOBlurPayload
{
    uint dummy;
};
groupshared SSAOBlurPayload gPayload;

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
    uint threadID : SV_GroupIndex,
    uniform PushConstantSSAOBlur pushConstant
)
{
    SetMeshOutputCounts(3, 1);

    if (threadID < 3)
    {
        vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
        vertices[threadID].uv = uvs[threadID];
    }

    if (threadID == 0)
    {
        triangles[0] = uint3(0, 1, 2);
    }
}

struct SSAOBlurOutput
{
    float ao : SV_Target0;
};

[shader("fragment")]
SSAOBlurOutput fragMain(MSOutput input, uniform PushConstantSSAOBlur pushConstant)
{
    SSAOBlurOutput output;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    int2 pixelCoord = int2(input.pos.xy);

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float centerDepth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (centerDepth <= 0.0)
    {
        output.ao = 1.0;
        return output;
    }

    Texture2D<float> ssaoTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.ssaoTextureIndex, 0));

    const int radius = 2; // 5x5
    float total = 0.0;
    float weightSum = 0.0;
    for (int y = -radius; y <= radius; y++)
    {
        for (int x = -radius; x <= radius; x++)
        {
            int2 tapCoord = pixelCoord + int2(x, y);
            if (any(tapCoord < int2(0, 0)) || tapCoord.x >= int(pushConstant.viewportSize.x) || tapCoord.y >= int(pushConstant.viewportSize.y))
                continue;

            float tapDepth = depthTex.Load(int3(tapCoord, 0)).r;
            if (tapDepth <= 0.0)
                continue;

            // Reverse-Z: depth values are not linear, but a plain relative difference is still a reasonable,
            // cheap proxy for "is this roughly the same surface" at this pass's small (5x5) tap radius.
            float depthDelta = abs(tapDepth - centerDepth);
            float weight = depthDelta < 0.01 ? 1.0 : 0.0;

            total += ssaoTex.Load(int3(tapCoord, 0)).r * weight;
            weightSum += weight;
        }
    }

    output.ao = weightSum > 0.0 ? (total / weightSum) : ssaoTex.Load(int3(pixelCoord, 0)).r;
    return output;
}
