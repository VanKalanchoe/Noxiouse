#include "shaderIO.h"
#include "Rtxdi/GI/ReSTIRGIParameters.h"

#ifndef RTXDI_RESERVOIR_BLOCK_SIZE
#define RTXDI_RESERVOIR_BLOCK_SIZE 16
#endif

// RTXDI Macro Configuration
#define RTXDI_ENABLE_STORE_RESERVOIR 0
#define RTXDI_GI_RESERVOIR_BUFFER g_HistoryReservoirBuffer
#define RTXDI_GI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_BASIC

static RTXDI_PackedGIReservoir* g_HistoryReservoirBuffer;
static PushConstantReSTIRGITemporal g_PushConstant;
static UniformBufferObject* g_UBO;

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

float RAB_GetSurfaceLinearDepth(RAB_Surface surface)
{
    return surface.linearDepth;
}

float3 RAB_GetSurfaceNormal(RAB_Surface surface)
{
    return surface.normal;
}

float3 RAB_GetSurfaceWorldPos(RAB_Surface surface)
{
    return surface.worldPos;
}

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

bool RAB_IsSurfaceValid(RAB_Surface surface)
{
    return surface.valid;
}

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

float RAB_GetGISampleTargetPdfForSurface(float3 samplePos, float3 sampleRadiance, RAB_Surface surface)
{
    float3 toSample = samplePos - surface.worldPos;
    float distSq = dot(toSample, toSample);
    if (distSq < 1e-6) return 1e-4;
    float3 L = toSample * rsqrt(distSq);
    float NdotL = max(dot(surface.normal, L), 1e-3);
    float lum = max(dot(sampleRadiance, float3(0.2126, 0.7152, 0.0722)), 1e-3);
    return lum * (NdotL / 3.14159265359);
}

bool RAB_ValidateGISampleWithJacobian(inout float jacobian)
{
    // Matches RTXPT's RAB_ValidateGISampleWithJacobian exactly -- see ReSTIRGISpatial.slang for the
    // full rationale (unclamped jacobians leak straight through as RIS energy gain).
    if (jacobian > 10.0 || jacobian < 1.0 / 10.0)
    {
        return false;
    }
    jacobian = clamp(jacobian, 1.0 / 3.0, 3.0);
    return true;
}

RAB_Surface RAB_GetGBufferSurface(int2 pixelPos, bool isPreviousFrame)
{
    RAB_Surface s = RAB_EmptySurface();
    if (pixelPos.x < 0 || pixelPos.y < 0 ||
        pixelPos.x >= int(g_PushConstant.viewportSize.x) ||
        pixelPos.y >= int(g_PushConstant.viewportSize.y))
    {
        return s;
    }

    if (g_UBO == nullptr)
    {
        return s;
    }

    uint depthTexIndex = (isPreviousFrame && g_PushConstant.prevDepthTextureIndex != 0xFFFFFFFF)
        ? g_PushConstant.prevDepthTextureIndex
        : g_PushConstant.depthTextureIndex;
    if (depthTexIndex == 0xFFFFFFFF)
    {
        return s;
    }

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(g_UBO->imageHeapIndexOffset + depthTexIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelPos, 0)).r;

    if (rawDepth <= 0.00001)
    {
        return s;
    }

    uint normalTexIndex = (isPreviousFrame && g_PushConstant.prevNormalTextureIndex != 0xFFFFFFFF)
        ? g_PushConstant.prevNormalTextureIndex
        : g_PushConstant.gbufferNormalIndex;
    if (normalTexIndex == 0xFFFFFFFF)
    {
        return s;
    }

    // m_gbufferNormal stores raw world normals directly in [-1, 1]
    Texture2D normalTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + normalTexIndex, 0));
    float4 normalSample = normalTex.Load(int3(pixelPos, 0));
    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6)
    {
        return s;
    }
    s.normal = normalSample.rgb * rsqrt(lenSqN);

    // A previous-frame depth/normal sample was rendered with LAST frame's camera matrix -- unprojecting
    // it with this frame's matrix instead reconstructs the wrong world position the instant the camera
    // moves (silently corrupting the reprojection validity check and the jacobian, which is exactly
    // what produced the camera-motion light leaks). Use the matrix matching whichever frame's textures
    // we're actually reading.
    float2 uv = (float2(pixelPos) + 0.5) / g_PushConstant.viewportSize;
    float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 ndc = float4(ndc_xy, rawDepth, 1.0);
    float4x4 unprojectMatrix = isPreviousFrame ? g_PushConstant.prevInvViewProj : g_PushConstant.invViewProj;
    float4 worldPosH = mul(unprojectMatrix, ndc);
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

void RTXDI_ApplyPermutationSampling(inout int2 prevPixelPos, uint uniformRandomNumber)
{
    int2 offset = int2(uniformRandomNumber & 3, (uniformRandomNumber >> 2) & 3);
    prevPixelPos += offset;
    prevPixelPos.x ^= 3;
    prevPixelPos.y ^= 3;
    prevPixelPos -= offset;
}

#include "Rtxdi/GI/TemporalResampling.hlsli"

struct ReSTIRGITemporalPayload
{
    uint dummy;
};
groupshared ReSTIRGITemporalPayload gTemporalPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gTemporalPayload);
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
    uniform PushConstantReSTIRGITemporal pushConstant
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
    uniform PushConstantReSTIRGITemporal pushConstant
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
        pushConstant.currentReservoirReference == 0 ||
        pushConstant.previousReservoirReference == 0 ||
        pushConstant.depthTextureIndex == 0xFFFFFFFF ||
        pushConstant.gbufferNormalIndex == 0xFFFFFFFF)
    {
        return outVal;
    }

    g_PushConstant = pushConstant;
    g_UBO = (UniformBufferObject*)pushConstant.matrixReference;
    g_HistoryReservoirBuffer = (RTXDI_PackedGIReservoir*)pushConstant.previousReservoirReference;

    RTXDI_ReservoirBufferParameters resParams;
    resParams.reservoirBlockRowPitch = pushConstant.reservoirBlockRowPitch;
    resParams.reservoirArrayPitch = pushConstant.reservoirArrayPitch;
    resParams.pad1 = 0;
    resParams.pad2 = 0;

    uint pointer = RTXDI_ReservoirPositionToPointer(resParams, pixelCoord, 0);
    RTXDI_PackedGIReservoir* currentReservoirBuffer = (RTXDI_PackedGIReservoir*)pushConstant.currentReservoirReference;

    // Current-frame surface for this pixel (isPreviousFrame = false)
    RAB_Surface surface = RAB_GetGBufferSurface(int2(pixelCoord), false);
    if (!surface.valid)
    {
        currentReservoirBuffer[pointer] = (RTXDI_PackedGIReservoir)0;
        return outVal;
    }

    // Pass 1's initial candidate for this pixel
    RTXDI_GIReservoir inputReservoir = RTXDI_UnpackGIReservoir(currentReservoirBuffer[pointer]);

    float3 screenSpaceMotion = float3(0.0);
    if (pushConstant.gbufferVelocityIndex != 0xFFFFFFFF)
    {
        Texture2D velocityTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + pushConstant.gbufferVelocityIndex, 0));
        float2 velUV = velocityTex.Load(int3(pixelCoord, 0)).rg;
        screenSpaceMotion = float3(velUV * pushConstant.viewportSize, 0.0);
    }

    RTXDI_GITemporalResamplingParameters tparams;
    tparams.depthThreshold = pushConstant.depthThreshold;
    tparams.normalThreshold = pushConstant.normalThreshold;
    tparams.maxHistoryLength = pushConstant.maxHistoryLength;
    tparams.enableFallbackSampling = 1; // matches RTXPT's default (true)
    tparams.biasCorrectionMode = RTXDI_BIAS_CORRECTION_BASIC;
    tparams.maxReservoirAge = pushConstant.maxReservoirAge;
    tparams.enablePermutationSampling = pushConstant.enablePermutationSampling;
    tparams.uniformRandomNumber = pushConstant.frameIndex;

    RTXDI_RuntimeParameters rParams;
    rParams.neighborOffsetMask = 0;
    rParams.activeCheckerboardField = 0;
    rParams.frameIndex = pushConstant.frameIndex;
    rParams.pad2 = 0;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixelCoord, pushConstant.frameIndex, 0);

    RTXDI_GIReservoir temporalResult = RTXDI_GITemporalResampling(
        pixelCoord,
        surface,
        screenSpaceMotion,
        0, // sourceBufferIndex
        inputReservoir,
        rng,
        rParams,
        resParams,
        tparams
    );

    // Overwrite Pass 1's initial candidate in place with the temporally-resampled result -- this is
    // what the Spatial pass reads next, for both its own-pixel input and (via every neighbor pixel's
    // own temporal pass output) its spatial neighbor reservoirs.
    currentReservoirBuffer[pointer] = RTXDI_PackGIReservoir(temporalResult, 0);

    outVal.debugOut = float4(temporalResult.radiance, 1.0);
    return outVal;
}
