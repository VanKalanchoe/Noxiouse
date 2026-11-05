#include "shaderIO.h"
#include "ShadowMaps.slang"

// Volumetric fog (RS6, Settings > Volumetric Fog): UE5's volumetric fog on its exponential height fog. A froxel grid over the view (a
// cell per 8 x 8 pixels, `slices` slices along the view ray out to maxDistance, slice z ending at maxDistance x ((z + 1) / slices)^2,
// as the aerial perspective's), each froxel:
//   lightMain     -- the height fog's density at its position (the Environment Light's Height Fog, times extinctionScale), lit by the
//                    sun (its cascaded shadow maps when they run, else a ray when RT shadows run; the clouds' shadow) and the point /
//                    spot lights (their atlas shadow maps when they have them), through a Henyey-Greenstein phase, plus the sky around
//                    (the irradiance map). Sampled at a depth that moves every frame inside the froxel, blended with last frame's
//                    (reprojected) -- the temporal filter smooths the slices into continuous fog and light shafts.
//   integrateMain -- front to back along each column: the light scattered towards the camera and the transmittance up to each
//                    slice (energy-conserving per slice, Hillaire 2015). HeightFog.slang looks it up per pixel.

static const float PI = 3.14159265358979;

float fog_slice_distance(float slice, float slices, float maxDistance) // the end of slice `slice` (fractional)
{
    const float t = saturate((slice + 1.0) / slices);
    return maxDistance * t * t;
}

float fog_phase_hg(float cosTheta, float g)
{
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// The sun's visibility by a ray (RT shadows without the shadow maps): opaque hits only (fog shafts need no alpha detail).
float fog_sun_ray(UniformBufferObject* ubo, float3 origin, float3 direction)
{
    if (ubo->enableRTShadows == 0 || ubo->tlasDeviceAddress == 0)
        return 1.0;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.01 * ubo->unitsPerMeter;
    ray.TMax = 100000.0 * ubo->unitsPerMeter;
    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(tlas, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

float3 fog_view_direction(float4x4 invViewProj, float3 cameraPos, float2 uv)
{
    const float4 h = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1e-4, 1.0)); // a point far along the pixel's ray (reverse Z)
    return normalize(h.xyz / h.w - cameraPos);
}

// Last frame's froxel lighting at a position, through the previous camera (-1: not in last frame's volume).
float4 fog_history(UniformBufferObject* ubo, PushConstantVolumetricFog pushConstant, float3 position, out bool valid)
{
    valid = false;
    const float4 clip = mul(ubo->prevProj, mul(ubo->prevView, float4(position, 1.0)));
    if (clip.w <= 0.0)
        return float4(0.0);
    const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    const float distance = length(position - pushConstant.previousCameraPos);
    const float slices = float(pushConstant.slices);
    const float slice = sqrt(saturate(distance / pushConstant.maxDistance)) * slices - 1.0;
    if (any(uv <= 0.0) || any(uv >= 1.0) || slice < 0.0 || slice > slices - 1.0)
        return float4(0.0);
    Texture2DArray history = Texture2DArray.Handle(uint2(pushConstant.historyTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    const float lower = floor(slice);
    const float upper = min(lower + 1.0, slices - 1.0);
    valid = true;
    return lerp(history.SampleLevel(linearClamp, float3(uv, lower), 0.0), history.SampleLevel(linearClamp, float3(uv, upper), 0.0), slice - lower);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void lightMain(uint3 id : SV_DispatchThreadID, uniform PushConstantVolumetricFog pushConstant)
{
    if (id.x >= pushConstant.gridWidth || id.y >= pushConstant.gridHeight || id.z >= pushConstant.slices)
        return;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    const float3 cameraPos = ubo->cameraWorldPos.xyz;
    const float2 uv = (float2(id.xy) + 0.5) / float2(pushConstant.gridWidth, pushConstant.gridHeight);
    const float3 direction = fog_view_direction(pushConstant.invViewProj, cameraPos, uv);
    const float slices = float(pushConstant.slices);
    // This frame's depth inside the froxel (between the ends of the slice before and this one).
    const float nearEnd = id.z == 0 ? 0.0 : fog_slice_distance(float(id.z) - 1.0, slices, pushConstant.maxDistance);
    const float farEnd = fog_slice_distance(float(id.z), slices, pushConstant.maxDistance);
    const float3 position = cameraPos + direction * lerp(nearEnd, farEnd, pushConstant.jitter);

    // The height fog's density there (per world unit).
    const float falloff = max(ubo->heightFogFalloff, 0.0);
    const float extinction = ubo->heightFogDensity * exp(-falloff * (position.y - ubo->heightFogBaseY)) * pushConstant.extinctionScale;
    float3 scattered = float3(0.0);
    if (extinction > 0.0)
    {
        const float noise = frac(52.9829189 * frac(0.06711056 * float(id.x) + 0.00583715 * float(id.y) + 0.1 * float(id.z)) + pushConstant.jitter);
        float3 light = float3(0.0);
        LightData* lights = (LightData*)ubo->lightDataReference;
        for (uint i = 0; i < ubo->lightCount; ++i)
        {
            const LightData lightData = lights[i];
            const uint type = (uint)lightData.position.w;
            float3 toLight;
            float3 radiance = lightData.color.rgb * lightData.color.w;
            float visibility = 1.0;
            if (type == 0)
            {
                toLight = normalize(lightData.direction.xyz);
                radiance *= cloud_shadow(ubo, position, i, toLight);
                if (i == ubo->shadowMaskLightIndex)
                {
                    if (ubo->sunShadowCascadesReference != 0)
                        visibility = sun_shadow_map(ubo, (GpuShadowCascades*)ubo->sunShadowCascadesReference, position, toLight, noise);
                    else
                        visibility = fog_sun_ray(ubo, position, toLight);
                }
            }
            else if ((type == 1 || type == 2) && pushConstant.localLights != 0)
            {
                const float3 lightVec = lightData.position.xyz - position;
                const float distance = length(lightVec);
                toLight = distance > 1e-4 ? lightVec / distance : float3(0.0, 1.0, 0.0);
                radiance *= light_distance_attenuation(lightData, distance) * light_spot_attenuation(lightData, toLight);
                if (any(radiance > 0.0))
                {
                    float mapVisibility;
                    if (local_light_shadow_map(ubo, i, lightData, position, toLight, toLight, noise, mapVisibility))
                        visibility = mapVisibility;
                }
            }
            else
            {
                continue;
            }
            light += radiance * visibility * fog_phase_hg(dot(direction, toLight), pushConstant.anisotropy);
        }
        // The sky around (isotropic: the phase function integrates to 1 over it), from above and along the view.
        if (ubo->irradianceMapIndex != 0xFFFFFFFF)
        {
            TextureCube irradiance = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
            SamplerState irradianceSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
            light += 0.5 * (irradiance.SampleLevel(irradianceSampler, float3(0.0, 1.0, 0.0), 0.0).rgb +
                            irradiance.SampleLevel(irradianceSampler, direction, 0.0).rgb) * ubo->scaleIBLAmbient;
        }
        scattered = light * extinction * pushConstant.albedo;
    }
    float4 result = float4(scattered, extinction);

    if (pushConstant.historyTextureIndex != 0xFFFFFFFF)
    {
        bool valid;
        const float4 history = fog_history(ubo, pushConstant, cameraPos + direction * lerp(nearEnd, farEnd, 0.5), valid);
        if (valid)
            result = lerp(result, history, pushConstant.historyWeight);
    }
    RWTexture2DArray<float4> output = RWTexture2DArray<float4>.Handle(uint2(pushConstant.lightingStorageIndex, 0));
    output[id] = result;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void integrateMain(uint3 id : SV_DispatchThreadID, uniform PushConstantVolumetricFog pushConstant)
{
    if (id.x >= pushConstant.gridWidth || id.y >= pushConstant.gridHeight)
        return;
    RWTexture2DArray<float4> lighting = RWTexture2DArray<float4>.Handle(uint2(pushConstant.lightingStorageIndex, 0));
    RWTexture2DArray<float4> integrated = RWTexture2DArray<float4>.Handle(uint2(pushConstant.integratedStorageIndex, 0));
    const float slices = float(pushConstant.slices);
    float3 accumulated = float3(0.0);
    float transmittance = 1.0;
    float previousEnd = 0.0;
    for (uint z = 0; z < pushConstant.slices; ++z)
    {
        const float end = fog_slice_distance(float(z), slices, pushConstant.maxDistance);
        const float thickness = end - previousEnd;
        previousEnd = end;
        const float4 froxel = lighting[uint3(id.xy, z)];
        const float extinction = max(froxel.a, 0.0);
        const float sliceTransmittance = exp(-extinction * thickness);
        // The light scattered inside the slice, attenuated within it (Hillaire 2015): S (1 - T) / extinction.
        const float3 sliceScattered = extinction > 1e-7 ? froxel.rgb * (1.0 - sliceTransmittance) / extinction : froxel.rgb * thickness;
        accumulated += transmittance * sliceScattered;
        transmittance *= sliceTransmittance;
        integrated[uint3(id.xy, z)] = float4(accumulated, transmittance);
    }
}
