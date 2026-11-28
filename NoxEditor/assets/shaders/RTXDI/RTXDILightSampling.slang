// Shared RTXDI application bridge for sampling the lights (RTXDILights.slang: analytic lights and emissive triangles) at an arbitrary
// surface: ReSTIR DI's initial samples and ReSTIR GI's path vertices.
#ifndef RTXDI_LIGHT_SAMPLING_SLANG
#define RTXDI_LIGHT_SAMPLING_SLANG

#include "shaderIO.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"

// The lights: RTXDILights.slang (the same index space, sampling and weights in every pass that resamples them).
#include "RTXDI/RTXDILights.slang"

static BufferRef<uint2> g_RISBuffer;
#define RTXDI_RIS_BUFFER g_RISBuffer

#undef RTXDI_REGIR_MODE
#define RTXDI_REGIR_MODE 1 // RTXDI_REGIR_GRID

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
    return rtxdi_sample_light(light, surface.worldPos, random);
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample sample, out float3 direction, out float distance)
{
    rtxdi_light_direction_distance(surface.worldPos, sample, direction, distance);
}

float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample sample, RAB_Surface surface)
{
    return rtxdi_light_target_pdf(sample, surface.worldPos, surface.normal);
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
    UniformBufferObject ubo,
    RAB_Surface surface,
    inout RTXDI_RandomSamplerState rng,
    inout RTXDI_RandomSamplerState coherentRng,
    RTXDILightSamplingParams sampling,
    out RAB_LightSample selectedSample)
{
    rtxdi_bind_lights(ubo, sampling.lightDataReference);
    g_RISBuffer = BufferRef<uint2>(sampling.risBufferReference);

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
