#include "shaderIO.h"
#include "Rtxdi/GI/ReSTIRGIParameters.h"

#ifndef RTXDI_RESERVOIR_BLOCK_SIZE
#define RTXDI_RESERVOIR_BLOCK_SIZE 16
#endif

// RTXDI Macro Configuration
#define RTXDI_ENABLE_STORE_RESERVOIR 0
#define RTXDI_GI_RESERVOIR_BUFFER g_CurrentReservoirBuffer
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER g_NeighborOffsetsBuffer
#define RTXDI_GI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_BASIC

// Bound to THIS frame's already-temporally-resampled reservoir buffer (written by
// ReSTIRGITemporal.slang), used both for the current pixel's own input and for every spatial
// neighbor read -- this is the key architectural difference from the old fused pass, whose
// "spatial" phase read stale previous-frame history for neighbors instead of this frame's data.
// READ ONLY for the entire pass: writing the resampled result back into this same buffer would
// race against neighboring pixels' invocations still reading it (RTXDI's own reference keeps
// spatial input/output as separate buffers for exactly this reason -- see
// ReSTIRGIContext::UpdateBufferIndices, spatialResamplingInputBufferIndex vs …OutputBufferIndex).
static RTXDI_PackedGIReservoir* g_CurrentReservoirBuffer;
// Separate physical buffer the final per-pixel result is written to; becomes next frame's history.
static RTXDI_PackedGIReservoir* g_OutputReservoirBuffer;
static float2* g_NeighborOffsetsBuffer;
static PushConstantReSTIRGISpatial g_PushConstant;
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
    // Matches RTXPT's RAB_ValidateGISampleWithJacobian exactly. The SDK uses the jacobian directly
    // as a multiplier on the RIS selection weight (targetPdf * jacobian) without re-incorporating it
    // into the final normalization -- an unclamped jacobian leaks straight through as energy gain
    // whenever a neighbor is reused across surfaces at different angles/distances. Clamping the
    // jacobian itself (not just gating on it) bounds that leak to at most 3x.
    if (jacobian > 10.0 || jacobian < 1.0 / 10.0)
    {
        return false;
    }
    jacobian = clamp(jacobian, 1.0 / 3.0, 3.0);
    return true;
}

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

    if (g_UBO == nullptr)
    {
        return s;
    }

    if (g_PushConstant.depthTextureIndex == 0xFFFFFFFF)
    {
        return s;
    }

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(g_UBO->imageHeapIndexOffset + g_PushConstant.depthTextureIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelPos, 0)).r;

    if (rawDepth <= 0.00001)
    {
        return s;
    }

    if (g_PushConstant.gbufferNormalIndex == 0xFFFFFFFF)
    {
        return s;
    }

    // m_gbufferNormal stores raw world normals directly in [-1, 1]
    Texture2D normalTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + g_PushConstant.gbufferNormalIndex, 0));
    float4 normalSample = normalTex.Load(int3(pixelPos, 0));
    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6)
    {
        return s;
    }
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

#include "Rtxdi/GI/SpatialResampling.hlsli"

struct ReSTIRGISpatialPayload
{
    uint dummy;
};
groupshared ReSTIRGISpatialPayload gSpatialPayload;

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
    uniform PushConstantReSTIRGISpatial pushConstant
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
    float4 diffuseIrradiance : SV_Target0;
};

void ApplyGIBoilingFilter(inout RTXDI_GIReservoir reservoir, float filterStrength)
{
    float reservoirWeight = dot(reservoir.radiance, float3(0.2126, 0.7152, 0.0722)) * reservoir.weightSum;
    if (reservoirWeight <= 0.0) return;

    float waveWeight = WaveActiveSum(reservoirWeight);
    uint waveCount = WaveActiveCountBits(reservoirWeight > 0.0);
    float averageNonzeroWeight = (waveCount > 0) ? (waveWeight / float(waveCount)) : 0.0;

    float maxAllowedWeight = max(averageNonzeroWeight * (30.0 / clamp(filterStrength, 0.01, 1.0)), 100.0);
    if (averageNonzeroWeight > 0.0 && reservoirWeight > maxAllowedWeight)
    {
        // Clamp excessive outlier weights gracefully to suppress boiling fireflies without creating black holes
        reservoir.weightSum *= (maxAllowedWeight / reservoirWeight);
    }
}

[shader("fragment")]
PSOutput fragMain(
    MSOutput input,
    uniform PushConstantReSTIRGISpatial pushConstant
)
{
    PSOutput outVal;
    outVal.diffuseIrradiance = float4(0.0);

    uint2 pixelCoord = uint2(input.pos.xy);
    if (pixelCoord.x >= uint(pushConstant.viewportSize.x) || pixelCoord.y >= uint(pushConstant.viewportSize.y))
    {
        return outVal;
    }

    if (pushConstant.matrixReference == 0 ||
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
    g_CurrentReservoirBuffer = (RTXDI_PackedGIReservoir*)pushConstant.inputReservoirReference;
    g_OutputReservoirBuffer = (RTXDI_PackedGIReservoir*)pushConstant.outputReservoirReference;
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
        g_OutputReservoirBuffer[pointer] = (RTXDI_PackedGIReservoir)0;
        return outVal;
    }

    // This frame's temporally-resampled reservoir for this pixel (written by ReSTIRGITemporal.slang)
    RTXDI_GIReservoir inputReservoir = RTXDI_UnpackGIReservoir(g_CurrentReservoirBuffer[pointer]);

    RTXDI_GISpatialResamplingParameters sparams;
    sparams.depthThreshold = pushConstant.depthThreshold;
    sparams.normalThreshold = pushConstant.normalThreshold;
    sparams.numSamples = pushConstant.numSamples;
    sparams.samplingRadius = pushConstant.samplingRadius;
    sparams.biasCorrectionMode = RTXDI_BIAS_CORRECTION_BASIC;
    sparams.pad1 = 0;
    sparams.pad2 = 0;
    sparams.pad3 = 0;

    RTXDI_RuntimeParameters rParams;
    rParams.neighborOffsetMask = pushConstant.neighborOffsetMask;
    rParams.activeCheckerboardField = 0;
    rParams.frameIndex = pushConstant.frameIndex;
    rParams.pad2 = 0;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixelCoord, pushConstant.frameIndex, 1);

    RTXDI_GIReservoir resampledReservoir = RTXDI_GISpatialResampling(
        pixelCoord,
        surface,
        0, // sourceBufferIndex
        inputReservoir,
        rng,
        rParams,
        resParams,
        sparams
    );

    if (pushConstant.enableBoilingFilter != 0)
    {
        ApplyGIBoilingFilter(resampledReservoir, pushConstant.boilingFilterStrength);
    }

    // Final result is written to the SEPARATE output buffer, which becomes next frame's history --
    // never write back into g_CurrentReservoirBuffer here, it is still being read as neighbor input
    // by other in-flight pixel invocations of this same pass.
    g_OutputReservoirBuffer[pointer] = RTXDI_PackGIReservoir(resampledReservoir, 0);

    // Output finalized diffuse irradiance matching NVIDIA RTXDI FinalShading.hlsl
    float3 diffuse = float3(0.0);
    float hitDist = 0.0;
    if (RTXDI_IsValidGIReservoir(resampledReservoir))
    {
        float3 toSample = resampledReservoir.position - surface.worldPos;
        float distSq = dot(toSample, toSample);
        float3 L = distSq > 1e-6 ? toSample * rsqrt(distSq) : surface.normal;
        float NdotL = saturate(dot(surface.normal, L));
        hitDist = sqrt(distSq);

        // Final visibility check (matches RTXPT's GIFinalShading.hlsl / enableFinalVisibility,
        // which defaults to true): a reused reservoir's sample position was validated for surface
        // similarity, never for line-of-sight from THIS pixel. Right after camera motion reveals new
        // geometry (disocclusion), spatial/temporal reuse routinely hands a pixel a sample position
        // that is now behind something from its point of view -- shading it anyway injects light
        // that shouldn't be there, which is exactly the "everything lights up while the camera moves"
        // artifact. A stale reservoir that has survived long enough to be static-camera-stable has
        // already had many frames to naturally re-validate; it's specifically the freshly-reused
        // samples during motion that need this gate.
        bool visible = true;
        if (g_UBO->tlasDeviceAddress != 0)
        {
            RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(g_UBO->tlasDeviceAddress);
            RayDesc visRay;
            visRay.Origin = surface.worldPos + surface.normal * 0.02;
            visRay.Direction = L;
            visRay.TMin = 0.005;
            visRay.TMax = max(0.01, hitDist - 0.02);

            RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> visQuery;
            visQuery.TraceRayInline(accelerationStructure,
                RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE,
                0xFF, visRay);
            visQuery.Proceed();
            visible = visQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
        }

        if (visible)
        {
            // In RTXDI, resampledReservoir.weightSum represents 1.0 / targetPdf. Multiplying by
            // (NdotL / PI) cancels the geometric factor and yields physical diffuse radiance.
            float demodulatedDiffuse = NdotL / 3.14159265359;
            float weightFactor = clamp(demodulatedDiffuse * resampledReservoir.weightSum, 0.0, 50.0);
            if (isnan(weightFactor) || isinf(weightFactor)) weightFactor = 1.0;
            diffuse = resampledReservoir.radiance * weightFactor;
        }
    }

    // Alpha carries the hit distance for NRD's REBLUR_DIFFUSE/RELAX_DIFFUSE denoiser.
    outVal.diffuseIrradiance = float4(diffuse, hitDist);
    return outVal;
}
