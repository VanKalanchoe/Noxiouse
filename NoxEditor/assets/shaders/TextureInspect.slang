#include "shaderIO.h"

// Render graph texture inspection (editor): converts one mip of any graph texture into a displayable RGBA8 image and
// copies the raw value of one texel into a 1x1 probe texture for CPU readback.

// Distinct color per integer id.
float3 HashColor(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return float3(value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF) / 255.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compMain(uint3 dispatchThreadID : SV_DispatchThreadID, uniform PushConstantTextureInspect pushConstant)
{
    if (dispatchThreadID.x >= pushConstant.width || dispatchThreadID.y >= pushConstant.height)
        return;

    int3 location = int3(dispatchThreadID.xy, int(pushConstant.mip));
    float4 raw = float4(0.0, 0.0, 0.0, 0.0);
    float3 color = float3(0.0, 0.0, 0.0);

    if (pushConstant.mode == TEXTURE_INSPECT_SIGNED_ID)
    {
        // Entity ids: -1 (nothing) stays black.
        Texture2D<int4> source = Texture2D<int4>.Handle(uint2(pushConstant.sourceTextureIndex, 0));
        int id = source.Load(location).r;
        raw = float4(float(id), 0.0, 0.0, 0.0);
        color = id < 0 ? float3(0.0, 0.0, 0.0) : HashColor(uint(id) + 1u);
    }
    else if (pushConstant.mode == TEXTURE_INSPECT_UNSIGNED_PAIR)
    {
        // Visibility buffer: one color per (instance, meshlet/triangle) pair; cleared texels stay black.
        Texture2D<uint4> source = Texture2D<uint4>.Handle(uint2(pushConstant.sourceTextureIndex, 0));
        uint4 value = source.Load(location);
        raw = float4(float(value.r), float(value.g), 0.0, 0.0);
        color = (value.r == 0 && value.g == 0) ? float3(0.0, 0.0, 0.0) : HashColor(value.r * 7919u + value.g);
    }
    else if (pushConstant.mode == TEXTURE_INSPECT_DEPTH)
    {
        // Reverse-Z: near = 1, far -> 0; a power curve keeps distant geometry visible.
        Texture2D<float> source = Texture2D<float>.Handle(uint2(pushConstant.sourceTextureIndex, 0));
        float depth = source.Load(location);
        raw = float4(depth, 0.0, 0.0, 0.0);
        color = pow(saturate(depth), 0.2).xxx * float3((pushConstant.channelMask & 1u) != 0 ? 1.0 : 0.0,
                                                       (pushConstant.channelMask & 2u) != 0 ? 1.0 : 0.0,
                                                       (pushConstant.channelMask & 4u) != 0 ? 1.0 : 0.0);
    }
    else
    {
        Texture2D source = Texture2D.Handle(uint2(pushConstant.sourceTextureIndex, 0));
        raw = source.Load(location);
        float4 scaled = raw * pushConstant.exposure;
        float4 mask = float4((pushConstant.channelMask & 1u) != 0 ? 1.0 : 0.0, (pushConstant.channelMask & 2u) != 0 ? 1.0 : 0.0,
                             (pushConstant.channelMask & 4u) != 0 ? 1.0 : 0.0, (pushConstant.channelMask & 8u) != 0 ? 1.0 : 0.0);

        if (pushConstant.channelMask == 8u)
            color = raw.aaa; // alpha only
        else if (pushConstant.channelCount == 1)
            color = scaled.rrr * mask.r;
        else if (pushConstant.channelCount == 2)
            color = float3(scaled.r * mask.r, scaled.g * mask.g, 0.0);
        else
            color = scaled.rgb * mask.rgb;
    }

    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(pushConstant.outputStorageIndex, 0));
    output[dispatchThreadID.xy] = float4(saturate(color), 1.0);

    if (int(dispatchThreadID.x) == pushConstant.probeX && int(dispatchThreadID.y) == pushConstant.probeY)
    {
        RWTexture2D<float4> probe = RWTexture2D<float4>.Handle(uint2(pushConstant.probeStorageIndex, 0));
        probe[uint2(0, 0)] = raw;
    }
}
