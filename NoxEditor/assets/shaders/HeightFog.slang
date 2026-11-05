#pragma once
#include "shaderIO.h"

// Exponential height fog (UE's Exponential Height Fog): fog whose density falls off exponentially with height, so it lies
// thick in valleys and at the horizon and thins upwards. Its optical depth along a view ray has a closed form, so it costs
// one exp per pixel. The light it scatters towards the camera is the environment's (its irradiance, so the fog takes the
// sky's color) times a tint, plus a glow towards the sun. Used by DeferredLighting.slang (the lit scene, at the surface's
// distance), Skybox.slang (the sky, at "infinity": the fog then only depends on how much of it the ray crosses) and the forward
// passes (sceneFogTerms below).

// The height fog along a view ray up to `distance`: rgb the light it scatters towards the camera, a the transmittance of what is
// behind (fogged = color * a + rgb).
// Volumetric fog (VolumetricFog.slang; UniformBufferObject::volumetricFogTextureIndex): the light its froxels scatter towards the camera
// and their transmittance up to `distance` along the ray of screen position uv (rgb, a; nothing scattered, all transmitted without it).
float4 volumetricFogTerms(UniformBufferObject* ubo, float2 uv, float distance)
{
    if (ubo->volumetricFogTextureIndex == 0xFFFFFFFF || ubo->volumetricFogSlices == 0 || ubo->volumetricFogDistance <= 0.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    Texture2DArray volume = Texture2DArray.Handle(uint2(ubo->imageHeapIndexOffset + ubo->volumetricFogTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    const float slices = float(ubo->volumetricFogSlices);
    const float slice = sqrt(saturate(distance / ubo->volumetricFogDistance)) * slices - 1.0;
    if (slice < 0.0) // before the first slice ends: from nothing at the camera
        return lerp(float4(0.0, 0.0, 0.0, 1.0), volume.SampleLevel(linearClamp, float3(uv, 0.0), 0.0), slice + 1.0);
    const float lower = floor(slice);
    const float upper = min(lower + 1.0, slices - 1.0);
    return lerp(volume.SampleLevel(linearClamp, float3(uv, lower), 0.0), volume.SampleLevel(linearClamp, float3(uv, upper), 0.0), slice - lower);
}

float4 heightFogTerms(UniformBufferObject* ubo, float3 direction, float distance)
{
    // With the volumetric fog the height fog only starts where its volume ends (UE's volumetric fog distance).
    const float startDistance = ubo->volumetricFogTextureIndex != 0xFFFFFFFF ? max(ubo->heightFogStartDistance, ubo->volumetricFogDistance)
                                                                            : ubo->heightFogStartDistance;
    if (ubo->heightFogDensity <= 0.0 || distance <= startDistance)
        return float4(0.0, 0.0, 0.0, 1.0);

    // From where the fog starts, along the ray: the integral of density * exp(-falloff * direction.y * t) over the rest.
    const float3 origin = ubo->cameraWorldPos.xyz + direction * startDistance;
    const float travel = min(distance - startDistance, 1e30);
    const float falloff = max(ubo->heightFogFalloff, 1e-9);
    const float densityAtOrigin = ubo->heightFogDensity * exp(-falloff * (origin.y - ubo->heightFogBaseY));
    const float k = falloff * direction.y;
    // k * travel near 0 (a level ray, or no falloff): the integral is just the distance.
    const float opticalDepth = densityAtOrigin * (abs(k * travel) > 1e-4 ? (1.0 - exp(-k * travel)) / k : travel);
    const float fog = min(1.0 - exp(-max(opticalDepth, 0.0)), ubo->heightFogMaxOpacity);
    if (!(fog > 0.0))
        return float4(0.0, 0.0, 0.0, 1.0);

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
    return float4(inscattered * fog, 1.0 - fog);
}

// The volumetric fog's terms along a view direction (its screen position from the camera's projection).
float4 volumetricFogTermsAlong(UniformBufferObject* ubo, float3 direction, float distance)
{
    if (ubo->volumetricFogTextureIndex == 0xFFFFFFFF)
        return float4(0.0, 0.0, 0.0, 1.0);
    const float4 clip = mul(ubo->proj, mul(ubo->view, float4(ubo->cameraWorldPos.xyz + direction, 1.0)));
    const float2 uv = clip.w > 0.0 ? saturate(clip.xy / clip.w * float2(0.5, -0.5) + 0.5) : float2(0.5);
    return volumetricFogTerms(ubo, uv, distance);
}

float3 applyHeightFog(UniformBufferObject* ubo, float3 color, float3 direction, float distance)
{
    // The height fog beyond the volumetric fog's range, then the volumetric fog in front of it.
    const float4 fog = heightFogTerms(ubo, direction, distance);
    const float4 volumetric = volumetricFogTermsAlong(ubo, direction, distance);
    return (color * fog.a + fog.rgb) * volumetric.a + volumetric.rgb;
}

// Aerial perspective over a procedural sky (SkyAtmosphere.slang aerialPerspectiveMain): the air between the camera and a point at
// `distance` along the ray of screen position uv, looked up in the camera volume (textureIndex: its heap index, 0xFFFFFFFF: none).
// rgb the light it scatters towards the camera, a the transmittance of what is behind.
float4 aerialPerspectiveTerms(uint textureIndex, float kmPerUnit, float2 uv, float distance)
{
    if (textureIndex == 0xFFFFFFFF)
        return float4(0.0, 0.0, 0.0, 1.0);
    Texture2DArray volume = Texture2DArray.Handle(uint2(textureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));

    // Slice z ends at max * ((z + 1) / slices)^2: the inverse, -1 = at the camera.
    const float distanceKm = distance * kmPerUnit;
    const float slice = sqrt(saturate(distanceKm / AerialPerspectiveMaxDistanceKm)) * float(AerialPerspectiveSlices) - 1.0;
    if (slice < 0.0)
    {
        // Between the camera (no air yet: nothing scattered, all transmitted) and the end of the first slice.
        return lerp(float4(0.0, 0.0, 0.0, 1.0), volume.SampleLevel(linearClamp, float3(uv, 0.0), 0.0), slice + 1.0);
    }
    const float lower = floor(slice);
    const float upper = min(lower + 1.0, float(AerialPerspectiveSlices - 1));
    return lerp(volume.SampleLevel(linearClamp, float3(uv, lower), 0.0), volume.SampleLevel(linearClamp, float3(uv, upper), 0.0), slice - lower);
}

// The fog in front of a point the forward passes shade (glass, unlit surfaces, particles): the aerial perspective, then the height
// fog, as deferred lighting applies them to the opaque scene. rgb: the light they scatter towards the camera, a: the
// transmittance of the point's own light. A blended surface adds the scattered light in proportion to how much it covers what is
// behind it (that is already fogged): fogged = color * a + rgb * cover.
float4 sceneFogTerms(UniformBufferObject* ubo, float3 worldPos)
{
    const float3 toPoint = worldPos - ubo->cameraWorldPos.xyz;
    const float distance = length(toPoint);
    if (distance <= 1e-6)
        return float4(0.0, 0.0, 0.0, 1.0);

    float4 aerial = float4(0.0, 0.0, 0.0, 1.0);
    if (ubo->aerialPerspectiveTextureIndex != 0xFFFFFFFF)
    {
        const float4 clip = mul(ubo->proj, mul(ubo->view, float4(worldPos, 1.0)));
        const float2 uv = clip.w > 0.0 ? saturate(clip.xy / clip.w * float2(0.5, -0.5) + 0.5) : float2(0.5);
        aerial = aerialPerspectiveTerms(ubo->imageHeapIndexOffset + ubo->aerialPerspectiveTextureIndex, ubo->aerialPerspectiveKmPerUnit, uv, distance);
    }
    const float4 fog = heightFogTerms(ubo, toPoint / distance, distance);
    const float4 volumetric = volumetricFogTermsAlong(ubo, toPoint / distance, distance);
    const float4 combined = float4(aerial.rgb * fog.a + fog.rgb, aerial.a * fog.a);
    return float4(combined.rgb * volumetric.a + volumetric.rgb, combined.a * volumetric.a);
}
