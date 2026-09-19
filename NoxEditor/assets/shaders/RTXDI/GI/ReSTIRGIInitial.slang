#include "shaderIO.h"

#define RTXDI_ENABLE_STORE_RESERVOIR 0
#define RTXDI_GI_RESERVOIR_BUFFER g_DummyReservoirBuffer
RWStructuredBuffer<RTXDI_PackedGIReservoir> g_DummyReservoirBuffer;
#include "Rtxdi/GI/Reservoir.hlsli"
#include "Rtxdi/Utils/Math.hlsli"
#include "RTXDI/RTXDILightSampling.slang"

static const float M_PI = 3.141592653589793;
static const float GOLDEN_ANGLE = 2.399963229728653;

struct ReSTIRGIPayload
{
    uint dummy;
};
groupshared ReSTIRGIPayload gPayload;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain()
{
    DispatchMesh(1, 1, 1, gPayload);
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
    uniform PushConstantReSTIRGIInitial pushConstant
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

// Orthonormal basis construction (Duff et al. 2017)
void build_orthonormal_basis(float3 n, out float3 b1, out float3 b2)
{
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float b = n.x * n.y * a;
    b1 = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    b2 = float3(b, sign + n.y * n.y * a, -n.y);
}

// PCG Random Number Generator
uint pcg_hash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float2 pcg2d(uint2 p, uint frame)
{
    uint seed = p.x + p.y * 1920u + frame * 1920u * 1080u;
    uint h1 = pcg_hash(seed);
    uint h2 = pcg_hash(h1);
    return float2(float(h1) / 4294967295.0, float(h2) / 4294967295.0);
}

// Cosine-weighted hemisphere sampling
float3 sample_cosine_hemisphere(float3 n, float2 u)
{
    float r = sqrt(u.x);
    float phi = 2.0 * 3.14159265359 * u.y;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u.x));

    float3 b1, b2;
    build_orthonormal_basis(n, b1, b2);
    return normalize(x * b1 + y * b2 + z * n);
}

float2 sample_vogel_disk(uint sampleIndex, uint sampleCount, float phiOffset, float rOffset)
{
    float r = sqrt((float(sampleIndex) + rOffset) / float(sampleCount));
    float theta = float(sampleIndex) * GOLDEN_ANGLE + phiOffset * (2.0 * M_PI);
    return float2(r * cos(theta), r * sin(theta));
}

// Barycentric UV Interpolation for Alpha Testing
float2 intersection_uv(GpuRayTracingInstance lut, uint primIndex, float2 barycentrics)
{
    if (lut.vertexBufferAddress == 0) return float2(0.0);
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

bool trace_visibility_alpha(
    UniformBufferObject* ubo,
    GpuRayTracingInstance* instanceLUTBuffer,
    SamplerState textureSampler,
    float3 origin,
    float3 direction,
    float maxDist)
{
    if (ubo->tlasDeviceAddress == 0)
    {
        return true;
    }

    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.005;
    shadowRay.TMax = max(0.01, maxDist);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadowQuery;
    shadowQuery.TraceRayInline(accelerationStructure,
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xFF, shadowRay);

    while (shadowQuery.Proceed())
    {
        uint sInst = shadowQuery.CandidateRayInstanceCustomIndex();
        uint sPrim = shadowQuery.CandidatePrimitiveIndex();
        GpuRayTracingInstance sLut = instanceLUTBuffer[sInst];
        if (sLut.alphaMode == 0 || sLut.baseColorTextureIndex == 0xFFFFFFFF)
        {
            shadowQuery.CommitNonOpaqueTriangleHit();
        }
        else
        {
            float2 sUV = intersection_uv(sLut, sPrim, shadowQuery.CandidateTriangleBarycentrics());
            Texture2D sTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + sLut.baseColorTextureIndex, 0));
            if (sTex.SampleLevel(textureSampler, sUV, 0).a >= sLut.alphaCutoff)
            {
                shadowQuery.CommitNonOpaqueTriangleHit();
            }
        }
    }

    return shadowQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// Sample analytic lighting through RTXDI instead of walking every scene light at every GI vertex.
// The selected sample's reservoir inverse PDF reconstructs the complete-light estimate.
float3 sample_direct_light(
    UniformBufferObject* ubo,
    GpuRayTracingInstance* instanceLUTBuffer,
    SamplerState textureSampler,
    RTXDILightSamplingParams lightSampling,
    float3 worldPos,
    float3 normal,
    bool doubleSided,
    uint2 pixelCoord,
    uint frameIndex,
    uint sampleIndex)
{
    if (lightSampling.lightDataReference == 0 ||
        (lightSampling.numLocalLights == 0 && lightSampling.numInfiniteLights == 0))
    {
        return float3(0.0);
    }

    RAB_Surface surface;
    surface.worldPos = worldPos;
    surface.normal = normal;
    surface.linearDepth = 0.0;
    surface.valid = true;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixelCoord, frameIndex, sampleIndex * 2u);
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(pixelCoord / 8u, frameIndex, sampleIndex * 2u + 1u);
    RAB_LightSample lightSample;
    RTXDI_DIReservoir reservoir = RTXDI_SampleLightsAt(surface, rng, coherentRng, lightSampling, lightSample);
    if (!RTXDI_IsValidDIReservoir(reservoir) || dot(lightSample.radiance, lightSample.radiance) <= 1e-8)
    {
        return float3(0.0);
    }

    float3 lightDir;
    float lightDist;
    if (lightSample.lightType == 0)
    {
        lightDir = normalize(lightSample.normal);
        lightDist = 1e4;
    }
    else
    {
        float3 toLight = lightSample.position - worldPos;
        lightDist = length(toLight);
        lightDir = lightDist > 1e-4 ? toLight / lightDist : float3(0.0, 1.0, 0.0);
    }

    float signedNdotL = dot(normal, lightDir);
    float NdotL = doubleSided ? abs(signedNdotL) : max(signedNdotL, 0.0);
    if (NdotL <= 0.0)
    {
        return float3(0.0);
    }

    float3 shadowNormal = doubleSided && signedNdotL < 0.0 ? -normal : normal;
    bool visible = trace_visibility_alpha(
        ubo, instanceLUTBuffer, textureSampler,
        worldPos + shadowNormal * 0.02, lightDir, lightDist - 0.02);
    if (!visible)
    {
        return float3(0.0);
    }

    float terminatorFade = saturate(NdotL * 10.0);
    return lightSample.radiance * (NdotL * terminatorFade * RTXDI_GetDIReservoirInvPdf(reservoir));
}

float evaluate_soft_visibility_alpha(
    UniformBufferObject* ubo,
    GpuRayTracingInstance* instanceLUTBuffer,
    SamplerState textureSampler,
    LightData light,
    float3 worldPos,
    float3 normal,
    float3 centerDir,
    float centerDist,
    uint2 pixelCoord,
    uint frameIndex)
{
    uint lightType = (uint)light.position.w;
    float sourceRadius = light.spotParams.z;
    uint sampleCount = min(max(1u, (uint)light.spotParams.w), 16u);
    float3 rayOrigin = worldPos + normal * 0.02;

    if (sourceRadius <= 1e-4 || sampleCount <= 1u)
    {
        return trace_visibility_alpha(ubo, instanceLUTBuffer, textureSampler, rayOrigin, centerDir, centerDist - 0.02) ? 1.0 : 0.0;
    }

    float2 rnd = pcg2d(pixelCoord + uint2(asuint(light.color.a) & 1023u, (uint)light.position.w * 97u), frameIndex);
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
            visibleCount += trace_visibility_alpha(ubo, instanceLUTBuffer, textureSampler, rayOrigin, dir, 1e4) ? 1.0 : 0.0;
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
            visibleCount += trace_visibility_alpha(ubo, instanceLUTBuffer, textureSampler, rayOrigin, dir, sampleDist - 0.02) ? 1.0 : 0.0;
        }
    }

    return visibleCount / float(sampleCount);
}

struct PSOutput
{
    float4 initialDiffuse : SV_Target0;
};

[shader("fragment")]
PSOutput fragMain(
    MSOutput input,
    uniform PushConstantReSTIRGIInitial pushConstant
)
{
    PSOutput outVal;
    outVal.initialDiffuse = float4(0.0);

    uint2 pixelCoord = uint2(input.pos.xy);
    if (pixelCoord.x >= uint(pushConstant.viewportSize.x) || pixelCoord.y >= uint(pushConstant.viewportSize.y))
    {
        return outVal;
    }

    if (pushConstant.reservoirBufferReference == 0 || pushConstant.matrixReference == 0 ||
        pushConstant.depthTextureIndex == 0xFFFFFFFF || pushConstant.gbufferNormalIndex == 0xFFFFFFFF)
    {
        return outVal;
    }

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    RTXDI_ReservoirBufferParameters resParams;
    resParams.reservoirBlockRowPitch = pushConstant.reservoirBlockRowPitch;
    resParams.reservoirArrayPitch = pushConstant.reservoirArrayPitch;

    uint pointer = RTXDI_ReservoirPositionToPointer(resParams, pixelCoord, 0);
    RTXDI_PackedGIReservoir* reservoirBuffer = (RTXDI_PackedGIReservoir*)pushConstant.reservoirBufferReference;

    if (ubo->tlasDeviceAddress == 0 || ubo->sceneRayTracingInstancesReference == 0)
    {
        reservoirBuffer[pointer] = (RTXDI_PackedGIReservoir)0;
        return outVal;
    }

    // 1. Read Depth Buffer (Reverse-Z: 1.0 = near, 0.0 = infinity)
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelCoord, 0)).r;

    if (rawDepth <= 0.00001)
    {
        // Sky / void: Store empty reservoir
        reservoirBuffer[pointer] = (RTXDI_PackedGIReservoir)0;
        return outVal;
    }

    // 2. Read Primary Normal Buffer (m_gbufferNormal stores raw world normals directly in [-1, 1])
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
    float4 normalSample = normalTex.Load(int3(pixelCoord, 0));
    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6)
    {
        reservoirBuffer[pointer] = (RTXDI_PackedGIReservoir)0;
        return outVal;
    }
    float3 normal = normalSample.rgb * rsqrt(lenSqN);

    // 3. Reconstruct Primary World Position
    float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
    float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 ndc = float4(ndc_xy, rawDepth, 1.0);
    float4 worldPosH = mul(pushConstant.invViewProj, ndc);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    // 4. Generate Cosine-Weighted Hemisphere Ray
    float2 rngSample = pcg2d(pixelCoord, pushConstant.frameIndex);
    float3 rayDir = sample_cosine_hemisphere(normal, rngSample);
    float NdotL = max(dot(normal, rayDir), 0.0);
    // This must remain the actual solid-angle PDF of the cosine sample. A large floor biases
    // grazing samples dark and makes the reservoir systematically lose indirect energy.
    float rayPdf = max(NdotL / 3.14159265359, 1e-6);

    // 5. Trace Indirect Diffuse Ray against TLAS
    float3 rayOrigin = worldPos + normal * 0.01;
    RayDesc diffuseRay;
    diffuseRay.Origin = rayOrigin;
    diffuseRay.Direction = rayDir;
    diffuseRay.TMin = 0.005;
    diffuseRay.TMax = 1e4;

    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    let rayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

    rq.TraceRayInline(accelerationStructure, rayFlags, 0xFF, diffuseRay);

    GpuRayTracingInstance* instanceLUTBuffer = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

    while (rq.Proceed())
    {
        uint instanceID = rq.CandidateRayInstanceCustomIndex();
        uint primIndex = rq.CandidatePrimitiveIndex();

        GpuRayTracingInstance lut = instanceLUTBuffer[instanceID];

        if (lut.alphaMode == 0 || lut.baseColorTextureIndex == 0xFFFFFFFF)
        {
            rq.CommitNonOpaqueTriangleHit();
        }
        else
        {
            float2 hitUV = intersection_uv(lut, primIndex, rq.CandidateTriangleBarycentrics());
            Texture2D tex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.baseColorTextureIndex, 0));
            float4 intersectionColor = tex.SampleLevel(textureSampler, hitUV, 0);

            if (intersectionColor.a >= lut.alphaCutoff)
            {
                rq.CommitNonOpaqueTriangleHit();
            }
        }
    }

    bool hit = (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT);

    float3 hitPos;
    float3 hitNormal;
    float3 hitRadiance = float3(0.0);

    if (!hit)
    {
        // Miss: Sample Environment Cubemap Skybox
        if (ubo->prefilteredMapIndex != 0xFFFFFFFF)
        {
            TextureCube prefilteredTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
            SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
            hitRadiance = prefilteredTex.SampleLevel(samplerPrefiltered, rayDir, 0).rgb * ubo->scaleIBLAmbient;
        }
        else
        {
            hitRadiance = lerp(float3(0.08, 0.08, 0.12), float3(0.4, 0.6, 0.95), saturate(rayDir.y * 0.5 + 0.5)) * ubo->scaleIBLAmbient;
        }
        hitPos = worldPos + rayDir * 1000.0;
        hitNormal = -rayDir;
    }
    else
    {
        // Closest Hit Secondary Surface
        uint committedInstanceID = rq.CommittedRayInstanceCustomIndex();
        uint committedPrimIndex = rq.CommittedPrimitiveIndex();
        float2 committedBary = rq.CommittedTriangleBarycentrics();

        GpuRayTracingInstance hitLut = instanceLUTBuffer[committedInstanceID];
        if (hitLut.vertexBufferAddress == 0)
        {
            reservoirBuffer[pointer] = (RTXDI_PackedGIReservoir)0;
            return outVal;
        }

        uint32_t* indices = (uint32_t*)hitLut.indexBufferAddress;
        Vertex* vertices = (Vertex*)hitLut.vertexBufferAddress;

        uint i0, i1, i2;
        if (hitLut.indexBufferAddress != 0)
        {
            i0 = indices[committedPrimIndex * 3 + 0];
            i1 = indices[committedPrimIndex * 3 + 1];
            i2 = indices[committedPrimIndex * 3 + 2];
        }
        else
        {
            i0 = committedPrimIndex * 3 + 0;
            i1 = committedPrimIndex * 3 + 1;
            i2 = committedPrimIndex * 3 + 2;
        }

        float w0 = 1.0 - committedBary.x - committedBary.y;
        float w1 = committedBary.x;
        float w2 = committedBary.y;

        float2 hitUV = w0 * vertices[i0].uv0 + w1 * vertices[i1].uv0 + w2 * vertices[i2].uv0;
        float3 localNormal = normalize(w0 * vertices[i0].normal + w1 * vertices[i1].normal + w2 * vertices[i2].normal);
        hitNormal = normalize(mul((float3x3)hitLut.normalMatrix, localNormal));

        if (hitLut.doubleSided != 0 && dot(hitNormal, -rayDir) < 0.0)
        {
            hitNormal = -hitNormal;
        }

        float hitT = rq.CommittedRayT();
        hitPos = rayOrigin + rayDir * hitT;

        float3 albedo = hitLut.baseColorFactor.rgb;
        if (hitLut.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D hitTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + hitLut.baseColorTextureIndex, 0));
            albedo *= hitTex.SampleLevel(textureSampler, hitUV, 0).rgb;
        }

        float3 emissive = hitLut.emissiveFactor.rgb * hitLut.emissiveFactor.a;

        // Direct analytical lighting at the secondary hit, selected with the same RTXDI
        // importance distribution used by ReSTIR DI.
        float3 directLight = sample_direct_light(
            ubo, instanceLUTBuffer, textureSampler, pushConstant.lightSampling,
            hitPos, hitNormal, hitLut.doubleSided != 0,
            pixelCoord, pushConstant.frameIndex, 2u);

        // One-bounce real-time GI candidate: one diffuse ray plus one direct-light shadow ray.
        // Deeper paths belong to RTXPT; tracing four additional bounces per screen pixel made this
        // pass substantially more expensive than the ground-truth renderer it was approximating.
        hitRadiance = (albedo / 3.14159265359) * directLight + emissive;
    }

    // 6. Pack Initial Reservoir (M = 1, weightSum = 1.0 / rayPdf, age = 0)
    RTXDI_GIReservoir reservoir = RTXDI_MakeGIReservoir(hitPos, hitNormal, hitRadiance, rayPdf);
    reservoirBuffer[pointer] = RTXDI_PackGIReservoir(reservoir, 0);

    outVal.initialDiffuse = float4(hitRadiance, 1.0);
    return outVal;
}
