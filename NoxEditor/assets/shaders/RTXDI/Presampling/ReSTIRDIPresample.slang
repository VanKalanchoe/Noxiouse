#include "shaderIO.h"

// Builds RIS tiles of power-weighted local-light candidates. One compute thread per RIS buffer slot
// (dispatchThreadID.x in [0, risTileSize * risTileCount)). Calls the SDK's own RTXDI_PresampleLocalLights
// (Rtxdi/LightSampling/PresamplingFunctions.hlsli) directly against the light PDF mip chain built by
// ReSTIRDIWriteLightPDF.slang / ReSTIRDIReduceLightPDFMip.slang, exactly what RTXPT's own
// PresampleLights.hlsl does -- this is the real hierarchical-mip presampling the SDK is designed
// around, scaling correctly to scenes with thousands of lights instead of the O(numLights) per-thread
// weighted draw this file used to do.

static LightData* g_LightData;
static uint2* g_RISBuffer;
#define RTXDI_RIS_BUFFER g_RISBuffer

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
#include "Rtxdi/LightSampling/PresamplingFunctions.hlsli"

[shader("compute")]
[numthreads(64, 1, 1)]
void compMain(uint3 dispatchThreadID : SV_DispatchThreadID, uniform PushConstantReSTIRDIPresample pushConstant)
{
    uint slot = dispatchThreadID.x;
    if (slot >= pushConstant.risTileSize * pushConstant.risTileCount)
        return;

    g_LightData = (LightData*)pushConstant.lightDataReference;
    g_RISBuffer = (uint2*)pushConstant.risBufferReference;

    if (pushConstant.numLocalLights == 0)
    {
        g_RISBuffer[slot] = uint2(0, asuint(0.0));
        return;
    }

    uint tileIndex = slot / pushConstant.risTileSize;
    uint sampleInTile = slot % pushConstant.risTileSize;

    RTXDI_LightBufferRegion localLightBufferRegion;
    localLightBufferRegion.firstLightIndex = pushConstant.firstLocalLightIndex;
    localLightBufferRegion.numLights = pushConstant.numLocalLights;
    localLightBufferRegion.pad1 = 0;
    localLightBufferRegion.pad2 = 0;

    // Our plain local-light RIS tiles already occupy [0, risBufferOffset) at the start of the shared
    // RIS buffer -- RTXDI_PresampleLocalLights's own risBufferPtr math doesn't add bufferOffset at all
    // (confirmed directly from PresamplingFunctions.hlsli), so this only works because that segment
    // genuinely starts at offset 0 in our layout; don't move it without revisiting this.
    RTXDI_RISBufferSegmentParameters localLightsRISBufferSegmentParams;
    localLightsRISBufferSegmentParams.bufferOffset = 0;
    localLightsRISBufferSegmentParams.tileSize = pushConstant.risTileSize;
    localLightsRISBufferSegmentParams.tileCount = pushConstant.risTileCount;
    localLightsRISBufferSegmentParams.pad1 = 0;

    // RTXDI_TEX2D (RtxdiTypes.h) is a plain Texture2D, which HLSL/Slang default to a 4-component
    // format -- the underlying resource is still single-channel R32_SFLOAT; only .x is meaningful.
    Texture2D pdfTexture = Texture2D.Handle(uint2(pushConstant.pdfTextureIndex, 0));
    uint2 pdfTextureSize = uint2(pushConstant.pdfTextureSize, pushConstant.pdfTextureSize);

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(uint2(slot, 0), pushConstant.frameIndex, 4);

    RTXDI_PresampleLocalLights(rng, pdfTexture, pdfTextureSize, tileIndex, sampleInTile,
        localLightBufferRegion, localLightsRISBufferSegmentParams);
}
