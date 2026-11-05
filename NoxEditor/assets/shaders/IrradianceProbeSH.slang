#include "shaderIO.h"
#include "DDGICommon.slang"
#include "ShadowMaps.slang"
#include "IrradianceVolumeCommon.slang"

// The irradiance volume's probes (RS5a; Renderer::addIrradianceVolumePasses, GpuIrradianceVolume in shaderIO.h).
//   packMain    -- a probe's six captured faces (one row of the atlases: +X, -X, +Y, -Y, +Z, -Z, IrradianceProbeFaceSize texels each,
//                  unlit: Material_TransparentLit_Mesh.slang captureFragMain) into its surface cache (cacheFaceSize texels a face
//                  side: each the average of the block of captured texels it covers -- diffuse albedo, emission, normal, distance
//                  and how much of it is sky) and into the L1 SH of the distance it sees and of its square (the lookup's
//                  visibility). Only when the probe is captured: when the volume changes or the scene near it does.
//   relightMain -- a probe lit again from its surface cache, every frame a share of the volume: each cache texel's surface lit
//                  by the sun (through the volume's own sun shadow map, which covers the whole box), the point / spot / rect /
//                  disc lights (their atlas shadow maps where they have one this frame), its emission and the light the volume
//                  itself gives there (last frame's probes: every relight adds a bounce, DDGI's way); the sky where it sees the
//                  sky. Projected onto L1 SH, every texel weighted by the solid angle it covers. Enlighten's / Frostbite's
//                  relighting of precomputed visibility: the light changes (time of day, moving lights) without drawing the scene.

static const float PI = 3.14159265358979;

// A face's view (Renderer: a flipped right-handed look-at along its axis, 90 degrees): world direction of a texel's ndc.
static const float3 kFaceDirections[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
static const float3 kFaceUps[6] = { float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, 1), float3(0, 0, -1), float3(0, 1, 0), float3(0, 1, 0) };

float3 face_direction(uint face, float2 ndc)
{
    const float3 forward = kFaceDirections[face];
    const float3 side = normalize(cross(forward, kFaceUps[face]));
    const float3 up = cross(side, forward);
    return normalize(ndc.x * side + ndc.y * up + forward);
}

// Texel (x, y) of a face `size` texels a side: its ndc (y down the rows).
float2 face_ndc(uint x, uint y, float size)
{
    return float2((float(x) + 0.5) / size * 2.0 - 1.0, 1.0 - (float(y) + 0.5) / size * 2.0);
}

uint pack_unorm4(float4 v)
{
    const uint4 q = uint4(round(saturate(v) * 255.0));
    return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24);
}

float4 unpack_unorm4(uint p)
{
    return float4(float(p & 0xFFu), float((p >> 8) & 0xFFu), float((p >> 16) & 0xFFu), float(p >> 24)) / 255.0;
}

uint pack_half2(float a, float b)
{
    return f32tof16(min(a, 65000.0)) | (f32tof16(min(b, 65000.0)) << 16);
}

float2 unpack_half2(uint p)
{
    return float2(f16tof32(p & 0xFFFFu), f16tof32(p >> 16));
}

uint pack_snorm2(float2 v)
{
    const int2 q = int2(round(clamp(v, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xFFFFu) | (uint(q.y) << 16);
}

float2 unpack_snorm2(uint p)
{
    const int x = int(p << 16) >> 16;
    const int y = int(p) >> 16;
    return max(float2(float(x), float(y)) / 32767.0, -1.0);
}

float3 probe_center(GpuIrradianceVolume* volume, uint probe)
{
    const uint3 counts = uint3(volume->countX, volume->countY, volume->countZ);
    const uint3 coord = uint3(probe % counts.x, (probe / counts.x) % counts.y, probe / (counts.x * counts.y));
    const float3 extents = volume->extents.xyz;
    const float3 cell = 2.0 * extents / float3(max(counts, uint3(2)) - 1u);
    return mul(volume->localToWorld, float4(float3(coord) * cell - extents, 1.0)).xyz;
}

// --- packMain ----------------------------------------------------------------------------------------------------------------------

groupshared float4 gDistanceSH[256][2];

[shader("compute")]
[numthreads(16, 16, 1)]
void packMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uint threadIndex : SV_GroupIndex,
              uniform PushConstantIrradianceProbePack pushConstant)
{
    const uint row = groupId.x;
    const uint probe = ((uint*)pushConstant.probeIndicesReference)[row];
    GpuIrradianceVolume* volume = (GpuIrradianceVolume*)pushConstant.volumeReference;
    Texture2D albedoAtlas = Texture2D.Handle(uint2(pushConstant.albedoTextureIndex, 0));
    Texture2D emissionAtlas = Texture2D.Handle(uint2(pushConstant.emissionTextureIndex, 0));
    Texture2D<float> depthAtlas = Texture2D<float>.Handle(uint2(pushConstant.depthTextureIndex, 0));
    const float faceSize = float(IrradianceProbeFaceSize);
    const uint cacheSize = max(volume->cacheFaceSize, 1u);
    const uint block = max(IrradianceProbeFaceSize / cacheSize, 1u);
    uint4* cache = (uint4*)volume->surfaceCacheReference + probe * 6u * cacheSize * cacheSize;

    float4 shD = float4(0.0), shD2 = float4(0.0);
    for (uint face = 0; face < 6; ++face)
    {
        const float2 ndc = face_ndc(threadId.x, threadId.y, faceSize);
        const float3 direction = face_direction(face, ndc);
        // The texel's solid angle: (2 / size)^2 / (1 + x^2 + y^2)^(3/2).
        const float solidAngle = (4.0 / (faceSize * faceSize)) / pow(1.0 + dot(ndc, ndc), 1.5);
        const int2 origin = int2(int(face * IrradianceProbeFaceSize), int(row * IrradianceProbeFaceSize));
        // How far the probe sees this way: view depth along the face's axis, out along the texel's ray (the sky: as far as counts).
        const float depth = depthAtlas.Load(int3(origin + int2(threadId.xy), 0));
        const float distance = depth > 0.0 ? min(pushConstant.nearPlane / depth * sqrt(1.0 + dot(ndc, ndc)), pushConstant.maxDistance) : pushConstant.maxDistance;
        const float4 basis = float4(0.282095, 0.488603 * direction.y, 0.488603 * direction.z, 0.488603 * direction.x) * solidAngle;
        shD += basis * distance;
        shD2 += basis * (distance * distance);

        // The surface cache texel this thread's block makes (its first thread averages the block).
        if (threadId.x % block == 0 && threadId.y % block == 0)
        {
            float3 albedo = float3(0.0), emission = float3(0.0), normal = float3(0.0);
            float distanceSum = 0.0;
            float surfaces = 0.0;
            for (uint dy = 0; dy < block; ++dy)
            {
                for (uint dx = 0; dx < block; ++dx)
                {
                    const uint2 texel = threadId.xy + uint2(dx, dy);
                    const int3 at = int3(origin + int2(texel), 0);
                    const float texelDepth = depthAtlas.Load(at);
                    if (texelDepth <= 0.0) // the sky
                        continue;
                    const float4 a = albedoAtlas.Load(at);
                    const float4 e = emissionAtlas.Load(at);
                    const float2 texelNdc = face_ndc(texel.x, texel.y, faceSize);
                    albedo += a.rgb;
                    emission += e.rgb;
                    normal += oct_decode(float2(a.a, e.a));
                    distanceSum += pushConstant.nearPlane / texelDepth * sqrt(1.0 + dot(texelNdc, texelNdc));
                    surfaces += 1.0;
                }
            }
            const float sky = 1.0 - surfaces / float(block * block);
            if (surfaces > 0.0)
            {
                albedo /= surfaces;
                emission /= surfaces;
                distanceSum /= surfaces;
                normal = length(normal) > 1e-4 ? normalize(normal) : -direction;
            }
            else
            {
                normal = -direction;
            }
            const uint cacheTexel = (face * cacheSize + threadId.y / block) * cacheSize + threadId.x / block;
            cache[cacheTexel] = uint4(pack_unorm4(float4(albedo, sky)), pack_half2(emission.r, emission.g), pack_half2(emission.b, distanceSum),
                                      pack_snorm2(oct_encode(normal)));
        }
    }
    gDistanceSH[threadIndex][0] = shD;
    gDistanceSH[threadIndex][1] = shD2;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (threadIndex < stride)
        {
            gDistanceSH[threadIndex][0] += gDistanceSH[threadIndex + stride][0];
            gDistanceSH[threadIndex][1] += gDistanceSH[threadIndex + stride][1];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (threadIndex == 0)
    {
        float4* sh = (float4*)volume->probeSHReference;
        sh[probe * 6 + 4] = gDistanceSH[0][0];
        sh[probe * 6 + 5] = gDistanceSH[0][1];
    }
}

// --- relightMain -------------------------------------------------------------------------------------------------------------------

// The sun at a cache texel's surface through the volume's sun shadow map (one bilinear comparison: the probes' light is soft
// anyway). Normal offset and a step towards the sun of a texel or so against the averaged surface's self shadowing.
float relight_sun_visibility(UniformBufferObject* ubo, GpuIrradianceVolume* volume, float3 position, float3 n, float3 L)
{
    if (volume->sunShadowTextureIndex == 0xFFFFFFFF)
        return 1.0;
    const float texel = volume->sunTexelWorld;
    const float NoL = saturate(dot(n, L));
    const float sinTheta = sqrt(saturate(1.0 - NoL * NoL));
    const float3 receiver = position + n * (2.0 * texel * sinTheta) + L * texel;
    const float4 clip = mul(volume->sunViewProj, float4(receiver, 1.0));
    const float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    if (any(uv <= 0.0) || any(uv >= 1.0) || clip.z <= 0.0 || clip.z > 1.0)
        return 1.0;
    Texture2D<float> map = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + volume->sunShadowTextureIndex, 0));
    SamplerState pointClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_NEAREST_CLAMP, 0));
    const float slope = min(sinTheta / max(NoL, 0.05), 4.0);
    const float bias = (1.0 + slope) * texel / max(volume->sunDepthRange, 1e-6);
    return shadow_map_bilinear(map, pointClamp, uv, clip.z + bias, float(volume->sunShadowResolution));
}

// The direct light's irradiance at a surface (E, not yet / pi), every light of the frame.
float3 relight_direct(UniformBufferObject* ubo, GpuIrradianceVolume* volume, float3 position, float3 n, float noise)
{
    float3 irradiance = float3(0.0);
    LightData* lights = (LightData*)ubo->lightDataReference;
    for (uint i = 0; i < ubo->lightCount; ++i)
    {
        const LightData light = lights[i];
        const uint type = (uint)light.position.w;
        const float3 radiance = light.color.rgb * light.color.w;
        if (type == 0)
        {
            const float3 L = normalize(light.direction.xyz);
            const float NoL = dot(n, L);
            if (NoL <= 0.0)
                continue;
            float visibility = cloud_shadow(ubo, position, i, L);
            if (i == ubo->shadowMaskLightIndex && visibility > 0.0)
                visibility *= relight_sun_visibility(ubo, volume, position, n, L);
            irradiance += radiance * (NoL * visibility);
            continue;
        }
        const float3 lightVec = light.position.xyz - position;
        const float distance = length(lightVec);
        const float3 L = distance > 1e-4 ? lightVec / distance : n;
        float3 contribution;
        if (is_area_light(type))
        {
            contribution = radiance * rect_light_window(light, distance) * rect_light_form_factor(light, position, n);
        }
        else
        {
            const float NoL = dot(n, L);
            if (NoL <= 0.0)
                continue;
            contribution = radiance * (light_distance_attenuation(light, distance) * light_spot_attenuation(light, L) * NoL);
        }
        if (all(contribution <= 0.0))
            continue;
        float visibility;
        if (local_light_shadow_map(ubo, i, light, position, n, L, noise, visibility))
            contribution *= visibility;
        irradiance += contribution;
    }
    return irradiance;
}

groupshared float4 gRadianceSH[64][3];

[shader("compute")]
[numthreads(64, 1, 1)]
void relightMain(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex, uniform PushConstantIrradianceRelight pushConstant)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    GpuIrradianceVolume* volume = (GpuIrradianceVolume*)pushConstant.volumeReference;
    const uint probe = ((uint*)pushConstant.probeIndicesReference)[groupId.x];
    const uint cacheSize = max(volume->cacheFaceSize, 1u);
    const uint texelsPerFace = cacheSize * cacheSize;
    const uint texelCount = 6u * texelsPerFace;
    const float size = float(cacheSize);
    uint4* cache = (uint4*)volume->surfaceCacheReference + probe * texelCount;
    const float3 center = probe_center(volume, probe);

    // The sky around: the environment's prefiltered radiance at about a cache texel's width; its irradiance where the volume gives
    // no light (outside it, or probes not lit yet).
    const bool environment = ubo->prefilteredMapIndex != 0xFFFFFFFF && ubo->irradianceMapIndex != 0xFFFFFFFF;
    TextureCube prefiltered = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + (environment ? ubo->prefilteredMapIndex : 0u), 0));
    TextureCube irradianceMap = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + (environment ? ubo->irradianceMapIndex : 0u), 0));
    SamplerState prefilterSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
    SamplerState irradianceSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
    const float skyLod = 0.6 * float(ubo->prefilteredCubeMipLevels);

    float4 shR = float4(0.0), shG = float4(0.0), shB = float4(0.0);
    for (uint t = threadIndex; t < texelCount; t += 64u)
    {
        const uint face = t / texelsPerFace;
        const uint inFace = t % texelsPerFace;
        const float2 ndc = face_ndc(inFace % cacheSize, inFace / cacheSize, size);
        const float3 direction = face_direction(face, ndc);
        const float solidAngle = (4.0 / (size * size)) / pow(1.0 + dot(ndc, ndc), 1.5);

        const uint4 texel = cache[t];
        const float4 albedoSky = unpack_unorm4(texel.x);
        const float sky = albedoSky.w;
        float3 radiance = float3(0.0);
        if (sky > 0.0 && environment)
            radiance += sky * prefiltered.SampleLevel(prefilterSampler, direction, skyLod).rgb * ubo->scaleIBLAmbient;
        if (sky < 1.0)
        {
            const float2 emissionRG = unpack_half2(texel.y);
            const float2 emissionBDistance = unpack_half2(texel.z);
            const float3 emission = float3(emissionRG, emissionBDistance.x);
            const float3 n = oct_decode(unpack_snorm2(texel.w));
            const float3 position = center + direction * emissionBDistance.y;
            const float noise = frac(52.9829189 * frac(0.06711056 * float(t) + 0.00583715 * float(probe)) + 0.618034 * float(pushConstant.frameIndex & 63u));
            float3 lit = relight_direct(ubo, volume, position, n, noise) / PI;
            // Further bounces: the light the volume gives there (last frame's probes), the sky's irradiance where it gives none.
            float3 bounce = environment ? irradianceMap.SampleLevel(irradianceSampler, n, 0.0).rgb * ubo->scaleIBLAmbient : float3(0.0);
            float3 volumeLight;
            float weight;
            if (irradiance_volume_lookup(ubo, position, n, volumeLight, weight))
                bounce = lerp(bounce, volumeLight, weight);
            lit += bounce;
            radiance += (1.0 - sky) * (albedoSky.rgb * lit + emission);
        }
        const float4 basis = float4(0.282095, 0.488603 * direction.y, 0.488603 * direction.z, 0.488603 * direction.x) * solidAngle;
        shR += basis * radiance.r;
        shG += basis * radiance.g;
        shB += basis * radiance.b;
    }
    gRadianceSH[threadIndex][0] = shR;
    gRadianceSH[threadIndex][1] = shG;
    gRadianceSH[threadIndex][2] = shB;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (threadIndex < stride)
        {
            gRadianceSH[threadIndex][0] += gRadianceSH[threadIndex + stride][0];
            gRadianceSH[threadIndex][1] += gRadianceSH[threadIndex + stride][1];
            gRadianceSH[threadIndex][2] += gRadianceSH[threadIndex + stride][2];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (threadIndex == 0)
    {
        float4* sh = (float4*)volume->probeSHReference;
        sh[probe * 6 + 0] = gRadianceSH[0][0];
        sh[probe * 6 + 1] = gRadianceSH[0][1];
        sh[probe * 6 + 2] = gRadianceSH[0][2];
        sh[probe * 6 + 3] = float4(1.0, 0.0, 0.0, 0.0); // lit
    }
}
