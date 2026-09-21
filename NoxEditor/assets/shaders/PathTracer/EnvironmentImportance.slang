#include "../shaderIO.h"
#include "RTXPT/EnvMap.hlsli"

[shader("compute")]
[numthreads(8,8,1)]
void buildMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantPTEnvironment pc)
{
    if (any(tid.xy >= pc.dimension)) return;
    TextureCube<float4> env = TextureCube<float4>.Handle(uint2(pc.sourceIndex,0));
    SamplerState sampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT,0));
    // Stratified integration of each equal-area cell; positive support also handles black maps.
    float weight=0;
    for (uint y=0; y<4; ++y)
    for (uint x=0; x<4; ++x)
    {
        float2 uv=(float2(tid.xy)+(float2(x,y)+0.5)/4.0)/float(pc.dimension);
        weight += Luminance(env.SampleLevel(sampler,oct_to_ndir_equal_area_unorm(uv),0).rgb);
    }
    RWTexture2D<float> dst = RWTexture2D<float>.Handle(uint2(pc.outputIndex,0));
    dst[tid.xy]=max(weight/16.0,1e-8);
}

[shader("compute")]
[numthreads(8,8,1)]
void reduceMain(uint3 tid : SV_DispatchThreadID, uniform PushConstantPTEnvironment pc)
{
    if (any(tid.xy >= pc.dimension)) return;
    Texture2D<float> src = Texture2D<float>.Handle(uint2(pc.sourceIndex,0));
    RWTexture2D<float> dst = RWTexture2D<float>.Handle(uint2(pc.outputIndex,0));
    int2 p=int2(tid.xy)*2;
    dst[tid.xy]=0.25*(src.Load(int3(p,pc.sourceMip))+src.Load(int3(p+int2(1,0),pc.sourceMip))+
        src.Load(int3(p+int2(0,1),pc.sourceMip))+src.Load(int3(p+int2(1,1),pc.sourceMip)));
}
