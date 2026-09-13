#include "shaderIO.h"

// Builds ReGIR grid cells: one compute thread per (cell, slot-within-cell), i.e.
// dispatchThreadID.x in [0, cellsX*cellsY*cellsZ*lightsPerCell). Calls the SDK's own
// RTXDI_PresampleLocalLightsForReGIR (Rtxdi/LightSampling/PresamplingFunctions.hlsli) directly --
// exactly what RTXPT's own PresampleReGIR.hlsl does -- instead of hand-rolling the build loop, so the
// per-cell weighting (RAB_GetLightTargetPdfForVolume below) and the multi-sample RIS build
// (numRegirBuildSamples draws combined via streaming RIS, not a single full-light-list pass) both
// match the reference implementation exactly.

static LightData* g_LightData;
static uint2* g_RISBuffer;
static PushConstantReSTIRDIPresampleReGIR g_PushConstant;
#define RTXDI_RIS_BUFFER g_RISBuffer

// Grid mode only (== RTXDI_REGIR_GRID) -- our scenes are bounded interiors (like Sponza), never the
// open/unbounded worlds Onion mode targets. Must be defined before any RTXDI header that checks it.
#define RTXDI_REGIR_MODE 1

typealias RAB_LightInfo = LightData;

RAB_LightInfo RAB_EmptyLightInfo()
{
    LightData l;
    l.position = float4(0.0);
    l.direction = float4(0.0);
    l.color = float4(0.0);
    l.spotParams = float4(0.0);
    return l;
}

RAB_LightInfo RAB_LoadLightInfo(uint index, bool prevFrame)
{
    return g_LightData[index];
}

// We never use RTXDI's "compact light info" packed format -- our light counts are small enough that
// plain index + RAB_LoadLightInfo is simpler and just as fast. Returning false tells the SDK "not
// compact", so the consuming side (ReSTIRDIInitial.slang) always falls through to RAB_LoadLightInfo.
bool RAB_StoreCompactLightInfo(uint risBufferPtr, RAB_LightInfo lightInfo) { return false; }
RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex) { return RAB_EmptyLightInfo(); }

#include "Rtxdi/Utils/Math.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/ReGIR/ReGIRParameters.h"

// Matches RTXPT's PolymorphicLight::GetWeightForVolume / getAverageDistanceToVolume exactly (see
// PolymorphicLight.hlsli): smoothly floors the effective distance at ~0.75*volumeRadius as the light
// approaches the volume center instead of letting 1/distance^2 spike to near-infinity. Without this,
// a light merely landing near (or inside) a grid cell would get a weight cliff that made ReGIR flicker
// (see conversation history -- this was the actual bug behind "ReGIR makes it worse").
float RAB_GetLightTargetPdfForVolume(RAB_LightInfo light, float3 volumeCenter, float volumeRadius)
{
    float lum = max(dot(light.color.rgb, float3(0.2126, 0.7152, 0.0722)) * light.color.a, 1e-6);
    float distToCenter = length(light.position.xyz - volumeCenter);

    const float nonlinearFactor = 1.1547;
    float denom = distToCenter + volumeRadius * nonlinearFactor;
    float dist = distToCenter + (volumeRadius * volumeRadius * volumeRadius) / (denom * denom);

    float distSq = max(dist * dist, 1e-4);
    return lum / distSq;
}

#include "Rtxdi/LightSampling/LocalLightSelection.hlsli"
#include "Rtxdi/ReGIR/ReGIRSampling.hlsli"
#include "Rtxdi/LightSampling/PresamplingFunctions.hlsli"

[shader("compute")]
[numthreads(64, 1, 1)]
void compMain(uint3 dispatchThreadID : SV_DispatchThreadID, uniform PushConstantReSTIRDIPresampleReGIR pushConstant)
{
    uint totalCells = pushConstant.cellsX * pushConstant.cellsY * pushConstant.cellsZ;
    uint lightSlot = dispatchThreadID.x;
    if (lightSlot >= totalCells * pushConstant.lightsPerCell)
        return;

    g_PushConstant = pushConstant;
    g_LightData = (LightData*)pushConstant.lightDataReference;
    g_RISBuffer = (uint2*)pushConstant.risBufferReference;

    ReGIR_Parameters regirParams;
    regirParams.commonParams.localLightSamplingFallbackMode = ReSTIRDI_LocalLightSamplingMode_POWER_RIS;
    regirParams.commonParams.centerX = pushConstant.gridCenterAndCellSize.x;
    regirParams.commonParams.centerY = pushConstant.gridCenterAndCellSize.y;
    regirParams.commonParams.centerZ = pushConstant.gridCenterAndCellSize.z;
    regirParams.commonParams.risBufferOffset = pushConstant.risBufferOffset;
    regirParams.commonParams.lightsPerCell = pushConstant.lightsPerCell;
    regirParams.commonParams.cellSize = pushConstant.gridCenterAndCellSize.w;
    regirParams.commonParams.samplingJitter = pushConstant.regirSamplingJitter;
    regirParams.commonParams.localLightPresamplingMode = REGIR_LOCAL_LIGHT_PRESAMPLING_MODE_POWER_RIS;
    regirParams.commonParams.numRegirBuildSamples = pushConstant.numRegirBuildSamples;
    regirParams.commonParams.pad1 = 0;
    regirParams.commonParams.pad2 = 0;
    regirParams.gridParams.cellsX = pushConstant.cellsX;
    regirParams.gridParams.cellsY = pushConstant.cellsY;
    regirParams.gridParams.cellsZ = pushConstant.cellsZ;
    regirParams.gridParams.pad1 = 0;
    // onionParams intentionally left zero-initialized: Grid mode never reads it (confirmed directly
    // from ReGIRSampling.hlsli -- only Onion mode's cell math touches it).

    RTXDI_LightBufferRegion localLightBufferRegion;
    localLightBufferRegion.firstLightIndex = pushConstant.firstLocalLightIndex;
    localLightBufferRegion.numLights = pushConstant.numLocalLights;
    localLightBufferRegion.pad1 = 0;
    localLightBufferRegion.pad2 = 0;

    RTXDI_RISBufferSegmentParameters localLightRISBufferSegmentParams;
    localLightRISBufferSegmentParams.bufferOffset = 0; // plain RIS tiles occupy [0, risBufferOffset)
    localLightRISBufferSegmentParams.tileSize = pushConstant.risTileSize;
    localLightRISBufferSegmentParams.tileCount = pushConstant.risTileCount;
    localLightRISBufferSegmentParams.pad1 = 0;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(uint2(lightSlot, 1), pushConstant.frameIndex, 5);
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(uint2(lightSlot / max(pushConstant.lightsPerCell, 1u), 1), pushConstant.frameIndex, 6);

    RTXDI_PresampleLocalLightsForReGIR(
        rng,
        coherentRng,
        lightSlot,
        localLightBufferRegion,
        localLightRISBufferSegmentParams,
        regirParams);
}
