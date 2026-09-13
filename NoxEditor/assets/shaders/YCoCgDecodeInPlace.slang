#include "shaderIO.h"

// Decodes an NRD REBLUR-denoised texture's YCoCg color back to linear RGB, in place. REBLUR's own
// filtering requires radiance pre-encoded as YCoCg (see PathTracer.slang's LinearToYCoCg comment), and
// its output stays in that color space -- the application is expected to decode it back before any
// downstream consumer (DLSS, tonemapping) that doesn't already know about it. Safe to read-then-write
// the same texel per thread: no cross-thread dependency, so no race despite sharing one texture.

float3 YCoCgToLinear(float3 c)
{
    float Y = c.x;
    float Co = c.y;
    float Cg = c.z;
    return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compMain(uint3 dispatchThreadID : SV_DispatchThreadID, uniform PushConstantYCoCgDecode pushConstant)
{
    if (dispatchThreadID.x >= pushConstant.width || dispatchThreadID.y >= pushConstant.height)
        return;

    Texture2D readTex = Texture2D.Handle(uint2(pushConstant.readTextureIndex, 0));
    RWTexture2D<float4> writeTex = RWTexture2D<float4>.Handle(uint2(pushConstant.writeTextureIndex, 0));

    float4 sample = readTex.Load(int3(dispatchThreadID.xy, 0));
    writeTex[dispatchThreadID.xy] = float4(YCoCgToLinear(sample.rgb), sample.a);
}
