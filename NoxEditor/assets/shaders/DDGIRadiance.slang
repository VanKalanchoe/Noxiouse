#include "shaderIO.h"
#include "DDGICommon.slang"

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

float2 intersection_uv(InstanceLUT lut, uint primIndex, float2 barycentrics)
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

    uint countX = uint(ubo->ddgiGridOrigin.w);
    uint countY = uint(ubo->ddgiGridSpacing.w);
    float3 gridOrigin = ubo->ddgiGridOrigin.xyz;
    float3 gridSpacing = ubo->ddgiGridSpacing.xyz;

    float3 probePos = get_probe_world_pos(probeIndex, gridOrigin, gridSpacing, countX, countY);

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
    rayDesc.TMin = 0.01;
    rayDesc.TMax = 100.0;

    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    let rayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

    rq.TraceRayInline(accelerationStructure, rayFlags, 0xFF, rayDesc);

    InstanceLUT* instanceLUTBuffer = (InstanceLUT*)ubo->instanceLUTReference;
    SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

    while (rq.Proceed())
    {
        uint instanceID = rq.CandidateRayInstanceCustomIndex();
        uint primIndex = rq.CandidatePrimitiveIndex();

        InstanceLUT lut = instanceLUTBuffer[instanceID];

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

    InstanceLUT hitLut = instanceLUTBuffer[committedInstanceID];
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
    float3 hitNormal = normalize(mul((float3x3)hitLut.normalMatrix, localNormal));

    if (hitLut.doubleSided != 0 && dot(hitNormal, -rayDir) < 0.0)
    {
        hitNormal = -hitNormal;
    }

    float3 hitPos = probePos + rayDir * hitT;

    float3 albedo = hitLut.baseColorFactor.rgb;
    if (hitLut.baseColorTextureIndex != 0xFFFFFFFF)
    {
        Texture2D hitTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + hitLut.baseColorTextureIndex, 0));
        albedo *= hitTex.SampleLevel(textureSampler, hitUV, 0).rgb;
    }

    // Direct Analytical Lighting
    LightData* lights = (LightData*)ubo->lightDataReference;
    float3 directLight = float3(0.0);

    for (uint i = 0; i < ubo->lightCount; i++)
    {
        LightData light = lights[i];
        uint lightType = (uint)light.position.w;

        float3 l;
        float dist = 1000.0;
        float attenuation = 1.0;

        if (lightType == 0) // Directional Light
        {
            l = normalize(light.direction.xyz);
        }
        else // Point (1) or Spot (2)
        {
            float3 lightVec = light.position.xyz - hitPos;
            dist = length(lightVec);
            l = dist > 0.0001 ? lightVec / dist : float3(0.0, 1.0, 0.0);

            float range = light.direction.w;
            if (range > 0.0)
            {
                float dOverR = dist / range;
                float window = max(1.0 - pow(dOverR, 4.0), 0.0);
                attenuation = window / (dist * dist);
            }
            else
            {
                attenuation = 1.0 / max(dist * dist, 0.001);
            }

            if (lightType == 2) // Spot Cone
            {
                float cd = dot(normalize(light.direction.xyz), -l);
                float lightAngleScale = light.spotParams.x;
                float lightAngleOffset = light.spotParams.y;
                float angularAttenuation = saturate(cd * lightAngleScale + lightAngleOffset);
                angularAttenuation *= angularAttenuation;
                attenuation *= angularAttenuation;
            }
        }

        if (attenuation <= 0.0)
            continue;

        float NdotL = dot(hitNormal, l);
        float3 shadowNormal = hitNormal;
        if (hitLut.doubleSided != 0)
        {
            if (NdotL < 0.0)
            {
                NdotL = -NdotL;
                shadowNormal = -hitNormal;
            }
        }

        if (NdotL > 0.001)
        {
            // Simple shadow test for direct light
            RayDesc shadowRay;
            shadowRay.Origin = hitPos + shadowNormal * 0.02;
            shadowRay.Direction = l;
            shadowRay.TMin = 0.005;
            shadowRay.TMax = (lightType == 0) ? 1000.0 : dist;

            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> shadowRq;
            shadowRq.TraceRayInline(accelerationStructure, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, shadowRay);
            shadowRq.Proceed();

            if (shadowRq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            {
                float3 lightRadiance = light.color.rgb * light.color.w * attenuation;
                directLight += (albedo / 3.14159265) * lightRadiance * NdotL;
            }
        }
    }

    // Emissive component (e.g. glowing panels)
    float3 emissiveLight = hitLut.emissiveFactor.rgb * hitLut.emissiveFactor.a;

    // Multi-Bounce Diffuse GI: Sample previous frame's DDGI probes at the hit surface!
    float3 indirectLight = float3(0.0);
    if (ubo->ddgiIrradianceTextureIndex != 0xFFFFFFFF && ubo->ddgiDistanceTextureIndex != 0xFFFFFFFF && pushConstant.frameIndex > 0)
    {
        Texture2D prevIrrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiIrradianceTextureIndex, 0));
        Texture2D prevDistTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiDistanceTextureIndex, 0));
        SamplerState ddgiSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));

        float3 bouncedIrradiance = sample_ddgi_diffuse(ubo, hitPos, hitNormal, prevIrrTex, prevDistTex, ddgiSampler);
        indirectLight = (albedo / 3.14159265) * bouncedIrradiance;
    }
    else if (ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        // Initial frame fallback to scene irradiance cubemap
        TextureCube irradianceTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState samplerIrradiance = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        indirectLight = irradianceTex.SampleLevel(samplerIrradiance, hitNormal, 0).rgb * (albedo / 3.14159265) * ubo->scaleIBLAmbient * 0.2;
    }

    float3 hitRadiance = directLight + emissiveLight + indirectLight;
    return float4(hitRadiance, hitT);
}
