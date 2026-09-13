#include "shaderIO.h"
#include "Rtxdi/PT/ReSTIRPTParameters.h"
#include "Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli"

#ifndef RTXDI_RESERVOIR_BLOCK_SIZE
#define RTXDI_RESERVOIR_BLOCK_SIZE 16
#endif

static RTXDI_PackedPTReservoir* g_ReservoirBuffer;
#define RTXDI_PT_RESERVOIR_BUFFER g_ReservoirBuffer
static RTXDI_PackedPTReservoir* g_PreservedReservoirBuffer;

static PushConstantReSTIRPTFinalShading g_PushConstant;
static UniformBufferObject* g_UBO;

// NRD REBLUR YCoCg conversion (matching NRD.hlsli:398, same fix already applied to ReSTIR GI/DI/plain
// path tracer) -- REBLUR's internal filtering assumes IN_DIFF_RADIANCE_HITDIST is already YCoCg-encoded.
float3 LinearToYCoCg(float3 color)
{
    float Y = dot(color, float3(0.25, 0.5, 0.25));
    float Co = dot(color, float3(0.5, 0.0, -0.5));
    float Cg = dot(color, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

#include "Rtxdi/Utils/Math.hlsli"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

#define RTXDI_RESERVOIR_ADDRESSING_HLSLI

uint2 RTXDI_PixelPosToReservoirPos(uint2 pixelPosition, uint activeCheckerboardField)
{
    return min(pixelPosition, uint2(g_PushConstant.viewportSize) - 1);
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

#include "Rtxdi/PT/Reservoir.hlsli"

struct ReSTIRPTFinalShadingPayload
{
    uint dummy;
};
groupshared ReSTIRPTFinalShadingPayload gFinalShadingPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gFinalShadingPayload);
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
    uniform PushConstantReSTIRPTFinalShading pushConstant
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
    float4 directLighting : SV_Target0;
};

// v1 final shading: the reservoir's targetFunction * weightSum IS already the complete final radiance
// (the standard RIS unbiased estimator, same pattern as every other RTXDI pass this session) -- no
// re-evaluation of the primary surface's BSDF needed, because ptState.pathThroughput (baked into
// targetFunction during RAB_PathTrace/RISStreamPathSample) already includes the primary surface's own
// BSDF*NdotL/pdf term from the very first bounce onward. The real reference's FinalShading.hlsl DOES
// re-evaluate BSDF here, but only to split the SAME total into separate diffuse/specular channels for
// its NRD denoiser's guide buffers -- an orthogonal concern (denoiser plumbing) we don't need since our
// output is one combined signal, not a correctness requirement of the resampling algorithm itself.
[shader("fragment")]
PSOutput fragMain(
    MSOutput input,
    uniform PushConstantReSTIRPTFinalShading pushConstant
)
{
    PSOutput outVal;
    outVal.directLighting = float4(0.0, 0.0, 0.0, 1.0);

    uint2 pixelCoord = uint2(input.pos.xy);
    if (pixelCoord.x >= uint(pushConstant.viewportSize.x) || pixelCoord.y >= uint(pushConstant.viewportSize.y))
        return outVal;

    if (pushConstant.reservoirReference == 0)
        return outVal;

    g_PushConstant = pushConstant;
    g_UBO = (UniformBufferObject*)pushConstant.matrixReference;
    g_ReservoirBuffer = (RTXDI_PackedPTReservoir*)pushConstant.reservoirReference;
    g_PreservedReservoirBuffer = (RTXDI_PackedPTReservoir*)pushConstant.preservedReservoirReference;

    RTXDI_ReservoirBufferParameters resParams;
    resParams.reservoirBlockRowPitch = pushConstant.reservoirBlockRowPitch;
    resParams.reservoirArrayPitch = pushConstant.reservoirArrayPitch;
    resParams.pad1 = 0;
    resParams.pad2 = 0;

    // Resampled indirect (bounce 2+) radiance -- see RTXDI_PathTracerState.hlsli's own comment on why
    // RAB_PathTrace starts at bounceDepth=2: the reservoir never contains the primary surface's own
    // direct lighting by design, matching how this engine already splits ReSTIR GI (indirect) from
    // DeferredLighting (direct).
    float3 result = float3(0.0, 0.0, 0.0);
    RTXDI_PTReservoir reservoir = RTXDI_LoadPTReservoir(resParams, pixelCoord, 0);
    if (pushConstant.preservedReservoirReference != 0 &&
        pushConstant.decorrelationMode == RTXDI_PT_DECORRELATION_MODE_UNIFORM &&
        pushConstant.decorrelationFactor > 0.0)
    {
        RTXDI_RandomSamplerState decorrelationRng = RTXDI_InitRandomSampler(pixelCoord, pushConstant.frameIndex, RTXDI_PT_FINAL_SHADING_RANDOM_SEED);
        if (RTXDI_GetNextRandom(decorrelationRng) < saturate(pushConstant.decorrelationFactor))
            reservoir = RTXDI_UnpackPTReservoir(g_PreservedReservoirBuffer[RTXDI_ReservoirPositionToPointer(resParams, pixelCoord, 0)]);
    }

    if (RTXDI_IsValidPTReservoir(reservoir))
        result = reservoir.targetFunction * reservoir.weightSum;

    // Add back the primary-surface (bounce-1) direct lighting written by the Initial Sampling pass.
    if (pushConstant.primaryDirectTextureIndex != 0xFFFFFFFF)
    {
        Texture2D primaryDirectTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + pushConstant.primaryDirectTextureIndex, 0));
        result += primaryDirectTex.Load(int3(pixelCoord, 0)).rgb;
    }

    if (any(isnan(result)) || any(isinf(result)))
        result = float3(0.0, 0.0, 0.0);

    if (pushConstant.denoiserMode == 1) // NRD REBLUR: encode into YCoCg before writing
        result = LinearToYCoCg(result);

    outVal.directLighting = float4(result, 1.0);
    return outVal;
}
