// Shared RTXDI application bridge for sampling the engine's analytic lights at an arbitrary surface.
#ifndef RTXDI_LIGHT_SAMPLING_SLANG
#define RTXDI_LIGHT_SAMPLING_SLANG

#include "shaderIO.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"

static LightData* g_LightData;
static uint2* g_RISBuffer;
#define RTXDI_RIS_BUFFER g_RISBuffer

#undef RTXDI_REGIR_MODE
#define RTXDI_REGIR_MODE 1 // RTXDI_REGIR_GRID

typealias RAB_LightInfo = LightData;

struct RAB_LightSample
{
    float3 position;
    float3 normal;       // surface-to-light direction for directional lights
    float3 radiance;     // includes range and cone attenuation
    float solidAnglePdf;
    uint lightType;
};

struct RAB_Material { uint materialID; };

struct RAB_Surface
{
    float3 worldPos;
    float3 normal;
    float linearDepth;
    bool valid;
};

float RAB_GetSurfaceLinearDepth(RAB_Surface surface) { return surface.linearDepth; }
float3 RAB_GetSurfaceNormal(RAB_Surface surface) { return surface.normal; }
float3 RAB_GetSurfaceWorldPos(RAB_Surface surface) { return surface.worldPos; }
RAB_Material RAB_GetMaterial(RAB_Surface surface) { RAB_Material m; m.materialID = 0; return m; }
bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b) { return a.materialID == b.materialID; }
bool RAB_IsSurfaceValid(RAB_Surface surface) { return surface.valid; }

RAB_Surface RAB_EmptySurface()
{
    RAB_Surface surface;
    surface.worldPos = float3(0.0);
    surface.normal = float3(0.0, 0.0, 1.0);
    surface.linearDepth = 0.0;
    surface.valid = false;
    return surface;
}

RAB_LightInfo RAB_EmptyLightInfo()
{
    LightData light;
    light.position = float4(0.0);
    light.direction = float4(0.0);
    light.color = float4(0.0);
    light.spotParams = float4(0.0);
    return light;
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

RAB_LightInfo RAB_LoadLightInfo(uint index, bool previousFrame) { return g_LightData[index]; }

void RAB_BuildBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    float sign = normal.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + normal.z);
    float b = normal.x * normal.y * a;
    tangent = float3(1.0 + sign * normal.x * normal.x * a, sign * b, -sign * normal.x);
    bitangent = float3(b, sign + normal.y * normal.y * a, -normal.y);
}

float2 RAB_SampleDisk(float2 random)
{
    float radius = sqrt(saturate(random.x));
    float phi = 6.28318530718 * frac(random.y);
    return radius * float2(cos(phi), sin(phi));
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo light, RAB_Surface surface, float2 random)
{
    // The engine's light sampling (shaderIO.h, shared with the rest of the renderer): a point on the light -- the sun's cone, a sphere
    // light's disc, a rect light's area -- and the light from it with the falloff, cone and a rect's area / distance^2 folded in.
    const LightPointSample lightPoint = sample_light_point(light, surface.worldPos, random);
    RAB_LightSample s;
    s.lightType = (uint)light.position.w;
    s.position = lightPoint.position;
    s.normal = lightPoint.direction; // directional: the direction itself; local: towards the sampled point
    s.radiance = lightPoint.radiance;
    s.solidAnglePdf = 1.0;
    return s;
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample sample, out float3 direction, out float distance)
{
    if (sample.lightType == 0)
    {
        direction = sample.normal;
        distance = 1e4;
    }
    else
    {
        float3 lightVector = sample.position - surface.worldPos;
        distance = length(lightVector);
        direction = distance > 1e-4 ? lightVector / distance : float3(0.0, 1.0, 0.0);
    }
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample sample, RAB_Surface surface)
{
    float luminance = max(dot(sample.radiance, float3(0.2126, 0.7152, 0.0722)), 0.0);
    if (luminance <= 1e-7)
        return 0.0;
    float3 direction;
    float distance;
    RAB_GetLightDirDistance(surface, sample, direction, distance);
    return luminance * (max(dot(surface.normal, direction), 1e-3) / 3.14159265359);
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample sample) { return sample.solidAnglePdf; }
bool RAB_IsAnalyticLightSample(RAB_LightSample sample) { return true; }
bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 direction) { direction = float3(0.0, 1.0, 0.0); return false; }
float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 direction) { return 0.0; }
bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float minT, float maxT, out uint lightIndex, out float2 random) { lightIndex = RTXDI_InvalidLightIndex; random = float2(0.0); return false; }
float RAB_EvaluateLocalLightSourcePdf(uint lightIndex) { return 0.0; }
float RAB_EvaluateEnvironmentMapSamplingPdf(float3 direction) { return 0.0; }
float2 RAB_GetEnvironmentMapRandXYFromDir(float3 direction) { return float2(0.0); }
bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample sample) { return true; }

#include "Rtxdi/Utils/Math.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex) { return RAB_EmptyLightInfo(); }

#include "Rtxdi/DI/InitialSampling.hlsli"

ReGIR_Parameters BuildReGIRParams(RTXDILightSamplingParams sampling)
{
    ReGIR_Parameters params;
    params.commonParams.localLightSamplingFallbackMode = ReSTIRDI_LocalLightSamplingMode_POWER_RIS;
    params.commonParams.centerX = sampling.gridCenterAndCellSize.x;
    params.commonParams.centerY = sampling.gridCenterAndCellSize.y;
    params.commonParams.centerZ = sampling.gridCenterAndCellSize.z;
    params.commonParams.risBufferOffset = sampling.risBufferOffset;
    params.commonParams.lightsPerCell = sampling.lightsPerCell;
    params.commonParams.cellSize = sampling.gridCenterAndCellSize.w;
    params.commonParams.samplingJitter = sampling.regirSamplingJitter;
    params.commonParams.localLightPresamplingMode = REGIR_LOCAL_LIGHT_PRESAMPLING_MODE_POWER_RIS;
    params.commonParams.numRegirBuildSamples = 0;
    params.commonParams.pad1 = 0;
    params.commonParams.pad2 = 0;
    params.gridParams.cellsX = sampling.cellsX;
    params.gridParams.cellsY = sampling.cellsY;
    params.gridParams.cellsZ = sampling.cellsZ;
    params.gridParams.pad1 = 0;
    return params;
}

RTXDI_DIReservoir RTXDI_SampleLightsAt(
    RAB_Surface surface,
    inout RTXDI_RandomSamplerState rng,
    inout RTXDI_RandomSamplerState coherentRng,
    RTXDILightSamplingParams sampling,
    out RAB_LightSample selectedSample)
{
    g_LightData = (LightData*)sampling.lightDataReference;
    g_RISBuffer = (uint2*)sampling.risBufferReference;

    RTXDI_DIInitialSamplingParameters initial;
    initial.numLocalLightSamples = sampling.numLocalLightSamples;
    initial.numInfiniteLightSamples = sampling.numInfiniteLightSamples;
    initial.numEnvironmentSamples = 0;
    initial.numBrdfSamples = 0;
    initial.brdfCutoff = 0.0;
    initial.brdfRayMinT = 0.001;
    initial.localLightSamplingMode = sampling.regirEnabled != 0
        ? ReSTIRDI_LocalLightSamplingMode_REGIR_RIS
        : ReSTIRDI_LocalLightSamplingMode_POWER_RIS;
    initial.enableInitialVisibility = 0;
    initial.environmentMapImportanceSampling = 0;
    initial.pad1 = 0;
    initial.pad2 = 0;
    initial.pad3 = 0;

    RTXDI_LightBufferParameters lights;
    lights.localLightBufferRegion.firstLightIndex = sampling.firstLocalLightIndex;
    lights.localLightBufferRegion.numLights = sampling.numLocalLights;
    lights.localLightBufferRegion.pad1 = 0;
    lights.localLightBufferRegion.pad2 = 0;
    lights.infiniteLightBufferRegion.firstLightIndex = sampling.firstInfiniteLightIndex;
    lights.infiniteLightBufferRegion.numLights = sampling.numInfiniteLights;
    lights.infiniteLightBufferRegion.pad1 = 0;
    lights.infiniteLightBufferRegion.pad2 = 0;
    lights.environmentLightParams.lightPresent = 0;
    lights.environmentLightParams.lightIndex = 0;
    lights.environmentLightParams.pad1 = 0;
    lights.environmentLightParams.pad2 = 0;

    RTXDI_RISBufferSegmentParameters localRIS;
    localRIS.bufferOffset = 0;
    localRIS.tileSize = sampling.risTileSize;
    localRIS.tileCount = sampling.risTileCount;
    localRIS.pad1 = 0;
    RTXDI_RISBufferSegmentParameters environmentRIS;
    environmentRIS.bufferOffset = 0;
    environmentRIS.tileSize = 0;
    environmentRIS.tileCount = 0;
    environmentRIS.pad1 = 0;

    selectedSample = RAB_EmptyLightSample();
    return RTXDI_SampleLightsForSurface(rng, coherentRng, surface, initial, lights,
        localRIS, environmentRIS, BuildReGIRParams(sampling), selectedSample);
}

#endif
