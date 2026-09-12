#include "shaderIO.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"

#ifndef RTXDI_RESERVOIR_BLOCK_SIZE
#define RTXDI_RESERVOIR_BLOCK_SIZE 16
#endif

#define RTXDI_ENABLE_STORE_RESERVOIR 0
#define RTXDI_LIGHT_RESERVOIR_BUFFER g_ReservoirBuffer

static RTXDI_PackedDIReservoir* g_ReservoirBuffer; // buffer B, read only
static PushConstantReSTIRDIFinalShading g_PushConstant;
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

RAB_LightInfo RAB_LoadLightInfo(uint index, bool prevFrame)
{
    return g_LightData[index];
}

RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo light, float3 worldPos, float2 uv)
{
    RAB_LightSample s;
    s.position = float3(0.0);
    s.normal = float3(0.0, 1.0, 0.0);
    s.radiance = float3(0.0);
    s.solidAnglePdf = 1.0;
    uint lightType = (uint)light.position.w;
    s.lightType = lightType;

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

    float3 lightVec = light.position.xyz - worldPos;
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

#include "Rtxdi/Utils/Math.hlsli"

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

#include "Rtxdi/DI/ReservoirStorage.hlsli"

// Same glTF-style PBR terms DeferredLighting.slang's brute-force light loop already uses, copied here
// so ReSTIR-DI-shaded pixels look identical to non-resampled ones -- only the light *selection* differs.
struct PBRInfo
{
    float NdotL;
    float NdotV;
    float NdotH;
    float LdotH;
    float VdotH;
    float perceptualRoughness;
    float metalness;
    float3 reflectance0;
    float3 reflectance90;
    float alphaRoughness;
    float3 diffuseColor;
    float3 specularColor;
};

static const float M_PI = 3.141592653589793;
static const float c_MinRoughness = 0.04;

float3 pbrDiffuse(PBRInfo p) { return p.diffuseColor / M_PI; }

float3 pbrSpecularReflection(PBRInfo p)
{
    return p.reflectance0 + (p.reflectance90 - p.reflectance0) * pow(clamp(1.0 - p.VdotH, 0.0, 1.0), 5.0);
}

float pbrGeometricOcclusion(PBRInfo p)
{
    float r = p.alphaRoughness;
    float attenuationL = 2.0 * p.NdotL / (p.NdotL + sqrt(r * r + (1.0 - r * r) * (p.NdotL * p.NdotL)));
    float attenuationV = 2.0 * p.NdotV / (p.NdotV + sqrt(r * r + (1.0 - r * r) * (p.NdotV * p.NdotV)));
    return attenuationL * attenuationV;
}

float pbrMicrofacetDistribution(PBRInfo p)
{
    float roughnessSq = p.alphaRoughness * p.alphaRoughness;
    float f = (p.NdotH * roughnessSq - p.NdotH) * p.NdotH + 1.0;
    return roughnessSq / (M_PI * f * f);
}

struct ReSTIRDIFinalShadingPayload
{
    uint dummy;
};
groupshared ReSTIRDIFinalShadingPayload gFinalShadingPayload;

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
    uniform PushConstantReSTIRDIFinalShading pushConstant
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

[shader("fragment")]
PSOutput fragMain(
    MSOutput input,
    uniform PushConstantReSTIRDIFinalShading pushConstant
)
{
    PSOutput outVal;
    outVal.directLighting = float4(0.0);

    uint2 pixelCoord = uint2(input.pos.xy);
    if (pixelCoord.x >= uint(pushConstant.viewportSize.x) || pixelCoord.y >= uint(pushConstant.viewportSize.y))
    {
        return outVal;
    }

    if (pushConstant.matrixReference == 0 ||
        pushConstant.lightDataReference == 0 ||
        pushConstant.reservoirReference == 0 ||
        pushConstant.depthTextureIndex == 0xFFFFFFFF ||
        pushConstant.gbufferNormalIndex == 0xFFFFFFFF ||
        pushConstant.gbufferAlbedoIndex == 0xFFFFFFFF ||
        pushConstant.gbufferMaterialIndex == 0xFFFFFFFF)
    {
        return outVal;
    }

    g_PushConstant = pushConstant;
    g_UBO = (UniformBufferObject*)pushConstant.matrixReference;
    g_LightData = (LightData*)pushConstant.lightDataReference;
    g_ReservoirBuffer = (RTXDI_PackedDIReservoir*)pushConstant.reservoirReference;

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(g_UBO->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (rawDepth <= 0.00001) return outVal;

    Texture2D normalTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
    float4 normalSample = normalTex.Load(int3(pixelCoord, 0));
    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6) return outVal;
    float3 n = normalSample.rgb * rsqrt(lenSqN);

    float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
    float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 ndc = float4(ndc_xy, rawDepth, 1.0);
    float4 worldPosH = mul(pushConstant.invViewProj, ndc);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    RTXDI_ReservoirBufferParameters resParams;
    resParams.reservoirBlockRowPitch = pushConstant.reservoirBlockRowPitch;
    resParams.reservoirArrayPitch = pushConstant.reservoirArrayPitch;
    resParams.pad1 = 0;
    resParams.pad2 = 0;

    RTXDI_DIReservoir reservoir = RTXDI_LoadDIReservoir(resParams, pixelCoord, 0);
    if (!RTXDI_IsValidDIReservoir(reservoir))
    {
        return outVal;
    }

    RAB_LightInfo light = RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(reservoir), false);
    RAB_LightSample lightSample = RAB_SamplePolymorphicLight(light, worldPos, RTXDI_GetDIReservoirSampleUV(reservoir));

    float3 dir;
    float dist;
    if (lightSample.lightType == 0)
    {
        dir = lightSample.normal;
        dist = 1e4;
    }
    else
    {
        float3 toLight = lightSample.position - worldPos;
        dist = length(toLight);
        dir = dist > 0.0001 ? (toLight / dist) : float3(0.0, 1.0, 0.0);
    }

    float NdotL_raw = dot(n, dir);
    if (NdotL_raw <= 0.0) return outVal;

    // Shadow ray: exactly one per pixel regardless of how many lights exist in the scene, unlike the
    // brute-force loop in DeferredLighting.slang which only ever shadows light index 0.
    bool visible = true;
    if (g_UBO->tlasDeviceAddress != 0)
    {
        RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(g_UBO->tlasDeviceAddress);
        RayDesc shadowRay;
        shadowRay.Origin = worldPos + n * 0.02;
        shadowRay.Direction = dir;
        shadowRay.TMin = 0.005;
        shadowRay.TMax = max(0.01, dist - 0.02);

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> shadowQuery;
        shadowQuery.TraceRayInline(accelerationStructure,
            RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE,
            0xFF, shadowRay);
        shadowQuery.Proceed();
        visible = shadowQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
    }

    if (!visible)
    {
        return outVal;
    }

    Texture2D albedoTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + pushConstant.gbufferAlbedoIndex, 0));
    Texture2D materialTex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + pushConstant.gbufferMaterialIndex, 0));
    float4 albedoSample = albedoTex.Load(int3(pixelCoord, 0));
    float4 materialSample = materialTex.Load(int3(pixelCoord, 0));

    if (materialSample.b >= 2.0) // Unlit workflow: no direct lighting contribution
    {
        return outVal;
    }

    float3 baseColor = albedoSample.rgb;
    float perceptualRoughness = clamp(materialSample.r, c_MinRoughness, 1.0);
    float metallic = clamp(materialSample.g, 0.0, 1.0);
    float alphaRoughness = perceptualRoughness * perceptualRoughness;

    float3 f0 = float3(0.04);
    float3 diffuseColor = baseColor * (float3(1.0) - f0) * (1.0 - metallic);
    float3 specularColor = lerp(f0, baseColor, metallic);
    float reflectance = max(max(specularColor.r, specularColor.g), specularColor.b);
    float3 specularEnvironmentR0 = specularColor;
    float3 specularEnvironmentR90 = clamp(float3(reflectance * 50.0), 0.0, 1.0);

    float3 v = normalize(g_UBO->cameraWorldPos.xyz - worldPos);
    float NdotV = clamp(dot(n, v), 0.001, 1.0);
    float3 h = normalize(dir + v);

    float NdotL = clamp(NdotL_raw, 0.001, 1.0);
    float NdotH = clamp(dot(n, h), 0.0, 1.0);
    float LdotH = clamp(dot(dir, h), 0.0, 1.0);
    float VdotH = clamp(dot(v, h), 0.0, 1.0);

    PBRInfo pbrInputs;
    pbrInputs.NdotL = NdotL;
    pbrInputs.NdotV = NdotV;
    pbrInputs.NdotH = NdotH;
    pbrInputs.LdotH = LdotH;
    pbrInputs.VdotH = VdotH;
    pbrInputs.perceptualRoughness = perceptualRoughness;
    pbrInputs.metalness = metallic;
    pbrInputs.reflectance0 = specularEnvironmentR0;
    pbrInputs.reflectance90 = specularEnvironmentR90;
    pbrInputs.alphaRoughness = alphaRoughness;
    pbrInputs.diffuseColor = diffuseColor;
    pbrInputs.specularColor = specularColor;

    float3 F = pbrSpecularReflection(pbrInputs);
    float G = pbrGeometricOcclusion(pbrInputs);
    float D = pbrMicrofacetDistribution(pbrInputs);

    float3 diffuseContrib = (1.0 - F) * pbrDiffuse(pbrInputs);
    float3 specContrib = F * G * D / (4.0 * NdotL * NdotV);

    // reservoir.weightSum (post-FinalizeResampling) is the RIS inverse-PDF: multiplying the selected
    // sample's already-attenuated radiance by it reconstructs the unbiased resampled estimate.
    float3 lightRadiance = lightSample.radiance * RTXDI_GetDIReservoirInvPdf(reservoir);
    float3 result = NdotL * lightRadiance * (diffuseContrib + specContrib);

    if (any(isnan(result)) || any(isinf(result)))
    {
        result = float3(0.0);
    }

    outVal.directLighting = float4(result, 1.0);
    return outVal;
}
