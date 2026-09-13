#include "shaderIO.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"

#ifndef RTXDI_RESERVOIR_BLOCK_SIZE
#define RTXDI_RESERVOIR_BLOCK_SIZE 16
#endif

// RTXDI Macro Configuration
#define RTXDI_ENABLE_STORE_RESERVOIR 1
#define RTXDI_LIGHT_RESERVOIR_BUFFER g_ReservoirBuffer

static RTXDI_PackedDIReservoir* g_ReservoirBuffer;
static PushConstantReSTIRDIInitial g_PushConstant;
static UniformBufferObject* g_UBO;
static LightData* g_LightData;
static uint2* g_RISBuffer; // [0, risBufferOffset): plain RIS tiles; [risBufferOffset, end): ReGIR cells
#define RTXDI_RIS_BUFFER g_RISBuffer

// Grid mode only (== RTXDI_REGIR_GRID, defined in Rtxdi/ReGIR/ReGIRParameters.h) -- our scenes are
// bounded interiors (like Sponza), never the open/unbounded worlds Onion mode targets. Must be defined
// before any RTXDI header that checks it -- Rtxdi/DI/ReSTIRDIParameters.h above already transitively
// pulls in ReGIRParameters.h (via RtxdiParameters.h), which defaults this to RTXDI_REGIR_DISABLED, so
// undef first to override it cleanly instead of triggering a macro-redefinition warning.
#undef RTXDI_REGIR_MODE
#define RTXDI_REGIR_MODE 1

// v1: analytic point/spot/directional lights only -- every light is punctual/delta (no area to
// integrate over), so RAB_LightInfo is just our existing LightData directly, and RAB_LightSample
// never needs the UV-parameterized area sampling RTXDI's reference (triangle/sphere/disk) lights use.
typealias RAB_LightInfo = LightData;

struct RAB_LightSample
{
    float3 position;      // unused for directional (lightType == 0)
    float3 normal;         // "L" (surface-to-light direction) for every light type; for directional
                           // lights this is LightData.direction.xyz directly, unnegated
    float3 radiance;       // already includes distance/cone attenuation
    float solidAnglePdf;   // always 1.0 -- delta lights have no solid angle to speak of
    uint lightType;        // 0 = Directional, 1 = Point, 2 = Spot (mirrors LightData.position.w)
};

struct RAB_Material
{
    uint materialID;
};

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

RAB_Material RAB_GetMaterial(RAB_Surface surface)
{
    RAB_Material m;
    m.materialID = 0;
    return m;
}

bool RAB_AreMaterialsSimilar(RAB_Material m1, RAB_Material m2)
{
    return m1.materialID == m2.materialID;
}

bool RAB_IsSurfaceValid(RAB_Surface surface) { return surface.valid; }

RAB_Surface RAB_EmptySurface()
{
    RAB_Surface s;
    s.worldPos = float3(0.0);
    s.normal = float3(0.0, 0.0, 1.0);
    s.linearDepth = 0.0;
    s.valid = false;
    return s;
}

int2 RAB_ClampSamplePositionIntoView(int2 pixelPos, bool isPreviousFrame)
{
    return clamp(pixelPos, int2(0, 0), int2(g_PushConstant.viewportSize) - 1);
}

RAB_LightInfo RAB_EmptyLightInfo()
{
    LightData l;
    l.position = float4(0.0);
    l.direction = float4(0.0);
    l.color = float4(0.0);
    l.spotParams = float4(0.0);
    return l;
}

RAB_LightSample RAB_EmptyLightSample()
{
    RAB_LightSample s;
    s.position = float3(0.0);
    s.normal = float3(0.0, 1.0, 0.0);
    s.radiance = float3(0.0);
    s.solidAnglePdf = 0.0;
    s.lightType = 0;
    return s;
}

RAB_LightInfo RAB_LoadLightInfo(uint index, bool prevFrame)
{
    return g_LightData[index];
}

// Ignores `uv` entirely -- there is no area to sample on a punctual/delta light. Radiance already
// bakes in the same distance/spot-cone attenuation formula DeferredLighting.slang's brute-force loop
// uses, so ReSTIR DI's resampled result matches the existing lighting model exactly.
RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo light, RAB_Surface surface, float2 uv)
{
    RAB_LightSample s = RAB_EmptyLightSample();
    uint lightType = (uint)light.position.w;
    s.lightType = lightType;
    s.solidAnglePdf = 1.0;

    if (lightType == 0) // Directional
    {
        // LightData.direction.xyz is already the "toward the light" vector in this engine's
        // convention (both DeferredLighting.slang's brute-force loop and PathTracer.slang use it
        // directly, unnegated, as their lighting L vector) -- do NOT negate it here.
        float3 dir = normalize(light.direction.xyz);
        s.normal = dir;
        s.radiance = light.color.rgb * light.color.a;
        return s;
    }

    float3 lightVec = light.position.xyz - surface.worldPos;
    float distance = length(lightVec);
    s.position = light.position.xyz;
    s.normal = distance > 0.0001 ? (lightVec / distance) : float3(0.0, 1.0, 0.0);

    float attenuation;
    float range = light.direction.w;
    float distSq = max(distance * distance, 1e-4);
    if (range > 0.0)
    {
        float dOverR = distance / range;
        float window = max(1.0 - pow(dOverR, 4.0), 0.0);
        attenuation = window / distSq;
    }
    else
    {
        attenuation = 1.0 / distSq;
    }

    if (lightType == 2) // Spot
    {
        float cd = dot(normalize(light.direction.xyz), -s.normal);
        float angular = saturate(cd * light.spotParams.x + light.spotParams.y);
        angular *= angular;
        attenuation *= angular;
    }

    s.radiance = light.color.rgb * light.color.a * attenuation;
    return s;
}

void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample lightSample, out float3 dir, out float dist)
{
    if (lightSample.lightType == 0) // Directional: normal already stores the L direction
    {
        dir = lightSample.normal;
        dist = 1e4;
        return;
    }

    float3 toLight = lightSample.position - surface.worldPos;
    dist = length(toLight);
    dir = dist > 0.0001 ? (toLight / dist) : float3(0.0, 1.0, 0.0);
}

// Simplified diffuse-NdotL resampling weight (mirrors RAB_GetGISampleTargetPdfForSurface's pattern
// from ReSTIR GI) -- this only needs to rank candidates for resampling, not exactly reproduce final
// shading; FinalShading.slang does the real BRDF evaluation once, after resampling has picked a light.
float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample lightSample, RAB_Surface surface)
{
    float3 dir;
    float dist;
    RAB_GetLightDirDistance(surface, lightSample, dir, dist);
    float NdotL = max(dot(surface.normal, dir), 1e-3);
    float lum = max(dot(lightSample.radiance, float3(0.2126, 0.7152, 0.0722)), 0.0);
    return lum * (NdotL / 3.14159265359);
}

float RAB_LightSampleSolidAnglePdf(RAB_LightSample lightSample) { return lightSample.solidAnglePdf; }
bool RAB_IsAnalyticLightSample(RAB_LightSample lightSample) { return true; }

// The following 7 stubs exist only because Rtxdi/DI/InitialSampling.hlsli's RTXDI_SampleBrdf /
// RTXDI_SampleEnvironmentMap reference them unconditionally (Slang compiles every function in an
// included header regardless of whether it's actually called) -- they are genuinely unreachable given
// our config: RAB_IsAnalyticLightSample always returns true above (short-circuiting RTXDI_LightBrdfMisWeight
// before it ever reaches RAB_SurfaceEvaluateBrdfPdf), and we always pass numBrdfSamples = 0 /
// environmentLightParams.lightPresent = 0 from ReSTIRDIInitial's fragMain, so RTXDI_SampleBrdf's loop
// never executes and RTXDI_SampleEnvironmentMap returns immediately. We use the SDK's real
// RTXDI_SampleLightsForSurface (rather than a hand-rolled combine) specifically so this MIS/combine
// logic matches RTXPT exactly instead of being reimplemented.
bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 dir) { dir = float3(0.0, 1.0, 0.0); return false; }
float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 dir) { return 0.0; }
bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax, out uint lightIndex, out float2 randXY) { lightIndex = RTXDI_InvalidLightIndex; randXY = float2(0.0); return false; }
float RAB_EvaluateLocalLightSourcePdf(uint lightIndex) { return 0.0; }
float RAB_EvaluateEnvironmentMapSamplingPdf(float3 dir) { return 0.0; }
float2 RAB_GetEnvironmentMapRandXYFromDir(float3 dir) { return float2(0.0); }
bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample lightSample) { return true; }

RAB_Surface RAB_GetGBufferSurface(int2 pixelPos, bool isPreviousFrame)
{
    RAB_Surface s = RAB_EmptySurface();
    if (pixelPos.x < 0 || pixelPos.y < 0 ||
        pixelPos.x >= int(g_PushConstant.viewportSize.x) ||
        pixelPos.y >= int(g_PushConstant.viewportSize.y))
    {
        return s;
    }

    if (g_UBO == nullptr) return s;

    if (g_PushConstant.depthTextureIndex == 0xFFFFFFFF) return s;
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(g_UBO->imageHeapIndexOffset + g_PushConstant.depthTextureIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelPos, 0)).r;
    if (rawDepth <= 0.00001) return s;

    if (g_PushConstant.gbufferNormalIndex == 0xFFFFFFFF) return s;
    Texture2D normalTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + g_PushConstant.gbufferNormalIndex, 0));
    float4 normalSample = normalTex.Load(int3(pixelPos, 0));
    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6) return s;
    s.normal = normalSample.rgb * rsqrt(lenSqN);

    float2 uv = (float2(pixelPos) + 0.5) / g_PushConstant.viewportSize;
    float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 ndc = float4(ndc_xy, rawDepth, 1.0);
    float4 worldPosH = mul(g_PushConstant.invViewProj, ndc);
    s.worldPos = worldPosH.xyz / worldPosH.w;

    s.linearDepth = length(s.worldPos - g_PushConstant.cameraWorldPos.xyz);
    s.valid = true;
    return s;
}

#include "Rtxdi/Utils/Math.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

#define RTXDI_RESERVOIR_ADDRESSING_HLSLI

uint2 RTXDI_PixelPosToReservoirPos(uint2 pixelPosition, uint activeCheckerboardField)
{
    return min(pixelPosition, uint2(g_PushConstant.viewportSize) - 1);
}

uint2 RTXDI_ReservoirPosToPixelPos(uint2 reservoirIndex, uint activeCheckerboardField)
{
    return reservoirIndex;
}

uint RTXDI_ReservoirPositionToPointer(
    RTXDI_ReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    uint2 clampedPos = min(reservoirPosition, uint2(g_PushConstant.viewportSize) - 1);
    uint2 blockIdx = clampedPos / RTXDI_RESERVOIR_BLOCK_SIZE;
    uint2 positionInBlock = clampedPos % RTXDI_RESERVOIR_BLOCK_SIZE;

    uint ptr = reservoirArrayIndex * reservoirParams.reservoirArrayPitch
        + blockIdx.y * reservoirParams.reservoirBlockRowPitch
        + blockIdx.x * (RTXDI_RESERVOIR_BLOCK_SIZE * RTXDI_RESERVOIR_BLOCK_SIZE)
        + positionInBlock.y * RTXDI_RESERVOIR_BLOCK_SIZE
        + positionInBlock.x;
    uint maxElements = (reservoirArrayIndex + 1u) * reservoirParams.reservoirArrayPitch;
    return min(ptr, max(maxElements, 1u) - 1u);
}

#include "Rtxdi/DI/ReservoirStorage.hlsli"

RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex)
{
    // We never use RTXDI's "compact light info" packed format -- RAB_StoreCompactLightInfo (in
    // ReSTIRDIPresampleReGIR.slang / ReSTIRDIPresample.slang) always returns false, so
    // RTXDI_LIGHT_COMPACT_BIT is never set and this is genuinely unreachable at runtime; it only needs
    // to exist because LocalLightSelection.hlsli's RTXDI_UnpackLocalLightFromRISLightData references it.
    return RAB_EmptyLightInfo();
}

// The real SDK entry point (Rtxdi/DI/InitialSampling.hlsli) -- gives us RTXDI_SampleLightsForSurface,
// which combines local (RIS/ReGIR) + infinite + environment-map + BRDF sampling with correct MIS
// weighting, exactly like RTXPT's GenerateInitialSamples.hlsl. We pass numEnvironmentSamples = 0 and
// numBrdfSamples = 0 from fragMain below, and RAB_IsAnalyticLightSample always returns true above, so
// the BRDF/environment-map code paths are genuinely dead (their few RAB_ dependencies are stubbed
// above) -- this is the exact reference combine/MIS logic, not a reimplementation of it.
#include "Rtxdi/DI/InitialSampling.hlsli"

// Builds a ReGIR_Parameters struct from our compact push-constant fields. onionParams is left
// zero-initialized: Grid mode (RTXDI_REGIR_MODE == RTXDI_REGIR_GRID) never reads it -- confirmed
// directly from ReGIRSampling.hlsli -- so there's nothing gained by transferring it from the CPU side.
ReGIR_Parameters BuildReGIRParams()
{
    ReGIR_Parameters params;
    params.commonParams.localLightSamplingFallbackMode = ReSTIRDI_LocalLightSamplingMode_POWER_RIS;
    params.commonParams.centerX = g_PushConstant.gridCenterAndCellSize.x;
    params.commonParams.centerY = g_PushConstant.gridCenterAndCellSize.y;
    params.commonParams.centerZ = g_PushConstant.gridCenterAndCellSize.z;
    params.commonParams.risBufferOffset = g_PushConstant.risBufferOffset;
    params.commonParams.lightsPerCell = g_PushConstant.lightsPerCell;
    params.commonParams.cellSize = g_PushConstant.gridCenterAndCellSize.w;
    params.commonParams.samplingJitter = g_PushConstant.regirSamplingJitter;
    params.commonParams.localLightPresamplingMode = REGIR_LOCAL_LIGHT_PRESAMPLING_MODE_POWER_RIS;
    params.commonParams.numRegirBuildSamples = 0; // only meaningful to the presample pass, unused here
    params.commonParams.pad1 = 0;
    params.commonParams.pad2 = 0;
    params.gridParams.cellsX = g_PushConstant.cellsX;
    params.gridParams.cellsY = g_PushConstant.cellsY;
    params.gridParams.cellsZ = g_PushConstant.cellsZ;
    params.gridParams.pad1 = 0;
    return params;
}

struct ReSTIRDIInitialPayload
{
    uint dummy;
};
groupshared ReSTIRDIInitialPayload gInitialPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gInitialPayload);
}

struct MSOutput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

static const float2 uvs[3] = {
    float2(0.0, 0.0),
    float2(2.0, 0.0),
    float2(0.0, 2.0)
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void meshMain(
    out indices uint3 triangles[1],
    out vertices MSOutput vertices[3],
    uint threadID : SV_GroupIndex,
    uniform PushConstantReSTIRDIInitial pushConstant
)
{
    SetMeshOutputCounts(3, 1);

    if (threadID < 3)
    {
        vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
        vertices[threadID].uv = uvs[threadID];
    }

    if (threadID == 0)
    {
        triangles[0] = uint3(0, 1, 2);
    }
}

struct PSOutput
{
    float4 debugOut : SV_Target0;
};

[shader("fragment")]
PSOutput fragMain(
    MSOutput input,
    uniform PushConstantReSTIRDIInitial pushConstant
)
{
    PSOutput outVal;
    outVal.debugOut = float4(0.0);

    uint2 pixelCoord = uint2(input.pos.xy);
    if (pixelCoord.x >= uint(pushConstant.viewportSize.x) || pixelCoord.y >= uint(pushConstant.viewportSize.y))
    {
        return outVal;
    }

    if (pushConstant.matrixReference == 0 ||
        pushConstant.lightDataReference == 0 ||
        pushConstant.reservoirBufferReference == 0 ||
        pushConstant.risBufferReference == 0 ||
        pushConstant.depthTextureIndex == 0xFFFFFFFF ||
        pushConstant.gbufferNormalIndex == 0xFFFFFFFF)
    {
        return outVal;
    }

    g_PushConstant = pushConstant;
    g_UBO = (UniformBufferObject*)pushConstant.matrixReference;
    g_LightData = (LightData*)pushConstant.lightDataReference;
    g_ReservoirBuffer = (RTXDI_PackedDIReservoir*)pushConstant.reservoirBufferReference;
    g_RISBuffer = (uint2*)pushConstant.risBufferReference;

    RTXDI_ReservoirBufferParameters resParams;
    resParams.reservoirBlockRowPitch = pushConstant.reservoirBlockRowPitch;
    resParams.reservoirArrayPitch = pushConstant.reservoirArrayPitch;
    resParams.pad1 = 0;
    resParams.pad2 = 0;

    RAB_Surface surface = RAB_GetGBufferSurface(int2(pixelCoord), false);
    if (!surface.valid)
    {
        RTXDI_StoreDIReservoir(RTXDI_EmptyDIReservoir(), resParams, pixelCoord, 0);
        return outVal;
    }

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixelCoord, pushConstant.frameIndex, 0);
    // "Coherent" across a small pixel tile, matching RTXDI's own convention (RIS tile / ReGIR cell
    // choice is meant to be shared by nearby pixels for better cache locality when reading the RIS
    // buffer -- it's not needed for correctness, only performance).
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(pixelCoord / 8, pushConstant.frameIndex, 1);

    RTXDI_DIInitialSamplingParameters sampleParams;
    sampleParams.numLocalLightSamples = pushConstant.numLocalLightSamples;
    sampleParams.numInfiniteLightSamples = pushConstant.numInfiniteLightSamples;
    sampleParams.numEnvironmentSamples = 0; // no environment map in this engine yet
    sampleParams.numBrdfSamples = 0;        // no BRDF importance sampling for analytic lights
    sampleParams.brdfCutoff = 0.0;
    sampleParams.brdfRayMinT = 0.001;
    sampleParams.localLightSamplingMode = (pushConstant.regirEnabled != 0)
        ? ReSTIRDI_LocalLightSamplingMode_REGIR_RIS
        : ReSTIRDI_LocalLightSamplingMode_POWER_RIS;
    sampleParams.enableInitialVisibility = 0; // FinalShading.slang already traces the one real shadow ray
    sampleParams.environmentMapImportanceSampling = 0;
    sampleParams.pad1 = 0;
    sampleParams.pad2 = 0;
    sampleParams.pad3 = 0;

    RTXDI_LightBufferParameters lightBufferParams;
    lightBufferParams.localLightBufferRegion.firstLightIndex = pushConstant.firstLocalLightIndex;
    lightBufferParams.localLightBufferRegion.numLights = pushConstant.numLocalLights;
    lightBufferParams.localLightBufferRegion.pad1 = 0;
    lightBufferParams.localLightBufferRegion.pad2 = 0;
    lightBufferParams.infiniteLightBufferRegion.firstLightIndex = pushConstant.firstInfiniteLightIndex;
    lightBufferParams.infiniteLightBufferRegion.numLights = pushConstant.numInfiniteLights;
    lightBufferParams.infiniteLightBufferRegion.pad1 = 0;
    lightBufferParams.infiniteLightBufferRegion.pad2 = 0;
    lightBufferParams.environmentLightParams.lightPresent = 0;
    lightBufferParams.environmentLightParams.lightIndex = 0;
    lightBufferParams.environmentLightParams.pad1 = 0;
    lightBufferParams.environmentLightParams.pad2 = 0;

    RTXDI_RISBufferSegmentParameters localLightRISBufferSegmentParams;
    localLightRISBufferSegmentParams.bufferOffset = 0; // plain RIS tiles occupy [0, risBufferOffset)
    localLightRISBufferSegmentParams.tileSize = pushConstant.risTileSize;
    localLightRISBufferSegmentParams.tileCount = pushConstant.risTileCount;
    localLightRISBufferSegmentParams.pad1 = 0;

    RTXDI_RISBufferSegmentParameters environmentLightRISBufferSegmentParams; // unused (no env light)
    environmentLightRISBufferSegmentParams.bufferOffset = 0;
    environmentLightRISBufferSegmentParams.tileSize = 0;
    environmentLightRISBufferSegmentParams.tileCount = 0;
    environmentLightRISBufferSegmentParams.pad1 = 0;

    // The real SDK entry point: combines local (RIS/ReGIR) + infinite light sampling with correct MIS
    // weighting between the two streams (environment map / BRDF terms are inert given the zeroed
    // params above) -- exactly RTXPT's own GenerateInitialSamples.hlsl call.
    RAB_LightSample selectedSample = RAB_EmptyLightSample();
    RTXDI_DIReservoir state = RTXDI_SampleLightsForSurface(rng, coherentRng, surface,
        sampleParams, lightBufferParams,
        localLightRISBufferSegmentParams, environmentLightRISBufferSegmentParams,
        BuildReGIRParams(), selectedSample);

    RTXDI_StoreDIReservoir(state, resParams, pixelCoord, 0);

    // Debug-only preview: unshadowed candidate radiance, useful while bringing this pass up.
    if (RTXDI_IsValidDIReservoir(state))
    {
        uint lightIdx = RTXDI_GetDIReservoirLightIndex(state);
        RAB_LightInfo li = RAB_LoadLightInfo(lightIdx, false);
        RAB_LightSample ls = RAB_SamplePolymorphicLight(li, surface, RTXDI_GetDIReservoirSampleUV(state));
        outVal.debugOut = float4(ls.radiance * RTXDI_GetDIReservoirInvPdf(state), 1.0);
    }

    return outVal;
}
