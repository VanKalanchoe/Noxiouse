#include "shaderIO.h"

// Sum-reduces one mip level of the light PDF texture from the previous (finer) mip. Dispatched once
// per mip level (1..N-1) from the CPU side. Reads the source mip via a regular bindless Texture2D
// (explicit mip parameter to .Load), writes the destination mip via its own per-mip storage slot --
// same split GI's PrefilterEnv.slang already established (sampled read + per-mip UAV write on the
// same underlying texture).

[shader("compute")]
[numthreads(8, 8, 1)]
void compMain(uint3 dispatchThreadID : SV_DispatchThreadID, uniform PushConstantReSTIRDIReduceLightPDFMip pushConstant)
{
    if (dispatchThreadID.x >= pushConstant.dstSize || dispatchThreadID.y >= pushConstant.dstSize)
        return;

    Texture2D<float> srcTex = Texture2D<float>.Handle(uint2(pushConstant.srcTextureIndex, 0));
    RWTexture2D<float> dstMip = RWTexture2D<float>.Handle(uint2(pushConstant.dstStorageIndex, 0));

    int2 srcPos = int2(dispatchThreadID.xy) * 2;
    int srcMip = int(pushConstant.srcMip);

    float sum = srcTex.Load(int3(srcPos + int2(0, 0), srcMip)).x
              + srcTex.Load(int3(srcPos + int2(0, 1), srcMip)).x
              + srcTex.Load(int3(srcPos + int2(1, 0), srcMip)).x
              + srcTex.Load(int3(srcPos + int2(1, 1), srcMip)).x;

    dstMip[dispatchThreadID.xy] = sum;
}
