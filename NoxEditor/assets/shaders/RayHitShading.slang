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
        if (ray_shadow_passes(shadowLut))
            continue;
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

// A ray hit's diffuse albedo, specular F0 and perceptual roughness, as the path tracer reads its material (PathTracerBridge.slang):
// metallic-roughness from the factors and the metallic-roughness texture (G roughness, B metallic) with the IoR's dielectric F0;
// specular-glossiness (RGB specular, A glossiness) converted to it (specGlossToMetalRough there, Donut's formula).
void hit_material(UniformBufferObject* ubo, GpuRayTracingInstance lut, float3 color, float2 uv, SamplerState textureSampler,
                  out float3 albedo, out float3 specularF0, out float roughness)
{
    const float ior = lut.ior > 0.0 ? lut.ior : 1.5;
    const float dielectricF0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    float4 mrSample = float4(1.0);
    if (lut.metallicRoughnessTextureIndex != 0xFFFFFFFF)
    {
        Texture2D mrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.metallicRoughnessTextureIndex, 0));
        mrSample = mrTex.SampleLevel(textureSampler, uv, 0);
    }

    if (lut.workflow != 1.0)
    {
        const float metallic = saturate(lut.metallicFactor * mrSample.b);
        roughness = saturate(lut.roughnessFactor * mrSample.g);
        albedo = color * (1.0 - metallic);
        specularF0 = lerp(float3(dielectricF0), color, metallic);
        return;
    }

    roughness = saturate(1.0 - lut.specularFactor.a * mrSample.a);
    const float3 specularColor = lut.specularFactor.rgb * mrSample.rgb;
    const float dielectricSpecular = 0.04;
    const float specularStrength = max(specularColor.r, max(specularColor.g, specularColor.b));
    const float diffuseBrightness = sqrt(dot(color * color, float3(0.299, 0.587, 0.114)));
    const float specularBrightness = sqrt(dot(specularColor * specularColor, float3(0.299, 0.587, 0.114)));
    float metallic = 0.0;
    if (specularBrightness >= dielectricSpecular)
    {
        const float a = dielectricSpecular;
        const float b = diffuseBrightness * (1.0 - specularStrength) / (1.0 - dielectricSpecular) + specularBrightness - 2.0 * dielectricSpecular;
        const float c = dielectricSpecular - specularBrightness;
        metallic = saturate((-b + sqrt(max(b * b - 4.0 * a * c, 0.0))) / (2.0 * a));
    }
    const float epsilon = 1e-6;
    const float3 fromDiffuse = color * ((1.0 - specularStrength) / (1.0 - dielectricSpecular) / max(1.0 - metallic, epsilon));
    const float3 fromSpecular = specularColor - dielectricSpecular * (1.0 - metallic) / max(metallic, epsilon);
    const float3 converted = saturate(lerp(fromDiffuse, fromSpecular, metallic * metallic));
    albedo = converted * (1.0 - metallic);
    specularF0 = lerp(float3(dielectricF0), converted, metallic);
}

// The radiance leaving a ray's hit towards the ray's origin, without a G-buffer: the hit's base color, every analytic light
// with shadow rays (alpha-masked occluders cut out; the strongest exactly, LightSelection.slang), the scene's GI for the ambient part (SceneGI.slang: the
// screen-probe GI where the hit is on screen, else DDGI / the irradiance cubemap), emission.
// Diffuse only (the hit's own reflections are not traced). For rays traced from raster passes: RT reflections
// (Reflection.slang), RT refraction (Material_TransparentLit_Mesh.slang). `emissionWeight` scales the hit's emission: the MIS weight
// of a ray whose pass also samples the emissive triangles as lights (Reflection.slang), 1 otherwise.
float3 shadeRayHit(UniformBufferObject* ubo, uint instanceSlot, uint primitive, float2 barycentrics, float3 rayOrigin, float3 R, float hitT,
                   float emissionWeight = 1.0)
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
    // Zero vertex normals (imported meshes have them): the triangle's own normal instead of normalize(0) = NaN.
    float3 hitNormal = mul((float3x3)hitLut.normalMatrix, w0 * vertex0.normal + w1 * vertex1.normal + w2 * vertex2.normal);
    if (dot(hitNormal, hitNormal) <= 1e-20)
        hitNormal = mul((float3x3)hitLut.normalMatrix, cross(vertex1.pos - vertex0.pos, vertex2.pos - vertex0.pos));
    if (dot(hitNormal, hitNormal) <= 1e-20)
        return float3(0.0);
    hitNormal = normalize(hitNormal);

    if (hitLut.doubleSided != 0 && dot(hitNormal, -R) < 0.0)
    {
        hitNormal = -hitNormal;
    }

    float3 hitPos = rayOrigin + R * hitT;

    // Specular-glossiness materials keep their color in the diffuse factor (the base color factor is the default white there), as the
    // G-buffer and the path tracer read it.
    float3 baseColor = hitLut.workflow == 1.0 ? hitLut.diffuseFactor.rgb : hitLut.baseColorFactor.rgb;
    if (hitLut.baseColorTextureIndex != 0xFFFFFFFF)
    {
        Texture2D hitTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + hitLut.baseColorTextureIndex, 0));
        baseColor *= hitTex.SampleLevel(textureSampler, hitUV, 0).rgb;
    }
    // Its diffuse and specular parts, as the path tracer splits them (PathTracerBridge.slang: the metallic-roughness inputs, or the
    // specular-glossiness ones converted). A metal is all specular: chrome's diffuse is near black.
    float3 albedo, specularF0;
    float roughness;
    hit_material(ubo, hitLut, baseColor, hitUV, textureSampler, albedo, specularF0, roughness);

    // Direct light: the strongest lights at the hit exactly, one more for all the others (LightSelection.slang), each behind a
    // shadow ray with alpha-masked occluders cut out.
    uint lightRng = (primitive * 9781u + instanceSlot * 6271u + asuint(hitT) * 26699u) | 1u;
    // Lights closer than 10 cm count as 10 cm away (as the screen probes' rays do): a hit right next to a lamp otherwise gets an
    // inverse-square spike that a 1-sample reflection shows as fireflies, worst at night where lamps are most of the light.
    const LightSelection selectedLights = select_lights(ubo, hitPos, hitNormal, 0.1 * ubo->unitsPerMeter, lightRng);
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
    float specularOcclusion = 1.0;
    if (sample_scene_gi(ubo, hitPos, hitNormal, hitNormal, -R, sceneIrradiance))
    {
        ambientLight = albedo * sceneIrradiance;
        specularOcclusion = scene_gi_specular_occlusion(ubo, sceneIrradiance, hitNormal);
    }

    // The hit's own reflection, its last bounce: the environment along its mirror direction (no further ray), through its specular
    // color and the split-sum BRDF, occluded as the deferred lighting occludes it. Unreal's ray traced reflections end the same way
    // (reflection captures for the last bounce). Without it every reflection of a metal was black: the headlight's chrome reflector,
    // which the path tracer shows mirroring the street, its rays bouncing on.
    float3 specularLight = float3(0.0);
    if (ubo->prefilteredMapIndex != 0xFFFFFFFF && ubo->brdfLutIndex != 0xFFFFFFFF && any(specularF0 > 0.0))
    {
        const float NdotV = clamp(dot(hitNormal, -R), 0.001, 1.0);
        TextureCube environment = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
        const float3 environmentLight = environment.SampleLevel(SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0)),
                                                                reflect(R, hitNormal), roughness * ubo->prefilteredCubeMipLevels).rgb;
        Texture2D brdfTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->brdfLutIndex, 0));
        const float2 brdf = brdfTex.SampleLevel(SamplerState.Handle(uint2(SamplerIndex::SAMPLER_BRDFLUT, 0)), float2(NdotV, 1.0 - roughness), 0).rg;
        specularLight = environmentLight * ubo->scaleIBLAmbient * (specularF0 * brdf.x + brdf.y) * specularOcclusion;
    }

    // Emissive light from hit surface (e.g. Cornell Box ceiling light)
    float3 emissiveLight = hitLut.emissiveFactor.rgb * hitLut.emissiveFactor.a;
    // With its texture (a facade's emissive map is mostly dark windows: without it the whole facade glowed in reflections).
    if (hitLut.emissiveTextureIndex != 0xFFFFFFFF && any(emissiveLight > 0.0))
    {
        Texture2D emissiveTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + hitLut.emissiveTextureIndex, 0));
        emissiveLight *= emissiveTex.SampleLevel(SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0)), hitUV, 2.0).rgb;
    }

    return directLight + ambientLight + specularLight + emissiveLight * emissionWeight;
}

#endif
