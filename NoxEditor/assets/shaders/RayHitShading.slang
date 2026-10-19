// Shading for rays traced from raster passes (no G-buffer at the hit). Needs shaderIO.h and DDGICommon.slang first.
#ifndef RAY_HIT_SHADING_SLANG
#define RAY_HIT_SHADING_SLANG

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

// The radiance leaving a ray's hit towards the ray's origin, without a G-buffer: the hit's base color, every analytic light
// with one shadow ray each (alpha-masked occluders cut out), DDGI or the irradiance cubemap for the ambient part, emission.
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

    // Direct analytical lighting from scene lights
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
                attenuation = 1.0 / (dist * dist);
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
        else
        {
            NdotL = max(NdotL, 0.0);
        }

        if (NdotL > 0.0)
        {
            // Shadow test: Ensure reflected surface is truly lit and not in shadow
            RayDesc shadowRay;
            shadowRay.Origin = hitPos + shadowNormal * (0.02 * ubo->unitsPerMeter);
            shadowRay.Direction = l;
            shadowRay.TMin = 0.005 * ubo->unitsPerMeter;
            shadowRay.TMax = (lightType == 0) ? 1000.0 * ubo->unitsPerMeter : dist;

            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> shadowRq;
            shadowRq.TraceRayInline(accelerationStructure, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, shadowRay);
            
            while (shadowRq.Proceed())
            {
                uint shadowInstanceID = shadowRq.CandidateRayInstanceCustomIndex();
                uint shadowPrimIndex = shadowRq.CandidatePrimitiveIndex();
                GpuRayTracingInstance shadowLut = instanceLUTBuffer[shadowInstanceID];

                if (shadowLut.alphaMode == 0 || shadowLut.baseColorTextureIndex == 0xFFFFFFFF)
                {
                    shadowRq.CommitNonOpaqueTriangleHit();
                }
                else
                {
                    float2 shadowUV = intersection_uv(shadowLut, shadowPrimIndex, shadowRq.CandidateTriangleBarycentrics());
                    Texture2D tex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + shadowLut.baseColorTextureIndex, 0));
                    float4 intersectionColor = tex.SampleLevel(textureSampler, shadowUV, 0);

                    if (intersectionColor.a >= shadowLut.alphaCutoff)
                    {
                        shadowRq.CommitNonOpaqueTriangleHit();
                    }
                }
            }

            if (shadowRq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            {
                float3 lightRadiance = light.color.rgb * light.color.w * attenuation;
                directLight += (albedo / 3.14159265) * lightRadiance * NdotL;
            }
        }
    }

    // Ambient diffuse light: sample DDGI probes if active, or scene irradiance cubemap. Both hold irradiance / pi (cosine-weighted
    // mean radiance), as the deferred and forward lighting read them: the light the hit reflects is albedo times it.
    float3 ambientLight = float3(0.0);
    if (ubo->enableDDGI != 0 && ubo->ddgiIrradianceTextureIndex != 0xFFFFFFFF && ubo->ddgiDistanceTextureIndex != 0xFFFFFFFF)
    {
        Texture2D ddgiIrrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiIrradianceTextureIndex, 0));
        Texture2D ddgiDistTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiDistanceTextureIndex, 0));
        SamplerState ddgiSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));

        float3 ddgiIrr = sample_ddgi_diffuse(ubo, hitPos, hitNormal, ddgiIrrTex, ddgiDistTex, ddgiSampler);
        ambientLight = albedo * ddgiIrr;
    }
    else if (ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        TextureCube irradianceTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState samplerIrradiance = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        // Matches getIBLContribution's diffuse term in PBRLighting.slang exactly (the same irradiance cubemap, the
        // same scaleIBLAmbient, no extra factor) -- this used to have an additional, undocumented * 0.2 here, making
        // a reflected surface's ambient lighting five times dimmer than the exact same surface lit directly. Never
        // visible in a scene with strong direct lights (they dominate the sum below); in an environment-only scene
        // (Bistro, no direct light does anything on its own) that dim ambient term was the reflection's entire
        // radiance, reading as almost black.
        ambientLight = irradianceTex.SampleLevel(samplerIrradiance, hitNormal, 0).rgb * albedo * ubo->scaleIBLAmbient;
    }
    else
    {
        ambientLight = albedo * 0.05 * ubo->scaleIBLAmbient;
    }

    // Emissive light from hit surface (e.g. Cornell Box ceiling light)
    float3 emissiveLight = hitLut.emissiveFactor.rgb * hitLut.emissiveFactor.a;

    return directLight + ambientLight + emissiveLight;
}

#endif
