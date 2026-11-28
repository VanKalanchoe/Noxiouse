// RTXDI's lights for every pass that resamples them (ReSTIR DI initial / temporal / spatial / final shading, the presampling of
// the light PDF, RIS tiles and ReGIR, and ReSTIR GI's path vertices): one definition of what a light index is, how a light is
// sampled and how much it weighs, so the passes resample the same thing (RTXDI's bridge functions, Doc/RtxdiApplicationBridge.md).
//
// The light index space (UniformBufferObject::rtxdi*): RTXDI wants the local lights as one contiguous range and the infinite lights
// as another (RTXDI_LightBufferParameters).
//   [0, A)              the analytic local lights: point, spot, rect, disc -- LightData[0, A)
//   [A, A + T)          the emissive triangles that emit: entry j is the emitter list's j-th (UniformBufferObject::
//                       emissiveEmitterListReference, EmissiveLightGrid.slang scanMain) -- one light per emitting triangle, as RTXDI's
//                       sample prepares mesh lights. T is the emissive triangles' count, an upper bound the CPU knows; entries past
//                       the list's count (the GPU's) are empty lights of no power, which the light PDF never picks.
//   [A + T, ...)        the infinite (directional) lights: LightData[A, ...)
// T is 0 unless ReSTIR DI gives the emissive meshes' direct light (rtxdiEmissiveLights; the hybrid renderer with Direct Lighting:
// ReSTIR DI), so with T = 0 an index is its LightData index as before.
//
// A light sample's radiance carries the light's whole contribution for its random numbers -- the falloff, and for area lights and
// triangles their area x cos / distance^2 (1 / the solid angle pdf of a uniform point on them) -- and solidAnglePdf is 1: the
// convention of the engine's analytic lights (shaderIO.h sample_light_point). The samples count as analytic (no BRDF-ray MIS:
// numBrdfSamples 0), so RTXDI's RIS weight is targetPdf / selection pdf (Rtxdi/DI/InitialSampling.hlsli RTXDI_LightBrdfMisWeight)
// and final shading's radiance x invPdf x N.L x BRDF the unbiased estimate.
#pragma once

#include "shaderIO.h"
#include "PathTracer/EmissiveLights.slang"

struct RAB_LightInfo
{
    LightData light;   // analytic lights (local and infinite); for a triangle, position.w = RtxdiTriangleLightType
    uint triangle;     // the EmissiveTriangle record, ~0 for analytic lights
};

struct RAB_LightSample
{
    float3 position;
    float3 normal;       // directional lights: the direction towards the light; local lights: towards the sampled point
    float3 radiance;     // the sample's contribution (see above)
    float solidAnglePdf; // 1 (see above)
    uint lightType;      // LightType, or RtxdiTriangleLightType
};

static BufferRef<LightData> g_LightData;
static UniformBufferObject g_RtxdiUbo;

// Every pass binds the frame's uniforms and light buffer before RTXDI calls back into the functions below.
void rtxdi_bind_lights(UniformBufferObject ubo, uint64_t lightDataReference)
{
    g_RtxdiUbo = ubo;
    g_LightData = BufferRef<LightData>(lightDataReference);
}

uint rtxdi_triangle_light_count()
{
    return min(g_RtxdiUbo.rtxdiEmissiveTriangleLights, g_RtxdiUbo.emissiveTriangleCount);
}

RAB_LightInfo RAB_EmptyLightInfo()
{
    RAB_LightInfo info;
    info.light.position = float4(0.0);
    info.light.direction = float4(0.0);
    info.light.color = float4(0.0);
    info.light.spotParams = float4(0.0);
    info.triangle = 0xFFFFFFFFu;
    return info;
}

RAB_LightSample RAB_EmptyLightSample()
{
    RAB_LightSample sample;
    sample.position = float3(0.0);
    sample.normal = float3(0.0, 1.0, 0.0);
    sample.radiance = float3(0.0);
    sample.solidAnglePdf = 0.0;
    sample.lightType = 0;
    return sample;
}

// The light of an RTXDI index (the ranges above). prevFrame: the same lights -- an index space that changed is caught by
// RAB_TranslateLightIndex, which then drops the history.
RAB_LightInfo RAB_LoadLightInfo(uint index, bool prevFrame)
{
    RAB_LightInfo info = RAB_EmptyLightInfo();
    const uint analytic = g_RtxdiUbo.rtxdiAnalyticLocalLights;
    const uint triangles = rtxdi_triangle_light_count();
    if (index < analytic)
    {
        info.light = g_LightData[index];
    }
    else if (index < analytic + triangles)
    {
        // The (index - analytic)-th emitting triangle; past the list's count an empty light (no power: never sampled).
        const BufferRef<uint> emitters = BufferRef<uint>(g_RtxdiUbo.emissiveEmitterListReference);
        const uint entry = index - analytic;
        if (g_RtxdiUbo.emissiveEmitterListReference != 0 && entry < emitters[0])
        {
            info.triangle = emitters[1 + entry];
            info.light.position.w = float(RtxdiTriangleLightType);
        }
        else
        {
            info.light.position.w = float(LightType::Point); // a local light of no power (color 0)
        }
    }
    else if (index - triangles < g_RtxdiUbo.lightCount)
    {
        info.light = g_LightData[index - triangles];
    }
    return info;
}

// Last frame's index of a light (currentToPrevious) or this frame's of last frame's: the same while the light list stays as it
// was; -1 -- "not there" (RTXDI: the reservoir's history is dropped) -- for every light on a frame it changed (a light or an
// emissive mesh came or went, Renderer::updateLightBuffer).
int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return g_RtxdiUbo.rtxdiLightsChanged != 0 ? -1 : int(lightIndex);
}

bool rtxdi_is_triangle_light(RAB_LightInfo light)
{
    return light.triangle != 0xFFFFFFFFu;
}

bool rtxdi_is_directional_light(RAB_LightInfo light)
{
    return !rtxdi_is_triangle_light(light) && (uint)light.light.position.w == uint(LightType::Directional);
}

// A local light worth sampling (not an empty slot, not a directional light).
bool rtxdi_is_local_light(RAB_LightInfo light)
{
    return rtxdi_is_triangle_light(light) || (uint)light.light.position.w != uint(LightType::Directional);
}

// A point on the light seen from worldPos and its contribution (see the header). uv: the two uniform numbers RTXDI keeps in its
// reservoirs.
RAB_LightSample rtxdi_sample_light(RAB_LightInfo light, float3 worldPos, float2 uv)
{
    RAB_LightSample s = RAB_EmptyLightSample();
    if (rtxdi_is_triangle_light(light))
    {
        const EmissiveTriangle record = BufferRef<EmissiveTriangle>(g_RtxdiUbo.emissiveTrianglesReference)[light.triangle];
        s.lightType = RtxdiTriangleLightType;
        s.solidAnglePdf = 1.0;
        if (!(record.power > 0.0))
            return s;
        // Uniform on the triangle (areaProbability 1: the pdf is the point's alone), its emission with the emissive texture at its
        // finest mip -- the path tracer's triangle sampling, so both see the same lamp.
        const EmissiveLights::Sample point = EmissiveLights::sampleTrianglePoint(g_RtxdiUbo, worldPos, record.instanceSlot, record.primitive,
                                                                                 uv, 1.0, 0.0);
        if (!point.valid)
            return s;
        s.position = worldPos + point.direction * point.distance;
        s.normal = point.direction;
        s.radiance = point.radiance / point.pdf;
        return s;
    }
    // The engine's analytic lights (shaderIO.h, shared with the rest of the renderer): a point on the light -- the sun's cone, a
    // sphere light's disc, a rect light's area -- and its light with the falloff, cone and a rect's area / distance^2 folded in.
    const LightPointSample lightPoint = sample_light_point(light.light, worldPos, uv);
    s.lightType = (uint)light.light.position.w;
    s.position = lightPoint.position;
    s.normal = lightPoint.direction;
    s.radiance = lightPoint.radiance;
    s.solidAnglePdf = 1.0;
    return s;
}

void rtxdi_light_direction_distance(float3 worldPos, RAB_LightSample sample, out float3 direction, out float distance)
{
    if (sample.lightType == uint(LightType::Directional))
    {
        direction = sample.normal;
        distance = 1e4;
    }
    else
    {
        const float3 lightVector = sample.position - worldPos;
        distance = length(lightVector);
        direction = distance > 1e-4 ? lightVector / distance : float3(0.0, 1.0, 0.0);
    }
}

// RIS's target: the sample's light on a diffuse surface (luminance x N.L / pi). The same in every pass.
float rtxdi_light_target_pdf(RAB_LightSample sample, float3 worldPos, float3 normal)
{
    const float luminance = max(dot(sample.radiance, float3(0.2126, 0.7152, 0.0722)), 0.0);
    if (luminance <= 1e-7)
        return 0.0;
    float3 direction;
    float distance;
    rtxdi_light_direction_distance(worldPos, sample, direction, distance);
    return luminance * (max(dot(normal, direction), 1e-3) / 3.14159265359);
}

// A local light's share of the light PDF texture (ReSTIRDIWriteLightPDF.slang): its power over 4 pi -- a point light's intensity,
// a rect light's radiance x area / 4 (pi radiance area / 4 pi), a triangle's luminance x area / 2 (both faces emit: 2 pi L A / 4 pi;
// EmissiveTriangle::power is its average luminance x area).
float rtxdi_light_importance(RAB_LightInfo light)
{
    if (rtxdi_is_triangle_light(light))
    {
        const EmissiveTriangle record = BufferRef<EmissiveTriangle>(g_RtxdiUbo.emissiveTrianglesReference)[light.triangle];
        return max(record.power * 0.5, 0.0);
    }
    const float power = dot(light.light.color.rgb, float3(0.2126, 0.7152, 0.0722)) * light.light.color.a;
    return max(is_area_light((uint)light.light.position.w) ? power * rect_light_area(light.light) * 0.25 : power, 1e-6);
}

// Where a local light is and how much it gives, for ReGIR's per-cell weights (its importance at its position; a triangle: its
// centroid).
float3 rtxdi_light_position(RAB_LightInfo light)
{
    if (rtxdi_is_triangle_light(light))
        return BufferRef<EmissiveTriangle>(g_RtxdiUbo.emissiveTrianglesReference)[light.triangle].centroid;
    return light.light.position.xyz;
}
