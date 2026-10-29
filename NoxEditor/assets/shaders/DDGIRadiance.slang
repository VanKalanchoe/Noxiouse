#include "shaderIO.h"
#include "DDGICommon.slang"
#include "LightSelection.slang"

struct DDGIRadiancePayload
{
    uint dummy;
};
groupshared DDGIRadiancePayload gPayload;

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
    uint threadID : SV_GroupIndex
)
{
    SetMeshOutputCounts(3, 1);

    if (threadID < 3)
    {
        triangles[0] = uint3(0, 1, 2);
        vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
        vertices[threadID].uv = uvs[threadID];
    }
}

float2 intersection_uv(GpuRayTracingInstance lut, uint primIndex, float2 barycentrics)
{
    uint32_t* indices = (uint32_t*)lut.indexBufferAddress;
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

    Vertex* vertices = (Vertex*)lut.vertexBufferAddress;
    float w0 = 1.0 - barycentrics.x - barycentrics.y;
    return w0 * vertices[i0].uv0 + barycentrics.x * vertices[i1].uv0 + barycentrics.y * vertices[i2].uv0;
}

// One shadow ray from a probe ray's hit towards a light (any hit ends it).
bool ddgiShadowed(RaytracingAccelerationStructure accelerationStructure, UniformBufferObject* ubo, float3 origin, float3 direction, float maxDistance)
{
    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.005 * ubo->unitsPerMeter;
    shadowRay.TMax = maxDistance;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> shadowRq;
    shadowRq.TraceRayInline(accelerationStructure, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, shadowRay);
    shadowRq.Proceed();
    return shadowRq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[shader("fragment")]
float4 fragMain(
    MSOutput input,
    float4 pixelCoord : SV_Position,
    uniform PushConstantDDGIRadiance pushConstant
) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    uint rayIndex = uint(pixelCoord.x);
    uint probeIndex = uint(pixelCoord.y);

    if (rayIndex >= pushConstant.raysPerProbe || probeIndex >= pushConstant.probeCountTotal)
    {
        return float4(0.0, 0.0, 0.0, 1000.0);
    }

    // Probes not updated this frame (ddgiUpdateStride) trace nothing; their blend keeps the atlas as it was.
    if (!ddgi_probe_updates(ubo, probeIndex, pushConstant.frameIndex))
        return float4(0.0, 0.0, 0.0, 1000.0);

    // From where last frame's relocation put it (a probe that just came into its cascade: its grid point; the slot's offset was the
    // probe's that left).
    const uint cascade = ddgi_cascade_of(ubo, probeIndex);
    float3 probePos = ddgi_cascade_origin(ubo, cascade) + float3(ddgi_local_coord(ubo, probeIndex)) * ddgi_cascade_spacing(ubo, cascade);
    if (!ddgi_probe_entered(ubo, probeIndex))
        probePos += ddgi_probe_offset(ubo, probeIndex, pushConstant.probeOffsetsTextureIndex);

    // Uniformly distributed spherical Fibonacci ray direction
    float3 baseRayDir = spherical_fibonacci(rayIndex, pushConstant.raysPerProbe);

    // Rotate ray distribution each frame around a fixed axis to maximize temporal sample coverage
    float rotAngle = float((pushConstant.frameIndex + probeIndex * 17) % 128) * (2.0 * 3.14159265359 / 128.0);
    float3 rotAxis = normalize(float3(0.57735, 0.57735, 0.57735));
    float3 rayDir = rotate_vector(baseRayDir, rotAxis, rotAngle);

    // Trace Ray against TLAS
    RayDesc rayDesc;
    rayDesc.Origin = probePos;
    rayDesc.Direction = rayDir;
    rayDesc.TMin = 0.01 * ubo->unitsPerMeter;
    // Far enough for the coarse cascades' cells too.
    rayDesc.TMax = max(100.0 * ubo->unitsPerMeter, 4.0 * length(ddgi_cascade_spacing(ubo, ddgi_cascade_of(ubo, probeIndex))));

    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    let rayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

    rq.TraceRayInline(accelerationStructure, rayFlags, 0xFF, rayDesc);

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
            float4 intersectionColor = SampleMaterialTextureLevel(ubo, lut.baseColorTextureIndex, textureSampler, hitUV, 0);

            if (intersectionColor.a >= lut.alphaCutoff)
            {
                rq.CommitNonOpaqueTriangleHit();
            }
        }
    }

    bool hit = (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT);

    // Ray Miss: sample skybox cubemap
    if (!hit)
    {
        float3 skyColor = float3(0.0);
        if (ubo->prefilteredMapIndex != 0xFFFFFFFF)
        {
            TextureCube prefilteredTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
            SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
            skyColor = prefilteredTex.SampleLevel(samplerPrefiltered, rayDir, 0).rgb * ubo->scaleIBLAmbient;
        }
        return float4(skyColor, 1000.0);
    }

    // Ray Hit: evaluate surface illumination
    uint committedInstanceID = rq.CommittedRayInstanceCustomIndex();
    uint committedPrimIndex = rq.CommittedPrimitiveIndex();
    float2 committedBary = rq.CommittedTriangleBarycentrics();
    float hitT = rq.CommittedRayT();

    GpuRayTracingInstance hitLut = instanceLUTBuffer[committedInstanceID];
    uint32_t* indices = (uint32_t*)hitLut.indexBufferAddress;

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

    Vertex vertex0 = LoadRayTracingVertex(hitLut, i0);
    Vertex vertex1 = LoadRayTracingVertex(hitLut, i1);
    Vertex vertex2 = LoadRayTracingVertex(hitLut, i2);

    float w0 = 1.0 - committedBary.x - committedBary.y;
    float w1 = committedBary.x;
    float w2 = committedBary.y;

    float2 hitUV = w0 * vertex0.uv0 + w1 * vertex1.uv0 + w2 * vertex2.uv0;
    float3 localNormal = normalize(w0 * vertex0.normal + w1 * vertex1.normal + w2 * vertex2.normal);
    float3 hitNormal = normalize(mul((float3x3)hitLut.normalMatrix, localNormal));

    // The back of single-sided geometry: the probe is inside something (a wall, a building). A negative distance marks it; the
    // blend keeps such rays out of the probe's light and switches off probes that see mostly back faces (classification).
    if (hitLut.doubleSided == 0 && dot(hitNormal, rayDir) > 0.1)
    {
        return float4(0.0, 0.0, 0.0, -hitT);
    }

    if (hitLut.doubleSided != 0 && dot(hitNormal, -rayDir) < 0.0)
    {
        hitNormal = -hitNormal;
    }

    float3 hitPos = probePos + rayDir * hitT;

    float3 albedo = hitLut.baseColorFactor.rgb;
    if (hitLut.baseColorTextureIndex != 0xFFFFFFFF)
    {
        albedo *= SampleMaterialTextureLevel(ubo, hitLut.baseColorTextureIndex, textureSampler, hitUV, 0).rgb;
    }

    // Direct light: the strongest lights at the hit exactly, one more for all the others (LightSelection.slang), each behind its
    // shadow ray; point and spot lights at most what they give 10 cm away.
    uint pickState = (probeIndex * 9781u + rayIndex * 6271u + pushConstant.frameIndex * 26699u) | 1u;
    const LightSelection selectedLights = select_lights(ubo, hitPos, hitNormal, 0.1 * ubo->unitsPerMeter, pickState);
    float3 directIrradiance = float3(0.0);
    for (uint i = 0; i < selectedLights.count; ++i)
    {
        if (!ddgiShadowed(accelerationStructure, ubo, hitPos + hitNormal * (0.02 * ubo->unitsPerMeter), selectedLights.direction[i],
                          max(selectedLights.distance[i] - 0.02 * ubo->unitsPerMeter, 0.0)))
            directIrradiance += selectedLights.irradiance[i];
    }
    const float3 directLight = albedo / 3.14159265 * directIrradiance;

    // Emissive component (e.g. glowing panels)
    float3 emissiveLight = hitLut.emissiveFactor.rgb * hitLut.emissiveFactor.a;

    // Multi-bounce diffuse GI: last frame's atlases at the hit surface. Not on the atlases' first frame: nothing has written them yet
    // (uninitialized memory can hold NaN, which the blend's hysteresis would then keep forever).
    float3 indirectLight = float3(0.0);
    if (pushConstant.prevIrradianceTextureIndex != 0xFFFFFFFF && pushConstant.prevDistanceTextureIndex != 0xFFFFFFFF)
    {
        Texture2D prevIrrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.prevIrradianceTextureIndex, 0));
        Texture2D prevDistTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.prevDistanceTextureIndex, 0));
        SamplerState ddgiSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));

        // The probes hold irradiance / pi (a cosine-weighted mean of radiance), as the lighting reads them: reflected = albedo * it.
        float3 bouncedIrradiance = sample_ddgi_diffuse_surface(ubo, hitPos, hitNormal, hitNormal, -rayDir, prevIrrTex, prevDistTex, ddgiSampler,
                                                               pushConstant.probeOffsetsTextureIndex);
        indirectLight = albedo * bouncedIrradiance;
    }
    else if (ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        // Initial frame fallback to scene irradiance cubemap
        TextureCube irradianceTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState samplerIrradiance = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        indirectLight = irradianceTex.SampleLevel(samplerIrradiance, hitNormal, 0).rgb * albedo * ubo->scaleIBLAmbient;
    }

    float3 hitRadiance = directLight + emissiveLight + indirectLight;
    return float4(hitRadiance, hitT);
}
