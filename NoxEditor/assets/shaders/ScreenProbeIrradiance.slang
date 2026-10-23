// Screen-probe GI, 3/5: each probe's rays convolved into irradiance (ScreenProbeCommon.slang). One output texel per direction of
// the probe's octahedral map (6 x 6 and a border, in world space so neighbouring probes line up texel for texel): the clamped
// cosine of that direction over the probe's rays. The rays cover the probe's hemisphere uniformly, so
//   E(d) / pi = 2 / N * sum(L_i * max(0, d . w_i))
// and directions below the probe's surface get only the light from above it, as they should. No ray is clamped: a few rays
// catching a sunlit patch through a small window are most of a dark room's light (the filters and the history average them).
#include "ScreenProbeCommon.slang"

[shader("fragment")]
float4 fragMain(ScreenProbeVertex input, uniform PushConstantScreenProbe pushConstant) : SV_Target0
{
    const uint2 texel = uint2(input.pos.xy);
    const uint2 probe = texel / ScreenProbeIrradianceRes;
    const uint2 local = texel % ScreenProbeIrradianceRes;
    if (probe.x >= pushConstant.probeCountX || probe.y >= pushConstant.probeCountY)
        return float4(0.0);

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    Texture2D positionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.probePositionTextureIndex, 0));
    if (positionTex.Load(int3(probe, 0)).w <= 0.0)
        return float4(0.0);
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.probeNormalTextureIndex, 0));
    const float3 probeNormal = normalTex.Load(int3(probe, 0)).xyz;
    Texture2D rays = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.inputTextureIndex, 0));

    const uint2 rayOrigin = probe * ScreenProbeRayRes;
    const float3 direction = screen_probe_interior_direction(screen_probe_border_source(local));
    float3 sum = float3(0.0);
    for (uint ray = 0; ray < ScreenProbeRayCount; ++ray)
    {
        const float3 radiance = rays.Load(int3(rayOrigin + uint2(ray % ScreenProbeRayRes, ray / ScreenProbeRayRes), 0)).rgb;
        const float3 rayDirection = screen_probe_ray_direction(probeNormal, probe, ray, pushConstant.frameIndex);
        sum += radiance * max(dot(direction, rayDirection), 0.0);
    }
    sum *= 2.0 / float(ScreenProbeRayCount);

    // The emissive meshes' samples (ScreenProbeTrace), with this direction's cosine.
    if (pushConstant.emissiveTextureIndex != 0xFFFFFFFF)
    {
        Texture2D emissive = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.emissiveTextureIndex, 0));
        for (uint y = 0; y < ScreenProbeRayRes; ++y)
        {
            for (uint x = 0; x < ScreenProbeRayRes; ++x)
            {
                const float4 sample = emissive.Load(int3(rayOrigin + uint2(x, y), 0));
                if (any(sample.rgb > 0.0))
                    sum += sample.rgb * max(dot(direction, screen_probe_unpack_direction(sample.a)), 0.0);
            }
        }
    }
    sum = all(isfinite(sum)) ? min(sum, float3(30000.0)) : float3(0.0);
    return float4(sum, 1.0);
}
