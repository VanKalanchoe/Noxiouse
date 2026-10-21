#include "shaderIO.h"

// RTXPT-style log-average luminance pyramid. Mip 0 downsamples the current HDR
// image to 256x256; the remaining mips average 2x2 blocks down to one texel.
[shader("compute")]
[numthreads(8, 8, 1)]
void buildMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantAutoExposure pc)
{
    if (any(tid.xy >= pc.dimension)) return;

    Texture2D<float4> source = Texture2D<float4>.Handle(uint2(pc.sourceIndex, 0));
    SamplerState sampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    float2 uv = (float2(tid.xy) + 0.5) / float(pc.dimension);
    float3 color = max(source.SampleLevel(sampler, uv, 0.0).rgb, 0.0);
    float luminance = dot(color, float3(0.299, 0.587, 0.114));

    RWTexture2D<float> output = RWTexture2D<float>.Handle(uint2(pc.outputIndex, 0));
    output[tid.xy] = log2(max(luminance, 0.0001));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void reduceMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantAutoExposure pc)
{
    if (any(tid.xy >= pc.dimension)) return;

    Texture2D<float> source = Texture2D<float>.Handle(uint2(pc.sourceIndex, 0));
    uint2 p = tid.xy * 2;
    float average = source.Load(int3(p + uint2(0, 0), pc.sourceMip))
                  + source.Load(int3(p + uint2(1, 0), pc.sourceMip))
                  + source.Load(int3(p + uint2(0, 1), pc.sourceMip))
                  + source.Load(int3(p + uint2(1, 1), pc.sourceMip));

    RWTexture2D<float> output = RWTexture2D<float>.Handle(uint2(pc.outputIndex, 0));
    output[tid.xy] = average * 0.25;
}

// The eye adapts over time, not at once: the adapted log2 average (one texel, kept across frames) moves towards this frame's
// (the pyramid's last mip) by `adaptation` (1 - exp(-dt * speed), from the frame time). Without it the exposure jumped with every
// noisy frame and every bright object entering the view.
[shader("compute")]
[numthreads(1, 1, 1)]
void adaptMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantAutoExposure pc)
{
    Texture2D<float> source = Texture2D<float>.Handle(uint2(pc.sourceIndex, 0));
    const float current = source.Load(int3(0, 0, pc.sourceMip));
    RWTexture2D<float> adapted = RWTexture2D<float>.Handle(uint2(pc.outputIndex, 0));
    const float previous = adapted[uint2(0, 0)];
    const bool valid = pc.historyValid != 0 && isfinite(previous);
    adapted[uint2(0, 0)] = valid ? lerp(previous, current, saturate(pc.adaptation)) : current;
}
