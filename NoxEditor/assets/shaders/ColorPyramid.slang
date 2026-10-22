#include "shaderIO.h"

// The lit scene behind transparent surfaces, for their refraction (Renderer::addForward3DPass, Hybrid_Renderer_Plan_2026-10.md
// §4 H3b): mip 0 a copy of the HDR scene (opaque surfaces, unlit, sky), every further mip a 2x2 average of the one before, so
// rough glass reads a blurrier mip.

[shader("compute")]
[numthreads(8, 8, 1)]
void copyMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantColorPyramid pc)
{
    if (any(tid.xy >= uint2(pc.width, pc.height)))
        return;

    Texture2D<float4> source = Texture2D<float4>.Handle(uint2(pc.sourceIndex, 0));
    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(pc.outputIndex, 0));
    // A few pixels as bright as the sun's disc must not spread over the whole blur (and half floats end at 65504).
    float4 color = source.Load(int3(tid.xy, 0));
    output[tid.xy] = float4(clamp(color.rgb, 0.0, 60000.0), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void downsampleMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantColorPyramid pc)
{
    if (any(tid.xy >= uint2(pc.width, pc.height)))
        return;

    Texture2D<float4> source = Texture2D<float4>.Handle(uint2(pc.sourceIndex, 0));
    uint sourceWidth, sourceHeight, levels;
    source.GetDimensions(pc.sourceMip, sourceWidth, sourceHeight, levels);
    const int2 last = int2(sourceWidth, sourceHeight) - 1;
    const int2 p = int2(tid.xy) * 2;
    float4 sum = source.Load(int3(min(p, last), pc.sourceMip))
               + source.Load(int3(min(p + int2(1, 0), last), pc.sourceMip))
               + source.Load(int3(min(p + int2(0, 1), last), pc.sourceMip))
               + source.Load(int3(min(p + int2(1, 1), last), pc.sourceMip));

    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(pc.outputIndex, 0));
    output[tid.xy] = sum * 0.25;
}
