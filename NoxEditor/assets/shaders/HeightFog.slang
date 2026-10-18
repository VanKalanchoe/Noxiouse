#pragma once
#include "shaderIO.h"

// Exponential height fog (UE's Exponential Height Fog): fog whose density falls off exponentially with height, so it lies
// thick in valleys and at the horizon and thins upwards. Its optical depth along a view ray has a closed form, so it costs
// one exp per pixel. The light it scatters towards the camera is the environment's (its irradiance, so the fog takes the
// sky's color) times a tint, plus a glow towards the sun. Used by DeferredLighting.slang (the lit scene, at the surface's
// distance) and Skybox.slang (the sky, at "infinity": the fog then only depends on how much of it the ray crosses).
float3 applyHeightFog(UniformBufferObject* ubo, float3 color, float3 direction, float distance)
{
    if (ubo->heightFogDensity <= 0.0 || distance <= ubo->heightFogStartDistance)
        return color;

    // From where the fog starts, along the ray: the integral of density * exp(-falloff * direction.y * t) over the rest.
    const float3 origin = ubo->cameraWorldPos.xyz + direction * ubo->heightFogStartDistance;
    const float travel = min(distance - ubo->heightFogStartDistance, 1e30);
    const float falloff = max(ubo->heightFogFalloff, 1e-9);
    const float densityAtOrigin = ubo->heightFogDensity * exp(-falloff * (origin.y - ubo->heightFogBaseY));
    const float k = falloff * direction.y;
    // k * travel near 0 (a level ray, or no falloff): the integral is just the distance.
    const float opticalDepth = densityAtOrigin * (abs(k * travel) > 1e-4 ? (1.0 - exp(-k * travel)) / k : travel);
    const float fog = min(1.0 - exp(-max(opticalDepth, 0.0)), ubo->heightFogMaxOpacity);
    if (!(fog > 0.0))
        return color;

    float3 inscattered = ubo->heightFogColor;
    if (ubo->heightFogFromEnvironment != 0 && ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        // The environment's light around the view direction, from above the horizon (the sky lights the fog).
        TextureCube irradiance = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState irradianceSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        const float3 skyward = normalize(float3(direction.x, max(direction.y, 0.0) + 0.2, direction.z));
        inscattered *= irradiance.SampleLevel(irradianceSampler, skyward, 0.0).rgb * ubo->scaleIBLAmbient;
    }
    inscattered += ubo->heightFogSunColor * pow(saturate(dot(direction, ubo->heightFogSunDirection)), ubo->heightFogSunExponent);
    return lerp(color, inscattered, fog);
}
