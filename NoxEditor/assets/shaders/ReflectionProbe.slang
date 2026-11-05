#include "shaderIO.h"

// Reflection probe capture (RS4b; Renderer::addReflectionProbePasses): the six faces rendered from the probe (the forward lit pass of
// the editor's extra views) into the probe's cubemap, then its mip chain; PrefilterEnv.slang prefilters it like the sky's.

// The cube texel's direction: PrefilterEnv.slang's getCubemapVector (the faces' orientation every IBL cubemap here uses).
float3 probe_cube_direction(uint face, float2 uv)
{
    float3 dir;
    switch (face)
    {
    case 0: dir = float3(1.0, -uv.y, -uv.x); break;
    case 1: dir = float3(-1.0, -uv.y, uv.x); break;
    case 2: dir = float3(uv.x, 1.0, uv.y); break;
    case 3: dir = float3(uv.x, -1.0, -uv.y); break;
    case 4: dir = float3(uv.x, -uv.y, 1.0); break;
    default: dir = float3(-uv.x, -uv.y, -1.0); break;
    }
    return normalize(dir);
}

// Mip 0: each texel from the rendered face its direction falls in (by its largest axis), through that face's view and projection.
[shader("compute")]
[numthreads(16, 16, 1)]
void assembleMain(uint3 id : SV_DispatchThreadID, uniform PushConstantProbeAssemble pushConstant)
{
    if (id.x >= pushConstant.outputSize || id.y >= pushConstant.outputSize)
        return;
    const float2 st = (float2(id.xy) + 0.5) / float(pushConstant.outputSize) * 2.0 - 1.0;
    const float3 direction = probe_cube_direction(id.z, st);
    const float3 a = abs(direction);
    const uint view = a.x >= a.y && a.x >= a.z ? (direction.x >= 0.0 ? 0u : 1u) : (a.y >= a.z ? (direction.y >= 0.0 ? 2u : 3u) : (direction.z >= 0.0 ? 4u : 5u));
    float4x4* viewProjs = (float4x4*)pushConstant.facesReference;
    const float3 center = ((float4*)(pushConstant.facesReference + 6u * 64u))[0].xyz;
    const float4 clip = mul(viewProjs[view], float4(center + direction, 1.0));
    const float2 uv = saturate(float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5));
    Texture2D face = Texture2D.Handle(uint2(pushConstant.faceTextureIndex[view], 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(pushConstant.outputStorageIndex, 0));
    output[uint3(id.xy, id.z)] = float4(max(face.SampleLevel(linearClamp, uv, 0.0).rgb, float3(0.0)), 1.0);
}

// Mip outputMip from the one above (the cubemap sampled at its direction): the sources PrefilterEnv.slang's mip selection reads.
[shader("compute")]
[numthreads(16, 16, 1)]
void downsampleMain(uint3 id : SV_DispatchThreadID, uniform PushConstantProbeAssemble pushConstant)
{
    if (id.x >= pushConstant.outputSize || id.y >= pushConstant.outputSize)
        return;
    const float2 st = (float2(id.xy) + 0.5) / float(pushConstant.outputSize) * 2.0 - 1.0;
    TextureCube source = TextureCube.Handle(uint2(pushConstant.sourceTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(pushConstant.outputStorageIndex, 0));
    output[uint3(id.xy, id.z)] = float4(source.SampleLevel(linearClamp, probe_cube_direction(id.z, st), float(pushConstant.outputMip) - 1.0).rgb, 1.0);
}
