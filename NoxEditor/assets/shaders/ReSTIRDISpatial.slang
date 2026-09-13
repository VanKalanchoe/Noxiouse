#include "shaderIO.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"

#ifndef RTXDI_RESERVOIR_BLOCK_SIZE
#define RTXDI_RESERVOIR_BLOCK_SIZE 16
#endif

// RTXDI Macro Configuration
//
// RTXDI_DISpatialResampling only ever calls RTXDI_LoadDIReservoir/RTXDI_LIGHT_RESERVOIR_BUFFER
// internally to fetch the CENTER pixel's own input (re-read at the top) and every NEIGHBOR's
// reservoir -- all from buffer A (this frame's already-temporally-resampled data), never from the
// output buffer. The final result must be written to a SEPARATE buffer (B) -- writing back into A
// would race every neighboring pixel's own in-flight read of A within this same dispatch (the exact
// bug already found and fixed once for ReSTIR GI's spatial pass).
#define RTXDI_ENABLE_STORE_RESERVOIR 0
#define RTXDI_LIGHT_RESERVOIR_BUFFER g_InputReservoirBuffer
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER g_NeighborOffsetsBuffer
#define RTXDI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_BASIC

static RTXDI_PackedDIReservoir* g_InputReservoirBuffer;  // buffer A: read only
static RTXDI_PackedDIReservoir* g_OutputReservoirBuffer; // buffer B: write only
static float2* g_NeighborOffsetsBuffer; // reuses the same buffer/format as ReSTIR GI's spatial pass
static PushConstantReSTIRDISpatial g_PushConstant;
static UniformBufferObject* g_UBO;
static LightData* g_LightData;

typealias RAB_LightInfo = LightData;

struct RAB_LightSample
{
    float3 position;
    float3 normal;
    float3 radiance;
    float solidAnglePdf;
    uint lightType;
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

int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
    return int(lightIndex);
}

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
    if (lightSample.lightType == 0)
    {
        dir = lightSample.normal;
        dist = 1e4;
        return;
    }

    float3 toLight = lightSample.position - surface.worldPos;
    dist = length(toLight);
    dir = dist > 0.0001 ? (toLight / dist) : float3(0.0, 1.0, 0.0);
}

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

// Spatial resampling always operates on the current frame -- no previous-frame G-buffer needed here.
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
#include "Rtxdi/DI/SpatialResampling.hlsli"

struct ReSTIRDISpatialPayload
{
    uint dummy;
};
groupshared ReSTIRDISpatialPayload gSpatialPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gSpatialPayload);
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
    uniform PushConstantReSTIRDISpatial pushConstant
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
    uniform PushConstantReSTIRDISpatial pushConstant
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
        pushConstant.inputReservoirReference == 0 ||
        pushConstant.outputReservoirReference == 0 ||
        pushConstant.neighborOffsetsReference == 0 ||
        pushConstant.depthTextureIndex == 0xFFFFFFFF ||
        pushConstant.gbufferNormalIndex == 0xFFFFFFFF)
    {
        return outVal;
    }

    g_PushConstant = pushConstant;
    g_UBO = (UniformBufferObject*)pushConstant.matrixReference;
    g_LightData = (LightData*)pushConstant.lightDataReference;
    g_InputReservoirBuffer = (RTXDI_PackedDIReservoir*)pushConstant.inputReservoirReference;
    g_OutputReservoirBuffer = (RTXDI_PackedDIReservoir*)pushConstant.outputReservoirReference;
    g_NeighborOffsetsBuffer = (float2*)pushConstant.neighborOffsetsReference;

    RTXDI_ReservoirBufferParameters resParams;
    resParams.reservoirBlockRowPitch = pushConstant.reservoirBlockRowPitch;
    resParams.reservoirArrayPitch = pushConstant.reservoirArrayPitch;
    resParams.pad1 = 0;
    resParams.pad2 = 0;

    uint pointer = RTXDI_ReservoirPositionToPointer(resParams, pixelCoord, 0);

    RAB_Surface surface = RAB_GetGBufferSurface(int2(pixelCoord), false);
    if (!surface.valid)
    {
        g_OutputReservoirBuffer[pointer] = RTXDI_PackDIReservoir(RTXDI_EmptyDIReservoir());
        return outVal;
    }

    RTXDI_DIReservoir centerSample = RTXDI_UnpackDIReservoir(g_InputReservoirBuffer[pointer]);

    RTXDI_RuntimeParameters rParams;
    rParams.neighborOffsetMask = pushConstant.neighborOffsetMask;
    rParams.activeCheckerboardField = 0;
    rParams.frameIndex = pushConstant.frameIndex;
    rParams.pad2 = 0;

    RTXDI_DISpatialResamplingParameters sparams;
    sparams.numSamples = pushConstant.numSamples;
    sparams.numDisocclusionBoostSamples = 8; // matches RTXPT's default; inert while targetHistoryLength == 0 below (matches RTXPT's own default too -- M is unsigned so M < 0 never triggers the boost)
    sparams.samplingRadius = pushConstant.samplingRadius;
    sparams.biasCorrectionMode = RTXDI_BIAS_CORRECTION_BASIC;
    sparams.depthThreshold = pushConstant.depthThreshold;
    sparams.normalThreshold = pushConstant.normalThreshold;
    sparams.targetHistoryLength = 0;
    sparams.enableMaterialSimilarityTest = 1; // matches RTXPT; currently a no-op here since RAB_GetMaterial always reports materialID 0 (no per-pixel material ID plumbed into the G-buffer yet)
    sparams.discountNaiveSamples = 1;
    sparams.pad1 = 0;
    sparams.pad2 = 0;
    sparams.pad3 = 0;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixelCoord, pushConstant.frameIndex, 3);
    RAB_LightSample selectedLightSample = RAB_EmptyLightSample();

    RTXDI_DIReservoir resampledReservoir = RTXDI_DISpatialResampling(
        pixelCoord,
        surface,
        centerSample,
        rng,
        rParams,
        resParams,
        0, // sourceBufferIndex: g_InputReservoirBuffer is its own dedicated physical buffer
        sparams,
        selectedLightSample
    );

    g_OutputReservoirBuffer[pointer] = RTXDI_PackDIReservoir(resampledReservoir);

    if (RTXDI_IsValidDIReservoir(resampledReservoir))
    {
        outVal.debugOut = float4(selectedLightSample.radiance * RTXDI_GetDIReservoirInvPdf(resampledReservoir), 1.0);
    }

    return outVal;
}
