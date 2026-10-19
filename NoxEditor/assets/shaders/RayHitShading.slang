// Shading for rays traced from raster passes (no G-buffer at the hit). Needs shaderIO.h and DDGICommon.slang first.
#ifndef RAY_HIT_SHADING_SLANG
#define RAY_HIT_SHADING_SLANG

#include "SceneGI.slang"
#include "LightSelection.slang"

// Intersection UV interpolation
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
    float2 uv0 = vertices[i0].uv0;
    float2 uv1 = vertices[i1].uv0;
    float2 uv2 = vertices[i2].uv0;

    float w0 = 1.0 - barycentrics.x - barycentrics.y;
    float w1 = barycentrics.x;
    float w2 = barycentrics.y;

    return w0 * uv0 + w1 * uv1 + w2 * uv2;
}

// A shadow ray from a ray's hit (any hit ends it; alpha-masked occluders cut out).
bool ray_hit_shadowed(UniformBufferObject* ubo, float3 origin, float3 direction, float maxDistance)
{
    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.005 * ubo->unitsPerMeter;
    shadowRay.TMax = maxDistance;

    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    GpuRayTracingInstance* instanceLUTBuffer = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> shadowRq;
    shadowRq.TraceRayInline(accelerationStructure, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, shadowRay);
    while (shadowRq.Proceed())
    {
        GpuRayTracingInstance shadowLut = instanceLUTBuffer[shadowRq.CandidateRayInstanceCustomIndex()];
        if (shadowLut.alphaMode == 0 || shadowLut.baseColorTextureIndex == 0xFFFFFFFF)
        {
            shadowRq.CommitNonOpaqueTriangleHit();
        }
        else
        {
            float2 shadowUV = intersection_uv(shadowLut, shadowRq.CandidatePrimitiveIndex(), shadowRq.CandidateTriangleBarycentrics());
            Texture2D tex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + shadowLut.baseColorTextureIndex, 0));
            if (tex.SampleLevel(textureSampler, shadowUV, 0).a >= shadowLut.alphaCutoff)
                shadowRq.CommitNonOpaqueTriangleHit();
        }
    }
    return shadowRq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// The radiance leaving a ray's hit towards the ray's origin, without a G-buffer: the hit's base color, every analytic light
// with shadow rays (alpha-masked occluders cut out; the strongest exactly, LightSelection.slang), the scene's GI for the ambient part (SceneGI.slang: the
// screen-probe GI where the hit is on screen, else DDGI / the irradiance cubemap), emission.
// Diffuse only (the hit's own reflections are not traced). For rays traced from raster passes: RT reflections
// (Reflection.slang), RT refraction (Material_TransparentLit_Mesh.slang).
float3 shadeRayHit(UniformBufferObject* ubo, uint instanceSlot, uint primitive, float2 barycentrics, float3 rayOrigin, float3 R, float hitT)
{
    GpuRayTracingInstance* instanceLUTBuffer = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    uint committedPrimIndex = primitive;
    float2 committedBary = barycentrics;

    GpuRayTracingInstance hitLut = instanceLUTBuffer[instanceSlot];
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

    if (hitLut.doubleSided != 0 && dot(hitNormal, -R) < 0.0)
    {
        hitNormal = -hitNormal;
    }

    float3 hitPos = rayOrigin + R * hitT;

    float3 albedo = hitLut.baseColorFactor.rgb;
    if (hitLut.baseColorTextureIndex != 0xFFFFFFFF)
    {
        Texture2D hitTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + hitLut.baseColorTextureIndex, 0));
        albedo *= hitTex.SampleLevel(textureSampler, hitUV, 0).rgb;
    }

    // Direct light: the strongest lights at the hit exactly, one more for all the others (LightSelection.slang), each behind a
    // shadow ray with alpha-masked occluders cut out.
    uint lightRng = (primitive * 9781u + instanceSlot * 6271u + asuint(hitT) * 26699u) | 1u;
    const LightSelection selectedLights = select_lights(ubo, hitPos, hitNormal, 0.0, lightRng);
    float3 directIrradiance = float3(0.0);
    for (uint i = 0; i < selectedLights.count; ++i)
    {
        if (!ray_hit_shadowed(ubo, hitPos + hitNormal * (0.02 * ubo->unitsPerMeter), selectedLights.direction[i],
                              max(selectedLights.distance[i] - 0.02 * ubo->unitsPerMeter, 0.0)))
            directIrradiance += selectedLights.irradiance[i];
    }
    const float3 directLight = (albedo / 3.14159265) * directIrradiance;

    // Ambient diffuse light (irradiance / pi, as the deferred and forward lighting read it: the light the hit reflects is albedo
    // times it): the same light the camera sees on that surface where it is on screen, else DDGI or the environment. (The
    // environment's term matches getIBLContribution's diffuse in PBRLighting.slang exactly -- an extra * 0.2 here once made
    // reflections of environment-lit scenes such as Bistro almost black.)
    float3 ambientLight = albedo * 0.05 * ubo->scaleIBLAmbient;
    float3 sceneIrradiance;
    if (sample_scene_gi(ubo, hitPos, hitNormal, hitNormal, -R, sceneIrradiance))
        ambientLight = albedo * sceneIrradiance;

    // Emissive light from hit surface (e.g. Cornell Box ceiling light)
    float3 emissiveLight = hitLut.emissiveFactor.rgb * hitLut.emissiveFactor.a;

    return directLight + ambientLight + emissiveLight;
}

#endif
