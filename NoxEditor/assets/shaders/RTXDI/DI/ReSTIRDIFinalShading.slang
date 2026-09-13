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

void build_orthonormal_basis(float3 n, out float3 b1, out float3 b2);

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
        if (light.spotParams.z > 1e-4)
        {
            float3 t, b;
            build_orthonormal_basis(dir, t, b);
            float r = sqrt(saturate(uv.x));
            float phi = 2.0 * 3.14159265359 * frac(uv.y);
            dir = normalize(dir + (r * cos(phi) * t + r * sin(phi) * b) * tan(light.spotParams.z));
        }
        s.normal = dir;
        s.radiance = light.color.rgb * light.color.a;
        return s;
    }

    float3 centerVec = light.position.xyz - worldPos;
    float centerDistance = length(centerVec);
    float3 sampledPosition = light.position.xyz;
    if (light.spotParams.z > 1e-4)
    {
        float3 centerDir = centerDistance > 1e-4 ? centerVec / centerDistance : float3(0.0, 1.0, 0.0);
        float3 t, b;
        build_orthonormal_basis(centerDir, t, b);
        float r = sqrt(saturate(uv.x));
        float phi = 2.0 * 3.14159265359 * frac(uv.y);
        sampledPosition += (r * cos(phi) * t + r * sin(phi) * b) * light.spotParams.z;
    }

    float3 lightVec = sampledPosition - worldPos;
    float distance = length(lightVec);
    s.position = sampledPosition;
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
static const float GOLDEN_ANGLE = 2.399963229728653;
static const float c_MinRoughness = 0.04;

// NRD REBLUR YCoCg conversion (matching NRD.hlsli:398, same fix already applied for ReSTIR GI in
// ReSTIRGISpatial.slang) -- REBLUR's internal filtering assumes IN_DIFF_RADIANCE_HITDIST is already
// YCoCg-encoded; RELAX takes plain linear RGB directly, no conversion needed.
float3 LinearToYCoCg(float3 color)
{
    float Y = dot(color, float3(0.25, 0.5, 0.25));
    float Co = dot(color, float3(0.5, 0.0, -0.5));
    float Cg = dot(color, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

uint pcg_hash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float2 random2(uint2 pixelCoord, uint frameIndex, uint salt)
{
    uint seed = pixelCoord.x + pixelCoord.y * 4099u + frameIndex * 131071u + salt * 8191u;
    uint h1 = pcg_hash(seed);
    uint h2 = pcg_hash(h1);
    return float2(float(h1), float(h2)) / 4294967295.0;
}

float2 sample_vogel_disk(uint sampleIndex, uint sampleCount, float phiOffset, float rOffset)
{
    float r = sqrt((float(sampleIndex) + rOffset) / float(sampleCount));
    float theta = float(sampleIndex) * GOLDEN_ANGLE + phiOffset * (2.0 * M_PI);
    return float2(r * cos(theta), r * sin(theta));
}

void build_orthonormal_basis(float3 n, out float3 b1, out float3 b2)
{
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float b = n.x * n.y * a;
    b1 = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    b2 = float3(b, sign + n.y * n.y * a, -n.y);
}

float2 intersection_uv(InstanceLUT lut, uint primIndex, float2 barycentrics)
{
    if (lut.vertexBufferAddress == 0)
    {
        return float2(0.0);
    }

    uint32_t* indices = (uint32_t*)lut.indexBufferAddress;
    Vertex* vertices = (Vertex*)lut.vertexBufferAddress;

    uint i0, i1, i2;
    if (lut.indexBufferAddress != 0)
    {
        i0 = indices[primIndex * 3 + 0];
        i1 = indices[primIndex * 3 + 1];
        i2 = indices[primIndex * 3 + 2];
    }
    else
    {
        i0 = primIndex * 3 + 0;
        i1 = primIndex * 3 + 1;
        i2 = primIndex * 3 + 2;
    }

    float w0 = 1.0 - barycentrics.x - barycentrics.y;
    float w1 = barycentrics.x;
    float w2 = barycentrics.y;
    return w0 * vertices[i0].uv0 + w1 * vertices[i1].uv0 + w2 * vertices[i2].uv0;
}

bool trace_visibility(float3 origin, float3 direction, float maxDist)
{
    if (g_UBO->tlasDeviceAddress == 0)
    {
        return true;
    }

    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(g_UBO->tlasDeviceAddress);
    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.005;
    shadowRay.TMax = max(0.01, maxDist);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadowQuery;
    shadowQuery.TraceRayInline(accelerationStructure,
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xFF, shadowRay);

    if (g_UBO->instanceLUTReference == 0)
    {
        shadowQuery.Proceed();
        return shadowQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
    }

    InstanceLUT* instanceLUTBuffer = (InstanceLUT*)g_UBO->instanceLUTReference;
    SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

    while (shadowQuery.Proceed())
    {
        uint instanceID = shadowQuery.CandidateRayInstanceCustomIndex();
        uint primIndex = shadowQuery.CandidatePrimitiveIndex();
        InstanceLUT lut = instanceLUTBuffer[instanceID];

        if (lut.alphaMode == 0 || lut.baseColorTextureIndex == 0xFFFFFFFF)
        {
            shadowQuery.CommitNonOpaqueTriangleHit();
        }
        else
        {
            float2 uv = intersection_uv(lut, primIndex, shadowQuery.CandidateTriangleBarycentrics());
            Texture2D tex = Texture2D.Handle(uint2(g_UBO->imageHeapIndexOffset + lut.baseColorTextureIndex, 0));
            if (tex.SampleLevel(textureSampler, uv, 0).a >= lut.alphaCutoff)
            {
                shadowQuery.CommitNonOpaqueTriangleHit();
            }
        }
    }

    return shadowQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

float evaluate_soft_visibility(LightData light, float3 worldPos, float3 normal, float3 centerDir, float centerDist, uint2 pixelCoord, uint frameIndex)
{
    uint lightType = (uint)light.position.w;
    float sourceRadius = light.spotParams.z;
    uint sampleCount = min(max(1u, (uint)light.spotParams.w), 16u);
    float3 rayOrigin = worldPos + normal * 0.02;

    if (sourceRadius <= 1e-4 || sampleCount <= 1u)
    {
        return trace_visibility(rayOrigin, centerDir, centerDist - 0.02) ? 1.0 : 0.0;
    }

    float2 rnd = random2(pixelCoord, frameIndex, asuint(light.color.a) ^ (uint)light.position.w);
    float visibleCount = 0.0;

    if (lightType == 0)
    {
        float3 T, B;
        build_orthonormal_basis(centerDir, T, B);
        float tanRadius = tan(sourceRadius);
        for (uint i = 0u; i < sampleCount; i++)
        {
            float2 disk = sample_vogel_disk(i, sampleCount, rnd.x, rnd.y);
            float3 dir = normalize(centerDir + (disk.x * T + disk.y * B) * tanRadius);
            visibleCount += trace_visibility(rayOrigin, dir, 1e4) ? 1.0 : 0.0;
        }
    }
    else
    {
        float3 T, B;
        build_orthonormal_basis(centerDir, T, B);
        for (uint i = 0u; i < sampleCount; i++)
        {
            float2 disk = sample_vogel_disk(i, sampleCount, rnd.x, rnd.y);
            float3 samplePos = light.position.xyz + (disk.x * T + disk.y * B) * sourceRadius;
            float3 toSample = samplePos - rayOrigin;
            float sampleDist = length(toSample);
            float3 dir = sampleDist > 1e-4 ? toSample / sampleDist : centerDir;
            visibleCount += trace_visibility(rayOrigin, dir, sampleDist - 0.02) ? 1.0 : 0.0;
        }
    }

    return visibleCount / float(sampleCount);
}

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

    float visibility = evaluate_soft_visibility(light, worldPos, n, dir, dist, pixelCoord, pushConstant.frameIndex);
    if (visibility <= 0.0)
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
    float3 result = visibility * NdotL * lightRadiance * (diffuseContrib + specContrib);

    if (any(isnan(result)) || any(isinf(result)))
    {
        result = float3(0.0);
    }

    if (pushConstant.denoiserMode == 1) // NRD REBLUR: encode into YCoCg before writing
    {
        result = LinearToYCoCg(result);
    }

    // Alpha carries the shadow ray's traveled distance (to the light, or 1e4 for directional lights)
    // -- NRD's hit-distance-based blur radius; matches the same raw/unnormalized convention already
    // used for ReSTIR GI's diffuseIrradiance.a in ReSTIRGISpatial.slang.
    outVal.directLighting = float4(result, dist);
    return outVal;
}
