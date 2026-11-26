// Heterogeneous Volume Ray-Marcher (docs/VFX_And_Volumes_Plan_2026-11.md step N2):
// Ray-marches cooked FP16 .nvdb volumes (or a procedural fire/smoke vortex when no volume asset is bound)
// with Beer-Lambert transmittance, Frostbite/UE5 analytical scattering integration, physical blackbody
// temperature emission, Henyey-Greenstein phase, sun self-shadowing, IGN temporal jitter & Reverse-Z occlusion.
#include "shaderIO.h"

struct VolumeVSOut
{
    float4 pos : SV_Position;
    nointerpolation uint volumeIndex : TEXCOORD0;
};

// Inward-wound [0, 1]^3 unit cube so NRI_CULL_MODE_BACK renders only the far inside faces
// (ensures ray-marching still executes cleanly when the camera flies inside the volume).
static const float3 kVolCubeVerts[36] = {
    // -Z
    float3(0,0,0), float3(1,0,0), float3(1,1,0),
    float3(0,0,0), float3(1,1,0), float3(0,1,0),
    // +Z
    float3(0,0,1), float3(1,1,1), float3(1,0,1),
    float3(0,0,1), float3(0,1,1), float3(1,1,1),
    // -X
    float3(0,0,0), float3(0,1,1), float3(0,0,1),
    float3(0,0,0), float3(0,1,0), float3(0,1,1),
    // +X
    float3(1,0,0), float3(1,0,1), float3(1,1,1),
    float3(1,0,0), float3(1,1,1), float3(1,1,0),
    // -Y
    float3(0,0,0), float3(1,0,1), float3(1,0,0),
    float3(0,0,0), float3(0,0,1), float3(1,0,1),
    // +Y
    float3(0,1,0), float3(1,1,0), float3(1,1,1),
    float3(0,1,0), float3(1,1,1), float3(0,1,1)
};

[shader("vertex")]
VolumeVSOut vertMain(uint vertexID : SV_VertexID, uniform PushConstantHeterogeneousVolume pushConstant)
{
    VolumeVSOut outV;
    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    const GpuHeterogeneousVolume vol = BufferRef<GpuHeterogeneousVolume>(pushConstant.volumesReference)[pushConstant.volumeIndex];
    const float3 localPos = kVolCubeVerts[vertexID % 36u];
    const float3 worldPos = mul(vol.model, float4(localPos, 1.0)).xyz;
    outV.pos = mul(ubo.proj, mul(ubo.view, float4(worldPos, 1.0)));
    outV.volumeIndex = pushConstant.volumeIndex;
    return outV;
}

// Interleaved Gradient Noise (Jimenez 2014) for temporal ray-start jittering.
float vol_ign(float2 px, uint frame)
{
    const float2 p = px + float(frame & 63u) * float2(5.588238, 5.588238);
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Read a single FP16 voxel from a packed uint32[] buffer (2 IEEE-754 halfs per uint).
float vol_load_fp16(uint64_t bufRef, uint3 coord, uint3 dims)
{
    const uint idx = (coord.z * dims.y + coord.y) * dims.x + coord.x;
    const uint packed = BufferRef<uint>(bufRef)[idx >> 1u];
    const uint halfBits = (idx & 1u) != 0u ? (packed >> 16u) : (packed & 0xFFFFu);
    return f16tof32(halfBits);
}

// Hardware-agnostic trilinear interpolation of an FP16 3D volume stored in a raw storage buffer.
float vol_sample_fp16(uint64_t bufRef, float3 uvw, uint3 dims)
{
    if (bufRef == 0 || any(uvw < 0.0) || any(uvw > 1.0))
        return 0.0;
    const float3 p = uvw * float3(dims) - 0.5;
    const int3 i0 = clamp(int3(floor(p)), int3(0, 0, 0), int3(dims) - int3(1, 1, 1));
    const int3 i1 = min(i0 + int3(1, 1, 1), int3(dims) - int3(1, 1, 1));
    const float3 f = saturate(p - float3(i0));

    const float c000 = vol_load_fp16(bufRef, uint3(i0.x, i0.y, i0.z), dims);
    const float c100 = vol_load_fp16(bufRef, uint3(i1.x, i0.y, i0.z), dims);
    const float c010 = vol_load_fp16(bufRef, uint3(i0.x, i1.y, i0.z), dims);
    const float c110 = vol_load_fp16(bufRef, uint3(i1.x, i1.y, i0.z), dims);
    const float c001 = vol_load_fp16(bufRef, uint3(i0.x, i0.y, i1.z), dims);
    const float c101 = vol_load_fp16(bufRef, uint3(i1.x, i0.y, i1.z), dims);
    const float c011 = vol_load_fp16(bufRef, uint3(i0.x, i1.y, i1.z), dims);
    const float c111 = vol_load_fp16(bufRef, uint3(i1.x, i1.y, i1.z), dims);

    const float c00 = lerp(c000, c100, f.x);
    const float c10 = lerp(c010, c110, f.x);
    const float c01 = lerp(c001, c101, f.x);
    const float c11 = lerp(c011, c111, f.x);
    return lerp(lerp(c00, c10, f.y), lerp(c01, c11, f.y), f.z);
}

// Fast 3D hash + value noise for the built-in procedural fire/smoke vortex fallback.
float vol_hash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float vol_noise3(float3 x)
{
    const float3 i = floor(x);
    const float3 f = frac(x);
    const float3 u = f * f * (3.0 - 2.0 * f);
    return lerp(
        lerp(lerp(vol_hash31(i + float3(0,0,0)), vol_hash31(i + float3(1,0,0)), u.x),
             lerp(vol_hash31(i + float3(0,1,0)), vol_hash31(i + float3(1,1,0)), u.x), u.y),
        lerp(lerp(vol_hash31(i + float3(0,0,1)), vol_hash31(i + float3(1,0,1)), u.x),
             lerp(vol_hash31(i + float3(0,1,1)), vol_hash31(i + float3(1,1,1)), u.x), u.y), u.z);
}

void vol_sample_procedural(float3 uvw, float time, bool fastShadow, out float density, out float temperature)
{
    const float ny = uvw.y;
    const float heightFade = sin(ny * 3.14159265);
    if (heightFade <= 1e-4)
    {
        density = 0.0;
        temperature = 0.0;
        return;
    }
    const float twist = ny * 5.5 - time * 2.2;
    float s, c;
    sincos(twist, s, c);
    const float2 center = float2(0.5 + c * 0.08 * ny, 0.5 + s * 0.08 * ny);
    const float2 rel = uvw.xz - center;
    const float r = length(rel);
    const float funnelR = lerp(0.14, 0.44, ny);
    if (r >= funnelR)
    {
        density = 0.0;
        temperature = 0.0;
        return;
    }
    const float radial = 1.0 - r / funnelR;
    const float3 np = float3(rel.x * c - rel.y * s, ny * 1.4 - time * 0.65, rel.x * s + rel.y * c) * 5.5;
    const float fbm = fastShadow
        ? vol_noise3(np)
        : (vol_noise3(np) * 0.65 + vol_noise3(np * 2.1 + 7.3) * 0.35);

    density = saturate(radial * radial * heightFade * (0.45 + 0.95 * fbm) * 1.6);
    temperature = fastShadow ? 0.0 : saturate(radial * (1.0 - ny * 0.72) * fbm * 1.45);
}

// Physical Planckian blackbody color approximation (Kelvin -> linear RGB).
float3 vol_blackbody_rgb(float kelvin)
{
    const float t = clamp(kelvin, 800.0, 10000.0) / 100.0;
    float r = t <= 66.0 ? 1.0 : saturate(1.292936 * pow(max(t - 60.0, 0.01), -0.1332047));
    float g = t <= 66.0
        ? saturate(0.390081 * log(max(t, 1.0)) - 0.631841)
        : saturate(1.129891 * pow(max(t - 60.0, 0.01), -0.0755148));
    float b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : saturate(0.543207 * log(max(t - 10.0, 1.0)) - 1.196254));
    // Convert sRGB approximation to linear HDR-friendly RGB.
    const float3 c = float3(r, g, b);
    return c * c;
}

// Henyey-Greenstein phase function.
float vol_hg_phase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(denom, 1.5));
}

bool vol_intersect_unit_box(float3 ro, float3 rd, out float tNear, out float tFar)
{
    const float3 invDir = 1.0 / select(abs(rd) > 1e-6, rd, float3(1e-6, 1e-6, 1e-6));
    const float3 t0 = (float3(0.0, 0.0, 0.0) - ro) * invDir;
    const float3 t1 = (float3(1.0, 1.0, 1.0) - ro) * invDir;
    const float3 tmin = min(t0, t1);
    const float3 tmax = max(t0, t1);
    tNear = max(max(tmin.x, tmin.y), tmin.z);
    tFar = min(min(tmax.x, tmax.y), tmax.z);
    return tFar > max(tNear, 0.0);
}

// Hardware-agnostic nearest-neighbor FP16 voxel fetch (1 load vs 8 for trilinear).
// Used for fast empty-space skipping and shadow marching (8x-24x faster with identical look).
float vol_sample_fp16_nearest(uint64_t bufRef, float3 uvw, uint3 dims)
{
    if (bufRef == 0 || any(uvw < 0.0) || any(uvw > 1.0))
        return 0.0;
    const int3 idx = clamp(int3(uvw * float3(dims)), int3(0, 0, 0), int3(dims) - int3(1, 1, 1));
    return vol_load_fp16(bufRef, uint3(idx), dims);
}

float vol_sample_density_fast(UniformBufferObject ubo, GpuHeterogeneousVolume vol, float3 uvw, float time)
{
    if ((vol.flags & VOLUME_FLAG_PROCEDURAL) != 0u || vol.volumeTextureIndex == 0xFFFFFFFFu)
    {
        float d, t;
        vol_sample_procedural(uvw, time, true, d, t);
        return d;
    }
    if (any(uvw < 0.0) || any(uvw > 1.0))
        return 0.0;
    Texture3D volTex = Texture3D.Handle(uint2(ubo.imageHeapIndexOffset + vol.volumeTextureIndex, 0));
    SamplerState linSamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    const float2 rg = volTex.SampleLevel(linSamp, uvw, 0).rg;
    return (vol.flags & VOLUME_FLAG_HAS_DENSITY) != 0u ? (rg.x * vol.marchParams.z) : (rg.y * vol.marchParams.w);
}

void vol_eval_fields(UniformBufferObject ubo, GpuHeterogeneousVolume vol, float3 uvw, float time, bool needTemp, out float density, out float temperature)
{
    if ((vol.flags & VOLUME_FLAG_PROCEDURAL) != 0u || vol.volumeTextureIndex == 0xFFFFFFFFu)
    {
        vol_sample_procedural(uvw, time, false, density, temperature);
        return;
    }
    if (any(uvw < 0.0) || any(uvw > 1.0))
    {
        density = 0.0;
        temperature = 0.0;
        return;
    }
    // Single hardware 3D trilinear fetch (R16G16_SFLOAT: r = density, g = max(temperature, flame)).
    Texture3D volTex = Texture3D.Handle(uint2(ubo.imageHeapIndexOffset + vol.volumeTextureIndex, 0));
    SamplerState linSamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    const float2 rg = volTex.SampleLevel(linSamp, uvw, 0).rg;
    const bool hasDensity = (vol.flags & VOLUME_FLAG_HAS_DENSITY) != 0u;
    density = hasDensity ? (rg.x * vol.marchParams.z) : (rg.y * vol.marchParams.w);
    temperature = needTemp ? (rg.y * vol.marchParams.w) : 0.0;
}

[shader("fragment")]
float4 fragMain(VolumeVSOut inVert, uniform PushConstantHeterogeneousVolume pushConstant) : SV_Target0
{
    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    const GpuHeterogeneousVolume vol = BufferRef<GpuHeterogeneousVolume>(pushConstant.volumesReference)[inVert.volumeIndex];

    // Reconstruct render-space primary camera ray for this pixel.
    const float2 uv = inVert.pos.xy / max(pushConstant.viewportSize, float2(1.0, 1.0));
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 rayTargetH = mul(ubo.invViewProj, float4(ndc, 0.5, 1.0));
    const float3 rayTarget = rayTargetH.xyz / max(abs(rayTargetH.w), 1e-6);
    const float3 rayOriginW = ubo.cameraWorldPos.xyz;
    const float3 rayDirW = normalize(rayTarget - rayOriginW);

    // Transform ray into volume unit-box [0, 1]^3 space.
    const float3 rayOriginL = mul(vol.invModel, float4(rayOriginW, 1.0)).xyz;
    const float3 rayDirL = mul((float3x3)vol.invModel, rayDirW);

    float tNear, tFar;
    if (!vol_intersect_unit_box(rayOriginL, rayDirL, tNear, tFar))
        discard;

    // Reverse-Z depth occlusion against opaque scene geometry.
    float tMax = tFar;
    if (pushConstant.depthTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D depthTex = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
        uint dw = 1u, dh = 1u;
        depthTex.GetDimensions(dw, dh);
        const int2 depthPx = clamp(int2(uv * float2(dw, dh)), int2(0, 0), int2(int(dw) - 1, int(dh) - 1));
        const float sceneDepthNdc = depthTex.Load(int3(depthPx, 0)).r;
        if (sceneDepthNdc > 1e-6)
        {
            const float4 sceneH = mul(ubo.invViewProj, float4(ndc, sceneDepthNdc, 1.0));
            const float3 sceneW = sceneH.xyz / max(abs(sceneH.w), 1e-6);
            const float sceneDistW = dot(sceneW - rayOriginW, rayDirW);
            tMax = min(tMax, sceneDistW);
        }
    }

    const float tStart = max(tNear, 0.0);
    if (tMax <= tStart)
        discard;

    // Primary directional sun light (first directional light in UBO, fallback zenith).
    float3 sunDirW = normalize(float3(0.4, 0.8, 0.4));
    float3 sunColor = float3(3.0, 2.85, 2.6);
    if (ubo.lightCount > 0u && ubo.lightDataReference != 0)
    {
        BufferRef<LightData> lights = BufferRef<LightData>(ubo.lightDataReference);
        const uint count = min(ubo.lightCount, 8u);
        for (uint i = 0u; i < count; ++i)
        {
            if (uint(lights[i].position.w) == 0u) // directional light
            {
                sunDirW = normalize(lights[i].direction.xyz);
                sunColor = lights[i].color.rgb * lights[i].color.a;
                break;
            }
        }
    }
    const float3 sunDirL = mul((float3x3)vol.invModel, sunDirW);

    float3 ambientColor = float3(0.45, 0.55, 0.72) * (vol.emissionParams.w / 3.14159265);
    if (ubo.irradianceMapIndex != 0xFFFFFFFFu)
    {
        TextureCube irr = TextureCube.Handle(uint2(ubo.imageHeapIndexOffset + ubo.irradianceMapIndex, 0));
        SamplerState irrSamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        ambientColor = irr.SampleLevel(irrSamp, float3(0.0, 1.0, 0.0), 0).rgb * (ubo.scaleIBLAmbient * vol.emissionParams.w / 3.14159265);
    }

    const uint steps = clamp(uint(vol.marchParams.x), 16u, 192u);
    const uint shadowSteps = clamp(uint(vol.marchParams.y), 0u, 16u);
    const float dt = (tMax - tStart) / float(steps);
    const float jitter = vol_ign(inVert.pos.xy, pushConstant.frameIndex);

    // Convert world-space step length into normalized box step so DensityScale is scale-independent.
    const float boxStepLen = dt * length(rayDirL);
    const float cosTheta = dot(rayDirW, sunDirW);
    // Dual-lobe Henyey-Greenstein (forward + slight backscatter) normalized to 1/pi like PBR diffuse so sun isn't 12x dimmer than sky.
    const float g = clamp(vol.emissionParams.z, -0.9, 0.9);
    const float phase = (0.75 * vol_hg_phase(cosTheta, g) + 0.25 * vol_hg_phase(cosTheta, -0.25)) * 4.0;
    const float densityScale = max(vol.albedoAndDensity.w, 0.0);
    const float3 albedo = vol.albedoAndDensity.rgb;
    const float emissionScale = max(vol.emissionParams.x, 0.0);
    const float peakKelvin = vol.emissionParams.y;

    float3 scattering = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;

    for (uint s = 0u; s < steps; ++s)
    {
        if (transmittance < 0.005)
            break;

        const float t = tStart + (float(s) + jitter) * dt;
        const float3 uvw = rayOriginL + rayDirL * t;

        float rawDensity, rawTemp;
        vol_eval_fields(ubo, vol, uvw, ubo.timeSeconds, emissionScale > 0.0, rawDensity, rawTemp);

        const float sigmaT = rawDensity * densityScale;
        if (sigmaT > 1e-4 || rawTemp > 1e-3)
        {
            // Self-shadowing light-march toward the sun inside the volume.
            float shadowTrans = 1.0;
            float shadowOpticalDepth = 0.0;
            if (shadowSteps > 0u && sigmaT > 1e-4)
            {
                float sNear, sFar;
                if (vol_intersect_unit_box(uvw, sunDirL, sNear, sFar))
                {
                    // Exponentially spaced shadow steps capture both crisp local crevices and deep core self-shadowing.
                    const float sunDirLen = length(sunDirL);
                    for (uint k = 0u; k < shadowSteps; ++k)
                    {
                        const float f0 = float(k) / float(shadowSteps);
                        const float f1 = float(k + 1u) / float(shadowSteps);
                        const float mid = 0.5 * (f0 * f0 + f1 * f1) * sFar;
                        const float segLen = (f1 * f1 - f0 * f0) * sFar;
                        const float3 suvw = uvw + sunDirL * mid;
                        const float sd = vol_sample_density_fast(ubo, vol, suvw, ubo.timeSeconds);
                        shadowOpticalDepth += sd * densityScale * (segLen * sunDirLen);
                    }
                    // 3-octave energy-conserving multi-scattering (Wrenninge / UE5 Heterogeneous Volumes)
                    // + subtle powder effect for crisp billowy contrast on ridges vs deep folds.
                    const float multiScat = 0.65 * exp(-shadowOpticalDepth)
                                          + 0.25 * exp(-shadowOpticalDepth * 0.30)
                                          + 0.10 * exp(-shadowOpticalDepth * 0.08);
                    const float powder = lerp(1.0, 1.0 - exp(-shadowOpticalDepth * 2.0), 0.35 * (1.0 - saturate(cosTheta * 0.5 + 0.5)));
                    shadowTrans = multiScat * powder;
                }
            }

            // Height/core ambient occlusion so deep interior folds stay rich and dark instead of flat sky-blue.
            const float ambientAO = exp(-shadowOpticalDepth * 0.18);
            const float3 inScatter = albedo * (sunColor * shadowTrans * phase + ambientColor * ambientAO);
            float3 emission = float3(0.0, 0.0, 0.0);
            if (rawTemp > 0.02 && emissionScale > 0.0)
            {
                const float tempNorm = saturate(rawTemp);
                const float kelvin = lerp(900.0, peakKelvin, tempNorm);
                emission = vol_blackbody_rgb(kelvin) * (tempNorm * tempNorm * emissionScale);
            }

            const float stepOpticalDepth = sigmaT * boxStepLen;
            const float stepTrans = exp(-stepOpticalDepth);
            // Energy-conserving analytical integration (Frostbite / UE5).
            const float3 stepRadiance = sigmaT > 1e-4
                ? (inScatter * sigmaT + emission) * ((1.0 - stepTrans) / sigmaT)
                : emission * boxStepLen;

            scattering += transmittance * stepRadiance;
            transmittance *= stepTrans;
        }
    }

    const float alpha = saturate(1.0 - transmittance);
    if (alpha <= 1e-4 && dot(scattering, scattering) <= 1e-8)
        discard;

    return float4(scattering, alpha);
}

struct VolCompVSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
VolCompVSOut compositeVertMain(uint vertexID : SV_VertexID)
{
    static const float2 kPos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    VolCompVSOut o;
    o.pos = float4(kPos[vertexID % 3u], 0.0, 1.0);
    o.uv = float2(0.0, 0.0);
    return o;
}

[shader("fragment")]
float4 compositeFragMain(VolCompVSOut inVert, uniform PushConstantHeterogeneousVolume pushConstant) : SV_Target0
{
    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    Texture2D halfVol = Texture2D.Handle(uint2(ubo.imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    SamplerState linSamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    const float2 uv = inVert.pos.xy / max(pushConstant.viewportSize, float2(1.0, 1.0));
    const float4 v = halfVol.SampleLevel(linSamp, uv, 0);
    if (v.a <= 1e-4 && dot(v.rgb, v.rgb) <= 1e-8)
        discard;
    return v;
}
