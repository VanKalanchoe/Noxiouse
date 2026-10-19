// Screen-probe GI, 2/5: the rays (ScreenProbeCommon.slang). One output texel per ray, 8 x 8 per probe: the radiance coming back
// along it (rgb) and how far it went in meters (a). A hit is shaded like a diffuse surface: the lights with shadow rays (every
// light when there are a few, the sun always, else two picked in proportion to what they give), its emission, and the light
// already bouncing around it from the DDGI cascades (the world radiance cache: the second bounce and beyond). A miss is the sky.
// Glass and clear blended surfaces let the rays through (light comes in through windows); alpha-masked leaves are cut out.
#include "ScreenProbeCommon.slang"
#include "RayHitShading.slang"

// Does this candidate stop light? Glass (transmission) never, cut-outs where their alpha passes, blended surfaces from half
// opaque on.
bool screen_probe_candidate_blocks(UniformBufferObject* ubo, GpuRayTracingInstance instance, uint primitive, float2 barycentrics)
{
    if (instance.transmissionFactor >= 0.5)
        return false;
    if (instance.alphaMode == 0)
        return true;
    float alpha = instance.baseColorFactor.a;
    if (instance.baseColorTextureIndex != 0xFFFFFFFF)
    {
        const float2 uv = intersection_uv(instance, primitive, barycentrics);
        Texture2D tex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
        SamplerState linearRepeat = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
        alpha *= tex.SampleLevel(linearRepeat, uv, 0).a;
    }
    return instance.alphaMode == 1 ? alpha >= instance.alphaCutoff : alpha >= 0.5;
}

bool screen_probe_occluded(UniformBufferObject* ubo, float3 origin, float3 direction, float maxDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = maxDistance;

    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
    GpuRayTracingInstance* instances = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    while (query.Proceed())
    {
        if (screen_probe_candidate_blocks(ubo, instances[query.CandidateRayInstanceCustomIndex()], query.CandidatePrimitiveIndex(),
                                          query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// The radiance leaving a hit back along the ray (diffuse). The back of single-sided geometry (the ray got through a crack, or the
// probe's surface is a sliver inside something) gives no light.
float3 screen_probe_shade_hit(UniformBufferObject* ubo, uint instanceSlot, uint primitive, float2 barycentrics, float3 origin, float3 direction,
                              float hitT, uint seed)
{
    GpuRayTracingInstance* instances = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    const GpuRayTracingInstance hit = instances[instanceSlot];

    uint i0 = primitive * 3 + 0;
    uint i1 = primitive * 3 + 1;
    uint i2 = primitive * 3 + 2;
    if (hit.indexBufferAddress != 0)
    {
        uint32_t* indices = (uint32_t*)hit.indexBufferAddress;
        i0 = indices[i0];
        i1 = indices[i1];
        i2 = indices[i2];
    }
    const Vertex v0 = LoadRayTracingVertex(hit, i0);
    const Vertex v1 = LoadRayTracingVertex(hit, i1);
    const Vertex v2 = LoadRayTracingVertex(hit, i2);
    const float w0 = 1.0 - barycentrics.x - barycentrics.y;
    const float2 uv = w0 * v0.uv0 + barycentrics.x * v1.uv0 + barycentrics.y * v2.uv0;
    const float3 localNormal = w0 * v0.normal + barycentrics.x * v1.normal + barycentrics.y * v2.normal;
    float3 normal = normalize(mul((float3x3)hit.normalMatrix, localNormal));
    // Which side the ray came from, by the triangle's own plane (an interpolated normal leans away near silhouettes), turned to
    // the side the vertex normals say is the front.
    float3 faceNormal = mul((float3x3)hit.normalMatrix, cross(v1.pos - v0.pos, v2.pos - v0.pos));
    faceNormal = dot(faceNormal, faceNormal) > 1e-20 ? normalize(faceNormal) : normal;
    if (dot(faceNormal, normal) < 0.0)
        faceNormal = -faceNormal;

    if (dot(faceNormal, direction) > 0.0)
    {
        if (hit.doubleSided == 0)
            return float3(0.0);
        normal = -normal;
        faceNormal = -faceNormal;
    }

    const float3 position = origin + direction * hitT;
    const float hitMeters = hitT / max(ubo->unitsPerMeter, 1e-6);

    // A probe ray stands for a cone of about 0.4 rad: what it sees of a texture is its average over a patch that wide, a high mip.
    const float lod = clamp(log2(max(hitMeters, 1e-3) * 128.0), 0.0, 14.0);
    SamplerState linearRepeat = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    float3 albedo = hit.baseColorFactor.rgb;
    if (hit.baseColorTextureIndex != 0xFFFFFFFF)
    {
        Texture2D tex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + hit.baseColorTextureIndex, 0));
        albedo *= tex.SampleLevel(linearRepeat, uv, lod).rgb;
    }
    if (hit.workflow < 0.5) // metal reflects, it has no diffuse color
        albedo *= 1.0 - saturate(hit.metallicFactor);

    float3 emission = hit.emissiveFactor.rgb * hit.emissiveFactor.a;
    if (hit.emissiveTextureIndex != 0xFFFFFFFF && any(emission > 0.0))
    {
        Texture2D tex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + hit.emissiveTextureIndex, 0));
        emission *= tex.SampleLevel(linearRepeat, uv, lod).rgb;
    }

    // Direct light. Every light's unshadowed share costs nothing; the shadow rays are what costs, and scenes can have hundreds of
    // lights: with a few each gets its ray, with more two are picked in proportion to their share (weighted reservoir sampling)
    // and only they trace one. Directional lights (the sun) always trace their own.
    LightData* lights = (LightData*)ubo->lightDataReference;
    const uint ExactLightCount = 4;
    const uint PickedLights = 2;
    const bool pickLights = ubo->lightCount > ExactLightCount;
    const float3 shadowOrigin = position + faceNormal * (0.02 * ubo->unitsPerMeter);

    float3 direct = float3(0.0);
    uint pickState = seed | 1u;
    float pickedWeightSum[PickedLights];
    float3 pickedContribution[PickedLights];
    float3 pickedDirection[PickedLights];
    float pickedDistance[PickedLights];
    bool picked[PickedLights];
    for (uint slot = 0; slot < PickedLights; ++slot)
    {
        pickedWeightSum[slot] = 0.0;
        pickedContribution[slot] = float3(0.0);
        pickedDirection[slot] = float3(0.0, 1.0, 0.0);
        pickedDistance[slot] = 0.0;
        picked[slot] = false;
    }

    for (uint i = 0; i < ubo->lightCount; ++i)
    {
        const LightData light = lights[i];
        const uint lightType = (uint)light.position.w;
        float3 l;
        float distance = 1000.0 * ubo->unitsPerMeter;
        float attenuation = 1.0;
        if (lightType == 0)
        {
            l = normalize(light.direction.xyz);
        }
        else
        {
            const float3 toLight = light.position.xyz - position;
            distance = length(toLight);
            l = distance > 1e-4 ? toLight / distance : float3(0.0, 1.0, 0.0);
            const float range = light.direction.w;
            attenuation = range > 0.0 ? max(1.0 - pow(distance / range, 4.0), 0.0) / max(distance * distance, 1e-4)
                                      : 1.0 / max(distance * distance, 1e-4);
            if (lightType == 2)
            {
                const float angular = saturate(dot(normalize(light.direction.xyz), -l) * light.spotParams.x + light.spotParams.y);
                attenuation *= angular * angular;
            }
        }
        const float NdotL = dot(normal, l);
        if (attenuation <= 0.0 || NdotL <= 0.0)
            continue;

        const float3 contribution = (albedo / ScreenProbePi) * light.color.rgb * light.color.w * attenuation * NdotL;
        if (!pickLights || lightType == 0)
        {
            if (!screen_probe_occluded(ubo, shadowOrigin, l, distance))
                direct += contribution;
            continue;
        }

        const float weight = dot(contribution, float3(0.2126, 0.7152, 0.0722));
        if (weight <= 0.0)
            continue;
        for (uint slot = 0; slot < PickedLights; ++slot)
        {
            pickedWeightSum[slot] += weight;
            pickState ^= pickState << 13;
            pickState ^= pickState >> 17;
            pickState ^= pickState << 5;
            const float u = float(pickState >> 8) * (1.0 / 16777216.0);
            if (u * pickedWeightSum[slot] < weight)
            {
                picked[slot] = true;
                pickedContribution[slot] = contribution / weight;
                pickedDirection[slot] = l;
                pickedDistance[slot] = distance;
            }
        }
    }
    for (uint slot = 0; slot < PickedLights; ++slot)
    {
        if (picked[slot] && !screen_probe_occluded(ubo, shadowOrigin, pickedDirection[slot], pickedDistance[slot]))
            direct += pickedContribution[slot] * pickedWeightSum[slot] / float(PickedLights);
    }

    // The light around the hit: the DDGI cascades (seen from the probe), else the environment's irradiance.
    float3 bounce = float3(0.0);
    if (ubo->enableDDGI != 0 && ubo->ddgiIrradianceTextureIndex != 0xFFFFFFFF && ubo->ddgiDistanceTextureIndex != 0xFFFFFFFF)
    {
        Texture2D irradianceAtlas = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiIrradianceTextureIndex, 0));
        Texture2D distanceAtlas = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiDistanceTextureIndex, 0));
        SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
        bounce = sample_ddgi_diffuse_surface(ubo, position, normal, normal, -direction, irradianceAtlas, distanceAtlas, linearClamp);
    }
    else if (ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        TextureCube irradiance = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState irradianceSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        bounce = irradiance.SampleLevel(irradianceSampler, normal, 0).rgb * ubo->scaleIBLAmbient;
    }

    return direct + emission + albedo * bounce;
}

[shader("fragment")]
float4 fragMain(ScreenProbeVertex input, uniform PushConstantScreenProbe pushConstant) : SV_Target0
{
    const uint2 texel = uint2(input.pos.xy);
    const uint2 probe = texel / ScreenProbeRayRes;
    const uint2 local = texel % ScreenProbeRayRes;
    if (probe.x >= pushConstant.probeCountX || probe.y >= pushConstant.probeCountY)
        return float4(0.0);

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    Texture2D positionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.probePositionTextureIndex, 0));
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.probeNormalTextureIndex, 0));
    const float4 probePosition = positionTex.Load(int3(probe, 0));
    if (probePosition.w <= 0.0 || ubo->tlasDeviceAddress == 0)
        return float4(0.0);
    const float3 probeNormal = normalTex.Load(int3(probe, 0)).xyz;

    const uint ray = local.y * ScreenProbeRayRes + local.x;
    const float3 direction = screen_probe_ray_direction(probeNormal, probe, ray, pushConstant.frameIndex);

    // Off the surface by a little more far away, where the depth buffer's positions are coarser.
    const float viewDistance = length(probePosition.xyz - ubo->cameraWorldPos.xyz);
    const float3 origin = probePosition.xyz + probeNormal * (0.01 * ubo->unitsPerMeter + 0.001 * viewDistance);

    RayDesc rayDesc;
    rayDesc.Origin = origin;
    rayDesc.Direction = direction;
    rayDesc.TMin = 0.0;
    rayDesc.TMax = 250.0 * ubo->unitsPerMeter;

    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(tlas, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, rayDesc);
    GpuRayTracingInstance* instances = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    while (query.Proceed())
    {
        if (screen_probe_candidate_blocks(ubo, instances[query.CandidateRayInstanceCustomIndex()], query.CandidatePrimitiveIndex(),
                                          query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
    }

    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        // The sky, a little blurred (one ray stands for a cone; the sun itself is a light, not in the environment's sharp mips).
        float3 sky = float3(0.0);
        if (ubo->prefilteredMapIndex != 0xFFFFFFFF)
        {
            TextureCube prefiltered = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
            SamplerState prefilterSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
            const float lod = min(2.0, max(ubo->prefilteredCubeMipLevels, 0.0));
            sky = prefiltered.SampleLevel(prefilterSampler, direction, lod).rgb * ubo->scaleIBLAmbient;
        }
        return float4(sky, 60000.0);
    }

    const float hitT = query.CommittedRayT();
    const uint seed = screen_probe_hash((probe.x * 9781u + probe.y * 6271u) ^ (ray * 26699u) ^ (pushConstant.frameIndex * 7919u));
    const float3 radiance = screen_probe_shade_hit(ubo, query.CommittedRayInstanceCustomIndex(), query.CommittedPrimitiveIndex(),
                                                   query.CommittedTriangleBarycentrics(), origin, direction, hitT, seed);
    return float4(radiance, min(hitT / max(ubo->unitsPerMeter, 1e-6), 60000.0));
}
