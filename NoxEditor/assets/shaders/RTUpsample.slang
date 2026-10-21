// Ray traced results traced at a lower ray tracing resolution (Settings > Ray Tracing Resolution; rt_trace_pixel in shaderIO.h) back to
// the render resolution, before their denoisers. Each texel of the reduced result traced one pixel of its block, which one moving
// every frame. A pixel takes the traced samples around it that lie on its own surface -- depth within a few percent, normal
// agreeing -- the nearest weighing most: blended (reflections, AO; their alpha, a hit distance, from the best match only) or the
// best match alone (the shadow mask: its penumbra distance does not average). The denoisers' temporal accumulation then sees
// every pixel traced over the frames.
#include "shaderIO.h"

struct RTUpsamplePayload
{
    uint dummy;
};
groupshared RTUpsamplePayload gRTUpsamplePayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gRTUpsamplePayload);
}

struct RTUpsampleVertex
{
    float4 pos : SV_Position;
};

static const float2 RTUpsampleTriangle[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void meshMain(out indices uint3 triangles[1], out vertices RTUpsampleVertex vertices[3], uint threadID : SV_GroupIndex)
{
    SetMeshOutputCounts(3, 1);
    if (threadID < 3)
    {
        vertices[threadID].pos = float4(RTUpsampleTriangle[threadID], 0.0, 1.0);
        if (threadID == 0)
            triangles[0] = uint3(0, 1, 2);
    }
}

float3 rt_upsample_normal(Texture2D normalTex, int2 pixel)
{
    const float3 raw = normalTex.Load(int3(pixel, 0)).xyz;
    const float lengthSq = dot(raw, raw);
    return lengthSq > 1e-8 ? raw * rsqrt(lengthSq) : float3(0.0, 1.0, 0.0);
}

[shader("fragment")]
float4 fragMain(RTUpsampleVertex input, uniform PushConstantRTUpsample pushConstant) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    Texture2D source = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.sourceTextureIndex, 0));
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));

    const int2 pixel = int2(input.pos.xy);
    const int subsample = int(max(ubo->rtSubsample, 1u));
    const int2 sourceMax = int2(pushConstant.sourceSize) - 1;
    const int2 pixelMax = int2(pushConstant.viewportSize) - 1;
    const int2 block = min(pixel / subsample, sourceMax);

    const float depth = depthTex.Load(int3(pixel, 0));
    if (depth <= 0.0) // the sky: nothing traced it, whatever the pass wrote there
        return source.Load(int3(block, 0));
    const float3 normal = rt_upsample_normal(normalTex, pixel);

    float4 sum = float4(0.0);
    float weightSum = 0.0;
    float bestWeight = -1.0;
    float4 best = source.Load(int3(block, 0));
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 candidate = block + int2(dx, dy);
            if (any(candidate < 0) || any(candidate > sourceMax))
                continue;
            const int2 traced = min(candidate * subsample + rt_block_offset(ubo, candidate), pixelMax);
            const float tracedDepth = depthTex.Load(int3(traced, 0));
            if (tracedDepth <= 0.0)
                continue;
            // Reverse Z is about 1 / distance: its relative difference is the distances'.
            const float depthDifference = abs(tracedDepth - depth) / max(depth, 1e-8);
            const float facing = saturate(dot(rt_upsample_normal(normalTex, traced), normal));
            const float2 away = float2(traced - pixel);
            const float weight = exp(-0.5 * dot(away, away) / float(subsample * subsample)) * exp(-depthDifference * 50.0) *
                                 pow(facing, 8.0);
            const float4 value = source.Load(int3(candidate, 0));
            sum += value * weight;
            weightSum += weight;
            if (weight > bestWeight)
            {
                bestWeight = weight;
                best = value;
            }
        }
    }

    if (pushConstant.mode == 1 || weightSum < 1e-5)
        return best;
    return float4(sum.rgb / weightSum, best.a);
}
