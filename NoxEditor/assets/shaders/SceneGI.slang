// The diffuse light (irradiance / pi) at a surface point, for the passes that shade something other than the G-buffer's own
// pixels: forward-shaded transparent surfaces (decals, glass) and ray hits (RT reflections, RT glass refraction).
//
// Where the point is what the camera sees at its pixel -- a decal lying on its wall, a reflection or refraction hit that is on
// screen -- it is the screen-probe GI's per-pixel result there, the same light the deferred lighting gives that surface (so a
// decal matches its wall and a traced hit matches the surface seen directly). Elsewhere the DDGI cascades (the world radiance
// cache), else the environment's irradiance. Needs shaderIO.h and DDGICommon.slang first.
#pragma once

#include "DDGICommon.slang"

// The screen-probe GI of the surface at worldPos, if the camera sees that surface at the pixel worldPos lands on.
bool sample_screen_gi(UniformBufferObject* ubo, float3 worldPos, out float3 irradiance)
{
    irradiance = float3(0.0);
    if (ubo->screenGITextureIndex == 0xFFFFFFFF || ubo->screenGIDistanceTextureIndex == 0xFFFFFFFF)
        return false;

    // The G-buffer's pixels are the jittered projection's.
    const float4 clip = mul(ubo->proj, mul(ubo->view, float4(worldPos, 1.0)));
    if (clip.w <= 0.0)
        return false;
    const float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
    if (any(uv <= 0.0) || any(uv >= 1.0))
        return false;

    Texture2D<float> distanceTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + ubo->screenGIDistanceTextureIndex, 0));
    uint width, height;
    distanceTex.GetDimensions(width, height);
    const int2 pixel = min(int2(uv * float2(width, height)), int2(width, height) - 1);
    const float surfaceDistance = distanceTex.Load(int3(pixel, 0));
    if (surfaceDistance <= 0.0)
        return false;
    // The same point along the pixel's ray: a decal a little in front of its wall still is (1 %, at least 3 cm).
    const float distance = length(worldPos - ubo->cameraWorldPos.xyz);
    if (abs(distance - surfaceDistance) > max(0.01 * distance, 0.03 * ubo->unitsPerMeter))
        return false;

    Texture2D giTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->screenGITextureIndex, 0));
    irradiance = giTex.Load(int3(pixel, 0)).rgb;
    return true;
}

// The diffuse light at a surface: the screen-probe GI where the camera sees that surface, else DDGI, else the environment.
// toViewer: unit vector from the surface towards whoever sees it (the camera, a ray's origin). False when there is no GI at all
// (no DDGI and no environment map): the caller keeps its own ambient.
bool sample_scene_gi(UniformBufferObject* ubo, float3 worldPos, float3 shadingNormal, float3 geometricNormal, float3 toViewer,
                     out float3 irradiance)
{
    if (sample_screen_gi(ubo, worldPos, irradiance))
        return true;
    if (ubo->enableDDGI != 0 && ubo->ddgiIrradianceTextureIndex != 0xFFFFFFFF && ubo->ddgiDistanceTextureIndex != 0xFFFFFFFF)
    {
        Texture2D irradianceAtlas = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiIrradianceTextureIndex, 0));
        Texture2D distanceAtlas = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiDistanceTextureIndex, 0));
        SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
        irradiance = sample_ddgi_diffuse_surface(ubo, worldPos, shadingNormal, geometricNormal, toViewer, irradianceAtlas, distanceAtlas, linearClamp);
        return true;
    }
    if (ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        TextureCube irradianceTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState irradianceSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        irradiance = irradianceTex.SampleLevel(irradianceSampler, shadingNormal, 0).rgb * ubo->scaleIBLAmbient;
        return true;
    }
    irradiance = float3(0.0);
    return false;
}

// How much of the sky's reflection reaches a surface, judged by its diffuse light against the sky's: indoors, little. The same
// approximation the deferred lighting uses with DDGI / screen probes.
float scene_gi_specular_occlusion(UniformBufferObject* ubo, float3 irradiance)
{
    const float luma = dot(irradiance, float3(0.2126, 0.7152, 0.0722));
    return saturate(luma / max(ubo->scaleIBLAmbient, 0.01));
}
