// Screen-probe GI, 4/5: spatial filter (ScreenProbeCommon.slang). Each irradiance texel averaged with the same direction's texel
// of the 3 x 3 probes around, weighted by how much they lie on the probe's surface and face its way (a probe on the wall behind
// or on the floor below is left out). Run filterPasses times (ping-pong): two passes reach the probes two tiles away.
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
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.probeNormalTextureIndex, 0));
    Texture2D irradiance = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.inputTextureIndex, 0));

    const float4 position = positionTex.Load(int3(probe, 0));
    if (position.w <= 0.0)
        return float4(0.0);
    const float3 normal = normalTex.Load(int3(probe, 0)).xyz;
    const float viewDistance = length(position.xyz - ubo->cameraWorldPos.xyz);

    float3 sum = irradiance.Load(int3(texel, 0)).rgb;
    float weightSum = 1.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 neighbour = int2(probe) + int2(dx, dy);
            if ((dx == 0 && dy == 0) || any(neighbour < 0) || neighbour.x >= int(pushConstant.probeCountX) ||
                neighbour.y >= int(pushConstant.probeCountY))
                continue;
            const float4 neighbourPosition = positionTex.Load(int3(neighbour, 0));
            if (neighbourPosition.w <= 0.0)
                continue;
            const float3 neighbourNormal = normalTex.Load(int3(neighbour, 0)).xyz;
            float weight = screen_probe_surface_weight(neighbourPosition.xyz, neighbourNormal, position.xyz, normal, viewDistance);
            weight *= (dx == 0 || dy == 0) ? 1.0 : 0.7; // the diagonal ones are farther
            if (weight < 0.01)
                continue;
            sum += irradiance.Load(int3(uint2(neighbour) * ScreenProbeIrradianceRes + local, 0)).rgb * weight;
            weightSum += weight;
        }
    }
    return float4(sum / weightSum, 1.0);
}
