#include "shaderIO.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"

#ifndef RTXDI_RESERVOIR_BLOCK_SIZE
#define RTXDI_RESERVOIR_BLOCK_SIZE 16
#endif

#define RTXDI_ENABLE_STORE_RESERVOIR 1
#define RTXDI_LIGHT_RESERVOIR_BUFFER g_ReservoirBuffer

static RTXDI_PackedDIReservoir* g_ReservoirBuffer;
static PushConstantReSTIRDIInitial g_PushConstant;
static UniformBufferObject* g_UBO;

// Shared RTXDI application bridge and initial light sampler. Both ReSTIR DI primary surfaces and
// ReSTIR GI path vertices use this implementation, so their light conventions and RIS weights stay
// identical.
#include "RTXDI/RTXDILightSampling.slang"

int2 RAB_ClampSamplePositionIntoView(int2 pixelPos, bool isPreviousFrame)
{
    return clamp(pixelPos, int2(0, 0), int2(g_PushConstant.viewportSize) - 1);
}

RAB_Surface RAB_GetGBufferSurface(int2 pixelPos, bool isPreviousFrame)
{
    RAB_Surface surface = RAB_EmptySurface();
    if (pixelPos.x < 0 || pixelPos.y < 0 ||
        pixelPos.x >= int(g_PushConstant.viewportSize.x) ||
        pixelPos.y >= int(g_PushConstant.viewportSize.y) ||
        g_UBO == nullptr ||
        g_PushConstant.depthTextureIndex == 0xFFFFFFFF ||
        g_PushConstant.gbufferNormalIndex == 0xFFFFFFFF)
    {
        return surface;
    }

    Texture2D<float> depthTexture = Texture2D<float>.Handle(
        uint2(g_UBO->imageHeapIndexOffset + g_PushConstant.depthTextureIndex, 0));
    float rawDepth = depthTexture.Load(int3(pixelPos, 0)).r;
    if (rawDepth <= 0.00001)
        return surface;

    Texture2D normalTexture = Texture2D.Handle(
        uint2(g_UBO->imageHeapIndexOffset + g_PushConstant.gbufferNormalIndex, 0));
    float3 encodedNormal = normalTexture.Load(int3(pixelPos, 0)).rgb;
    float normalLengthSquared = dot(encodedNormal, encodedNormal);
    if (normalLengthSquared < 1e-6)
        return surface;

    surface.normal = encodedNormal * rsqrt(normalLengthSquared);

    float2 uv = (float2(pixelPos) + 0.5) / g_PushConstant.viewportSize;
    float2 ndcXY = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 worldPositionH = mul(g_PushConstant.invViewProj, float4(ndcXY, rawDepth, 1.0));
    surface.worldPos = worldPositionH.xyz / worldPositionH.w;
    surface.linearDepth = length(surface.worldPos - g_PushConstant.cameraWorldPos.xyz);
    surface.valid = true;
    return surface;
}

#define RTXDI_RESERVOIR_ADDRESSING_HLSLI

uint2 RTXDI_PixelPosToReservoirPos(uint2 pixelPosition, uint activeCheckerboardField)
{
    return min(pixelPosition, uint2(g_PushConstant.viewportSize) - 1);
}

uint2 RTXDI_ReservoirPosToPixelPos(uint2 reservoirPosition, uint activeCheckerboardField)
{
    return reservoirPosition;
}

uint RTXDI_ReservoirPositionToPointer(
    RTXDI_ReservoirBufferParameters reservoirParams,
    uint2 reservoirPosition,
    uint reservoirArrayIndex)
{
    uint2 clampedPosition = min(reservoirPosition, uint2(g_PushConstant.viewportSize) - 1);
    uint2 blockIndex = clampedPosition / RTXDI_RESERVOIR_BLOCK_SIZE;
    uint2 positionInBlock = clampedPosition % RTXDI_RESERVOIR_BLOCK_SIZE;

    uint pointer = reservoirArrayIndex * reservoirParams.reservoirArrayPitch
        + blockIndex.y * reservoirParams.reservoirBlockRowPitch
        + blockIndex.x * (RTXDI_RESERVOIR_BLOCK_SIZE * RTXDI_RESERVOIR_BLOCK_SIZE)
        + positionInBlock.y * RTXDI_RESERVOIR_BLOCK_SIZE
        + positionInBlock.x;
    uint maxElements = (reservoirArrayIndex + 1u) * reservoirParams.reservoirArrayPitch;
    return min(pointer, max(maxElements, 1u) - 1u);
}

#include "Rtxdi/DI/ReservoirStorage.hlsli"

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
    uniform PushConstantReSTIRDIInitial pushConstant)
{
    SetMeshOutputCounts(3, 1);
    if (threadID < 3)
    {
        vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
        vertices[threadID].uv = uvs[threadID];
    }
    if (threadID == 0)
        triangles[0] = uint3(0, 1, 2);
}

struct PSOutput
{
    float4 debugOut : SV_Target0;
};

[shader("fragment")]
PSOutput fragMain(
    MSOutput input,
    uniform PushConstantReSTIRDIInitial pushConstant)
{
    PSOutput output;
    output.debugOut = float4(0.0);

    uint2 pixelCoord = uint2(input.pos.xy);
    if (pixelCoord.x >= uint(pushConstant.viewportSize.x) ||
        pixelCoord.y >= uint(pushConstant.viewportSize.y))
    {
        return output;
    }

    if (pushConstant.matrixReference == 0 ||
        pushConstant.lightSampling.lightDataReference == 0 ||
        pushConstant.lightSampling.risBufferReference == 0 ||
        pushConstant.reservoirBufferReference == 0 ||
        pushConstant.depthTextureIndex == 0xFFFFFFFF ||
        pushConstant.gbufferNormalIndex == 0xFFFFFFFF)
    {
        return output;
    }

    g_PushConstant = pushConstant;
    g_UBO = (UniformBufferObject*)pushConstant.matrixReference;
    g_ReservoirBuffer = (RTXDI_PackedDIReservoir*)pushConstant.reservoirBufferReference;

    RTXDI_ReservoirBufferParameters reservoirParams;
    reservoirParams.reservoirBlockRowPitch = pushConstant.reservoirBlockRowPitch;
    reservoirParams.reservoirArrayPitch = pushConstant.reservoirArrayPitch;
    reservoirParams.pad1 = 0;
    reservoirParams.pad2 = 0;

    RAB_Surface surface = RAB_GetGBufferSurface(int2(pixelCoord), false);
    if (!surface.valid)
    {
        RTXDI_StoreDIReservoir(RTXDI_EmptyDIReservoir(), reservoirParams, pixelCoord, 0);
        return output;
    }

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixelCoord, pushConstant.frameIndex, 0);
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(pixelCoord / 8, pushConstant.frameIndex, 1);

    RAB_LightSample selectedSample;
    RTXDI_DIReservoir reservoir = RTXDI_SampleLightsAt(
        surface, rng, coherentRng, pushConstant.lightSampling, selectedSample);
    RTXDI_StoreDIReservoir(reservoir, reservoirParams, pixelCoord, 0);

    return output;
}
