// Screen-probe GI, 1/5: where this frame's probes stand (ScreenProbeCommon.slang). One output texel per tile: a pixel of the
// tile that is not sky, at an offset that moves every frame (over a few frames the probes visit the whole tile, so thin
// objects and corners get probes of their own and the temporal accumulation averages over the tile). Writes the probe's
// world position (w: 1, or 0 for a tile of sky) and the flat normal of the surface there, which its rays are around.
#include "ScreenProbeCommon.slang"

struct ProbePlaceOutput
{
    float4 position;
    float4 normal;
};

ProbePlaceOutput screen_probe_place(uint2 probe, PushConstantScreenProbe pushConstant)
{
    ProbePlaceOutput output;
    output.position = float4(0.0);
    output.normal = float4(0.0, 1.0, 0.0, 0.0);

    if (probe.x >= pushConstant.probeCountX || probe.y >= pushConstant.probeCountY)
        return output;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));

    // The frame's offset in the tile (R2 sequence), then fallbacks when that pixel is sky: the tile's centre and its quarters.
    const float tile = float(pushConstant.tileSize);
    const float2 jitter = screen_probe_jitter(pushConstant.frameIndex);
    const float2 candidates[5] = { jitter, float2(0.5, 0.5), float2(0.25, 0.25), float2(0.75, 0.75), float2(0.75, 0.25) };
    const int2 maxPixel = int2(pushConstant.viewportSize) - 1;

    for (uint i = 0; i < 5; ++i)
    {
        const int2 pixel = min(int2(float2(probe) * tile + candidates[i] * tile), maxPixel);
        const float depth = depthTex.Load(int3(pixel, 0));
        if (depth <= 0.0) // reverse Z: the sky
            continue;

        const float3 position = screen_probe_world_position(pushConstant.invViewProj, float2(pixel) + 0.5, pushConstant.viewportSize, depth);
        const float3 rawNormal = normalTex.Load(int3(pixel, 0)).xyz;
        const float lengthSq = dot(rawNormal, rawNormal);
        const float3 shadingNormal = lengthSq > 1e-8 ? rawNormal * rsqrt(lengthSq) : float3(0.0, 1.0, 0.0);
        // The surface's own plane, not its normal map: the probe's hemisphere is what the surface can see.
        const float3 normal = ddgi_geometric_normal(depthTex, pixel, pushConstant.viewportSize, pushConstant.invViewProj, position, shadingNormal);

        output.position = float4(position, 1.0);
        output.normal = float4(normal, 0.0);
        return output;
    }
    return output;
}

[shader("compute")]
[numthreads(SCREEN_PROBE_GROUP_SIZE, SCREEN_PROBE_GROUP_SIZE, 1)]
void computeMain(uint3 id : SV_DispatchThreadID, uniform PushConstantScreenProbe pushConstant)
{
    if (!screen_probe_in_target(pushConstant.outputStorageIndex0, id.xy))
        return;
    const ProbePlaceOutput output = screen_probe_place(id.xy, pushConstant);
    screen_probe_store(pushConstant.outputStorageIndex0, id.xy, output.position);
    screen_probe_store(pushConstant.outputStorageIndex1, id.xy, output.normal);
}
