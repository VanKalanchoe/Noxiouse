// Screen-probe GI, 2/5: the rays (ScreenProbeCommon.slang). One output texel per ray, 8 x 8 per probe: the radiance coming back
// along it (rgb) and how far it went in meters (a). A hit is shaded like a diffuse surface: the lights with shadow rays (the
// strongest few exactly, one for all the others: LightSelection.slang), its emission, and the light already bouncing around it
// from the DDGI cascades (the world radiance cache: the second bounce and beyond). A miss is the sky.
// Glass and clear blended surfaces let the rays through (light comes in through windows); alpha-masked leaves are cut out.
#include "ScreenProbeCommon.slang"
#include "RayHitShading.slang"
#include "LightSelection.slang"
#include "PathTracer/EmissiveLights.slang"

// Does this candidate stop light? Glass (transmission) never, cut-outs where their alpha passes, blended surfaces from half
// opaque on.
bool screen_probe_candidate_blocks(UniformBufferObject* ubo, GpuRayTracingInstance instance, uint primitive, float2 barycentrics)
{
    if (ray_see_through(instance.transmissionFactor, instance.alphaMode))
        return false;
    if (instance.alphaMode == 0)
        return true;
    SamplerState linearRepeat = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    const float alpha = instance.baseColorFactor.a *
                        RayHitAlpha(ubo, instance, linearRepeat, intersection_uv(instance, primitive, barycentrics), RayAlphaTestLod, primitive, barycentrics);
    return instance.alphaMode == 1 ? alpha >= instance.alphaCutoff : alpha >= 0.5;
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
    const float2 uv = ray_hit_uv(hit, w0 * v0.uv0 + barycentrics.x * v1.uv0 + barycentrics.y * v2.uv0);
    const float3 localNormal = w0 * v0.normal + barycentrics.x * v1.normal + barycentrics.y * v2.normal;
    // Which side the ray came from, by the triangle's own plane (an interpolated normal leans away near silhouettes), turned to
    // the side the vertex normals say is the front. Zero vertex normals (imported meshes have them) or a degenerate triangle
    // must not normalize a zero vector: one NaN here spread through the filter and the history over the whole screen.
    const float3 worldNormal = mul((float3x3)hit.normalMatrix, localNormal);
    float3 faceNormal = mul((float3x3)hit.normalMatrix, cross(v1.pos - v0.pos, v2.pos - v0.pos));
    const bool faceValid = dot(faceNormal, faceNormal) > 1e-20;
    const bool smoothValid = dot(worldNormal, worldNormal) > 1e-20;
    if (!faceValid && !smoothValid)
        return float3(0.0);
    faceNormal = faceValid ? normalize(faceNormal) : normalize(worldNormal);
    float3 normal = smoothValid ? normalize(worldNormal) : faceNormal;
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
    float3 albedo = hit.workflow == 1.0 ? hit.diffuseFactor.rgb : hit.baseColorFactor.rgb; // spec-gloss: the diffuse factor
    if (hit.baseColorTextureIndex != 0xFFFFFFFF)
    {
        albedo *= SampleMaterialTextureLevel(ubo, hit.baseColorTextureIndex, linearRepeat, uv, lod).rgb;
    }
    albedo *= RayTracingVertexColor(hit, uint3(i0, i1, i2), barycentrics).rgb; // glTF COLOR_0
    if (hit.workflow < 0.5) // metal reflects, it has no diffuse color
        albedo *= 1.0 - saturate(hit.metallicFactor);

    float3 emission = hit.emissiveFactor.rgb * hit.emissiveFactor.a;
    // An emissive mesh the probes sample as a light: its light comes in through those samples (screen_probe_trace), not through the rays.
    if (any(emission > 0.0) && EmissiveLights::find(ubo, instanceSlot) != 0xFFFFFFFF)
        emission = float3(0.0);
    if (hit.emissiveTextureIndex != 0xFFFFFFFF && any(emission > 0.0))
    {
        emission *= SampleMaterialTextureLevel(ubo, hit.emissiveTextureIndex, linearRepeat, uv, lod).rgb;
    }

    // Direct light: the strongest lights at the hit exactly, one more for all the others (LightSelection.slang), each behind its
    // shadow ray. Point and spot lights give at most what they give 10 cm away (a ray grazing a lamp's own fixture is no firefly).
    // Through glass tinted, as the path tracer's shadow rays (ray_transmittance, RayHitShading.slang).
    const float3 shadowOrigin = position + faceNormal * (0.02 * ubo->unitsPerMeter);
    uint rng = seed | 1u;
    const LightSelection lights = select_lights(ubo, position, normal, 0.1 * ubo->unitsPerMeter, rng);
    float3 irradiance = float3(0.0);
    for (uint i = 0; i < lights.count; ++i)
        irradiance += lights.irradiance[i] *
                      ray_transmittance(ubo, shadowOrigin, lights.direction[i], 0.0, max(lights.distance[i] - 0.02 * ubo->unitsPerMeter, 0.0));
    const float3 direct = albedo / ScreenProbePi * irradiance;

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

struct ScreenProbeTraceOutput
{
    float4 ray;      // radiance, distance in meters
    float4 emissive; // an emissive light sample (ScreenProbeCommon.slang ScreenProbeEmissiveSamples), 0: none
};

float4 screen_probe_trace_ray(uint2 texel, PushConstantScreenProbe pushConstant);

// One sample of the emissive meshes from the probe, behind its shadow ray: its share of the probe's irradiance / pi (before the
// cosine of the direction the irradiance is for) and its direction. Resampled from ScreenProbeEmissiveCandidates points by what
// each would give unshadowed (its emission, texture included, the probe's cosine, the distance): the list's own pick knows
// neither where the probe is nor that most of a facade's emissive texture is dark windows, and its raw samples boiled.
static const uint ScreenProbeEmissiveCandidates = 4;

float4 screen_probe_emissive_sample(UniformBufferObject* ubo, float3 origin, float3 probeNormal, uint seed)
{
    if (ubo->emissiveLightCount == 0 || ubo->tlasDeviceAddress == 0)
        return float4(0.0);
    EmissiveLights::Sample chosen = (EmissiveLights::Sample)0;
    float chosenTarget = 0.0;
    float weightSum = 0.0;
    for (uint candidate = 0; candidate < ScreenProbeEmissiveCandidates; ++candidate)
    {
        const uint candidateSeed = screen_probe_hash(seed + candidate * 0x9e3779b9u);
        const float4 u = float4(screen_probe_random(candidateSeed), screen_probe_random(candidateSeed ^ 0x68e31da4u),
                                screen_probe_random(candidateSeed ^ 0xb5297a4du), screen_probe_random(candidateSeed ^ 0x1b56c4e9u));
        // The emissive triangles by the light they give around the probe (EmissiveLightGrid.slang), as the path tracer samples them:
        // an instance and any of its triangles put nearly every sample on a lamp's dark fixture, and the rooms the lamps light came
        // out dim. The instances only when the triangle lights are not built.
        EmissiveLights::Sample sample = EmissiveLights::hasTriangleLights(ubo) ? EmissiveLights::sampleTriangles(ubo, origin, u)
                                                                             : EmissiveLights::sample(ubo, origin, u);
        const float cosProbe = dot(sample.direction, probeNormal);
        if (!sample.valid || cosProbe <= 0.0)
            continue;
        // No 1/d^2 spike next to a fixture: a sample credits its triangle with at most a hemisphere (2 pi sr), which no triangle
        // exceeds -- the bound acts only where the point estimate is wrong (closer than the triangle's own size). It used to be a
        // fixed 10 cm (a lamp nearer counted as 10 cm away), which dimmed every surface close to a small lamp: inside the
        // AnisotropyBarnLamp's shade (3-7 cm from the filament) 70% of the filament's light was lost, the shade gray, not white.
        sample.radiance *= min(1.0, 2.0 * ScreenProbePi / max(sample.solidAngle, 1e-20));
        // Not the cosine alone: the irradiance pass weighs a sample by the cosine of each direction it is for, so a sample picked
        // for a near-zero probe cosine came out huge in the sideways directions (unbounded spikes).
        const float target = dot(sample.radiance, float3(0.2126, 0.7152, 0.0722)) * (0.25 + 0.75 * cosProbe);
        const float weight = target / sample.pdf;
        if (!(weight > 0.0) || !isfinite(weight))
            continue;
        weightSum += weight;
        if (screen_probe_random(candidateSeed ^ 0x2545f491u) * weightSum < weight)
        {
            chosen = sample;
            chosenTarget = target;
        }
    }
    if (weightSum <= 0.0 || chosenTarget <= 0.0)
        return float4(0.0);
    // Through a lamp's glass tinted (ray_transmittance: the path tracer's shadow rays): a lantern's yellow panes lit the wall with its
    // bulb's white when glass let the shadow ray through untouched.
    const float3 transmittance = ray_transmittance(ubo, origin, chosen.direction, 0.0, max(chosen.distance - 0.02 * ubo->unitsPerMeter, 0.0));
    if (all(transmittance <= 0.0))
        return float4(0.0);

    // RIS: L(y) / target(y) * mean candidate weight; E / pi = 1 / (pi K) * sum over the probe's K samples, the cosine the irradiance
    // pass's.
    const float contributionWeight = weightSum / (float(ScreenProbeEmissiveCandidates) * chosenTarget);
    float3 share = chosen.radiance * transmittance * contributionWeight / (ScreenProbePi * float(ScreenProbeEmissiveSamples));
    if (any(isnan(share)) || any(isinf(share)))
        return float4(0.0);
    return float4(min(share, float3(4000.0)), screen_probe_pack_direction(chosen.direction)); // a half float; the irradiance pass sums in float
}

ScreenProbeTraceOutput screen_probe_trace(uint2 texel, PushConstantScreenProbe pushConstant)
{
    ScreenProbeTraceOutput output;
    output.ray = screen_probe_trace_ray(texel, pushConstant);
    output.emissive = float4(0.0);

    const uint2 probe = texel / ScreenProbeRayRes;
    const uint2 local = texel % ScreenProbeRayRes;
    if (probe.x >= pushConstant.probeCountX || probe.y >= pushConstant.probeCountY || !screen_probe_is_emissive_texel(local))
        return output;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    Texture2D positionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.probePositionTextureIndex, 0));
    const float4 probePosition = positionTex.Load(int3(probe, 0));
    if (probePosition.w <= 0.0)
        return output;
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.probeNormalTextureIndex, 0));
    const float3 probeNormal = normalTex.Load(int3(probe, 0)).xyz;
    const float viewDistance = length(probePosition.xyz - ubo->cameraWorldPos.xyz);
    const float3 origin = probePosition.xyz + probeNormal * raster_surface_bias(ubo, probePosition.xyz, 0.01 * ubo->unitsPerMeter + 0.001 * viewDistance);
    const uint seed = screen_probe_hash((probe.x * 7793u + probe.y * 3571u) ^ ((local.y * ScreenProbeRayRes + local.x) * 48611u) ^
                                       (pushConstant.frameIndex * 104729u) ^ 0x5bd1e995u);
    output.emissive = screen_probe_emissive_sample(ubo, origin, probeNormal, seed);
    return output;
}

float4 screen_probe_trace_ray(uint2 texel, PushConstantScreenProbe pushConstant)
{
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
    const float3 origin = probePosition.xyz + probeNormal * raster_surface_bias(ubo, probePosition.xyz, 0.01 * ubo->unitsPerMeter + 0.001 * viewDistance);

    RayDesc rayDesc;
    rayDesc.Origin = origin;
    rayDesc.Direction = direction;
    rayDesc.TMin = 0.0;
    rayDesc.TMax = 250.0 * ubo->unitsPerMeter;

    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(tlas, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, rayDesc);
    GpuRayTracingInstance* instances = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    bool passedThrough = false;
    while (query.Proceed())
    {
        if (screen_probe_candidate_blocks(ubo, instances[query.CandidateRayInstanceCustomIndex()], query.CandidatePrimitiveIndex(),
                                          query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
        else
            passedThrough = true;
    }
    // What came through glass (or a blended surface) on the way: tinted by it (ray_transmittance up to the hit, the path tracer's rule),
    // not as if the pane were not there. Only for rays that passed one.
    const float throughEnd = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? query.CommittedRayT() * 0.999 : rayDesc.TMax;
    const float3 throughGlass = passedThrough ? ray_transmittance(ubo, origin, direction, 0.0, throughEnd) : float3(1.0);

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
        return float4(sky * throughGlass, 60000.0);
    }

    const float hitT = query.CommittedRayT();
    const uint seed = screen_probe_hash((probe.x * 9781u + probe.y * 6271u) ^ (ray * 26699u) ^ (pushConstant.frameIndex * 7919u));
    float3 radiance = screen_probe_shade_hit(ubo, query.CommittedRayInstanceCustomIndex(), query.CommittedPrimitiveIndex(),
                                             query.CommittedTriangleBarycentrics(), origin, direction, hitT, seed) * throughGlass;
    // Half floats end at 65504: an overflow becomes Inf, then NaN in the history.
    radiance = all(isfinite(radiance)) ? min(radiance, float3(30000.0)) : float3(0.0);
    return float4(radiance, min(hitT / max(ubo->unitsPerMeter, 1e-6), 60000.0));
}

[shader("compute")]
[numthreads(SCREEN_PROBE_GROUP_SIZE, SCREEN_PROBE_GROUP_SIZE, 1)]
void computeMain(uint3 id : SV_DispatchThreadID, uniform PushConstantScreenProbe pushConstant)
{
    if (!screen_probe_in_target(pushConstant.outputStorageIndex0, id.xy))
        return;
    const ScreenProbeTraceOutput output = screen_probe_trace(id.xy, pushConstant);
    screen_probe_store(pushConstant.outputStorageIndex0, id.xy, output.ray);
    screen_probe_store(pushConstant.outputStorageIndex1, id.xy, output.emissive);
}
