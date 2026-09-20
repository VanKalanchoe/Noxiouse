#include "shaderIO.h"
#include "NRDFrontEnd.slang"
#include "DDGICommon.slang"
#include "PBRLighting.slang"

    import Tonemapping;
    import Math;

    struct DeferredLightingPayload
    {
        uint dummy;
    };
    groupshared DeferredLightingPayload gPayload;

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
        uniform PushConstantDeferredLighting pushConstant
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

    // PBRInfo, IBLResult, diffuse/specularReflection/geometricOcclusion/microfacetDistribution,
    // evaluateDirectLight and getIBLContribution now live in PBRLighting.slang (included above) -
    // shared with the forward transparent pass so there's one lighting implementation, not two.

    // Generates a distinct, saturated pseudo-random color for each unique entity ID
    float3 idToColor(int id)
    {
        if (id < 0) return float3(0.05, 0.05, 0.05); // Background / unassigned
        // Golden ratio hue distribution guarantees adjacent IDs have radically different hues
        float hue = frac(float(id) * 0.618033988749895);
        // Convert HSV (Hue, Saturation = 0.85, Value = 0.95) to RGB
        float3 rgb = saturate(abs(fmod(hue * 6.0 + float3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0);
        return rgb * 0.85 + 0.1;
    }

    float2 intersection_uv(GpuRayTracingInstance lut, uint primIndex, float2 barycentrics)
    {
        uint32_t* indices = (uint32_t*)lut.indexBufferAddress;
        uint i0 = indices[primIndex * 3 + 0];
        uint i1 = indices[primIndex * 3 + 1];
        uint i2 = indices[primIndex * 3 + 2];

        Vertex* vertices = (Vertex*)lut.vertexBufferAddress;
        float2 uv0 = vertices[i0].uv0;
        float2 uv1 = vertices[i1].uv0;
        float2 uv2 = vertices[i2].uv0;

        float w0 = 1.0 - barycentrics.x - barycentrics.y;
        float w1 = barycentrics.x;
        float w2 = barycentrics.y;

        return w0 * uv0 + w1 * uv1 + w2 * uv2;
    }

    bool trace_shadow_ray(UniformBufferObject* ubo, float3 rayOrigin, float3 rayDir, float maxDist)
    {
        if (ubo->enableRTShadows == 0 || ubo->tlasDeviceAddress == 0)
            return false;

        RayDesc shadowRayDesc;
        shadowRayDesc.Origin = rayOrigin;
        shadowRayDesc.Direction = rayDir;
        shadowRayDesc.TMin = 0.005;
        shadowRayDesc.TMax = maxDist;

        RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
                 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
        let rayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
                       RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;

        sq.TraceRayInline(accelerationStructure, rayFlags, 0xFF, shadowRayDesc);

        GpuRayTracingInstance* instanceLUTBuffer = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
        SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

        while (sq.Proceed())
        {
            uint instanceID = sq.CandidateRayInstanceCustomIndex();
            uint primIndex = sq.CandidatePrimitiveIndex();

            GpuRayTracingInstance lut = instanceLUTBuffer[instanceID];

            if (lut.alphaMode == 0 || lut.baseColorTextureIndex == 0xFFFFFFFF)
            {
                sq.CommitNonOpaqueTriangleHit();
            }
            else
            {
                float2 uv = intersection_uv(lut, primIndex, sq.CandidateTriangleBarycentrics());
                Texture2D tex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.baseColorTextureIndex, 0));
                float4 intersectionColor = tex.SampleLevel(textureSampler, uv, 0);

                if (intersectionColor.a >= lut.alphaCutoff)
                {
                    sq.CommitNonOpaqueTriangleHit();
                }
            }
        }

        return (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT);
    }

    [shader("fragment")]
    float4 fragMain(
        MSOutput input,
        uniform PushConstantDeferredLighting pushConstant
    ) : SV_Target0
    {
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        int2 pixelCoord = int2(input.pos.xy);


        // =========================================================================
            // Mode 12: Whole-Screen Depth Buffer (Hi-Z / Shadow Map View)
            // =========================================================================
            if (pushConstant.debugMode == 12)
            {
                Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.
  depthTextureIndex, 0));
                float rawDepth = depthTex.Load(int3(pixelCoord, 0)).r;

                // In Reverse-Z, 0.0 is the far plane / background -> pure white (1.0)
                if (rawDepth <= 0.0)
                {
                    return float4(1.0, 1.0, 1.0, 1.0);
                }

                // For geometry: reconstruct world position to get exact linear camera distance
                float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
                float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
                float4 ndc = float4(ndc_xy, rawDepth, 1.0);
                float4 worldPosH = mul(pushConstant.invViewProj, ndc);
                float3 worldPos = worldPosH.xyz / worldPosH.w;

                float linearDist = length(worldPos - ubo->cameraWorldPos.xyz);

                // Near = Black/Dark Gray, Far = White (1.0), seamlessly merging into white background
                float visualDepth = saturate(linearDist / 25.0);
                return float4(visualDepth, visualDepth, visualDepth, 1.0);
            }

            // Mode 15: Motion Vectors (Velocity Buffer)
                if (pushConstant.debugMode == 15)
                {
                    Texture2D velocityTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferVelocityIndex, 0));
                    float2 vel = velocityTex.Load(int3(pixelCoord, 0)).rg;
                    // Amplify 100x so motion is vividly visible in the viewport
                    // Red = horizontal motion, Green = vertical motion
                    float3 visualVel = float3(abs(vel.x) * 100.0, abs(vel.y) * 100.0, 0.0);
                    return float4(visualVel, 1.0);
                }

            // Mode 16: Diffuse GI (ReSTIR GI Diffuse or DDGI Probes)
                if (pushConstant.debugMode == 16)
                {
                    if (pushConstant.diffuseGIMode == 2 && pushConstant.restirGIDiffuseTextureIndex != 0xFFFFFFFF)
                    {
                        Texture2D restirTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.restirGIDiffuseTextureIndex, 0));
                        float3 restirDiffuse = NRD_UnpackSignal(pushConstant.restirGIDenoiserMode, restirTex.Load(int3(pixelCoord, 0)));
                        return float4(restirDiffuse, 1.0);
                    }
                    else if (ubo->ddgiIrradianceTextureIndex != 0xFFFFFFFF && ubo->ddgiDistanceTextureIndex != 0xFFFFFFFF)
                    {
                        Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
                        float4 normalSample = normalTex.Load(int3(pixelCoord, 0));
                        float lenSqN = dot(normalSample.rgb, normalSample.rgb);
                        float3 n = lenSqN > 1e-6 ? normalSample.rgb * rsqrt(lenSqN) : float3(0.0, 1.0, 0.0);

                        Texture2D ddgiIrrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiIrradianceTextureIndex, 0));
                        Texture2D ddgiDistTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiDistanceTextureIndex, 0));
                        SamplerState ddgiSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));

                        float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
                        float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
                        Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
                        float depthVal = depthTex.Load(int3(pixelCoord, 0)).r;
                        float4 ndc = float4(ndc_xy, depthVal, 1.0);
                        float4 worldPosH = mul(ubo->invViewProj, ndc);
                        float3 worldPos = worldPosH.xyz / worldPosH.w;

                        float3 ddgiIrradiance = sample_ddgi_diffuse(ubo, worldPos, n, ddgiIrrTex, ddgiDistTex, ddgiSampler);
                        return float4(ddgiIrradiance, 1.0);
                    }
                }

            // 1. Check Visibility Buffer and Depth to distinguish background from geometry
                Texture2D<uint4> visTex = Texture2D<uint4>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.visibilityTextureIndex, 0));
                uint4 rawVis = visTex.Load(int3(pixelCoord, 0));
                uint2 visData = rawVis.xy;

                Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
                float depth = depthTex.Load(int3(pixelCoord, 0)).r;

                // In Reverse-Z: 0.0 is background/skybox -> discard so skybox pass can render!
                if (visData.x == 0 || depth <= 0.0)
                {
                    discard;
                }

        // 2. Fetch G-Buffer Render Targets
        Texture2D albedoTex   = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferAlbedoIndex, 0));
        Texture2D normalTex   = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
        Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferMaterialIndex, 0));
        Texture2D emissionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferEmissionIndex, 0));

        float4 albedoSample   = albedoTex.Load(int3(pixelCoord, 0));
        float4 normalSample   = normalTex.Load(int3(pixelCoord, 0));
        float4 materialSample = materialTex.Load(int3(pixelCoord, 0));
        float4 emissionSample = emissionTex.Load(int3(pixelCoord, 0));

        // Fetch Decoupled RT Shadow Mask (computed in Pass 2.5 / 2.6 for primary sun)
        float shadowFactor = 1.0;
        if (ubo->enableRTShadows != 0 && pushConstant.shadowMaskTextureIndex != 0 && pushConstant.shadowMaskTextureIndex != 0xFFFFFFFF)
        {
            Texture2D shadowMaskTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.shadowMaskTextureIndex, 0));
            float2 shadowData = shadowMaskTex.Load(int3(pixelCoord, 0)).rg;
            if ((pushConstant.nrdShadowsEnabled & 1) != 0)
            {
                float s = shadowData.r;
                shadowFactor = clamp(s * s, 0.0, 1.0);
            }
            else
            {
                shadowFactor = clamp(shadowData.g, 0.0, 1.0);
            }
        }

        // 3. Reconstruct World Position from Depth
        float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
        float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

        float4 ndc = float4(ndc_xy, depth, 1.0);
        float4 worldPosH = mul(pushConstant.invViewProj, ndc);
        float3 worldPos = worldPosH.xyz / worldPosH.w;

        // 4. Material Attributes
        float3 baseColor = albedoSample.rgb;
        float occlusion = albedoSample.a;
        float lenSqN = dot(normalSample.rgb, normalSample.rgb);
            float3 n = lenSqN > 1e-6 ? normalSample.rgb * rsqrt(lenSqN) : float3(0.0, 1.0, 0.0);
        float perceptualRoughness = clamp(materialSample.r, c_MinRoughness, 1.0);
        float metallic = clamp(materialSample.g, 0.0, 1.0);
        float alphaRoughness = perceptualRoughness * perceptualRoughness;

        // --- UNLIT MATERIAL CHECK ---
        // If material is UNLIT (workflow == 2.0), bypass all lighting and output base color directly!
        if (materialSample.b >= 2.0)
        {
            TonemapParameters tonemapParams;
            tonemapParams.exposure = ubo->exposure;
            tonemapParams.gamma = ubo->gamma;
            tonemapParams.mode = (int)TonemapMode::KhronosPbrNeutral;
            return float4(toneMap(baseColor, tonemapParams), 1.0);
        }

        float3 f0 = float3(0.04);
        float3 diffuseColor = baseColor * (float3(1.0) - f0) * (1.0 - metallic);
        float3 specularColor = lerp(f0, baseColor, metallic);
        float reflectance = max(max(specularColor.r, specularColor.g), specularColor.b);
        float3 specularEnvironmentR0 = specularColor;
        float3 specularEnvironmentR90 = clamp(float3(reflectance * 50.0), 0.0, 1.0);

        // 5. Vectors
        float3 v = normalize(ubo->cameraWorldPos.xyz - worldPos);
        float NdotV = clamp(dot(n, v), 0.001, 1.0);
        float3 reflection = normalize(reflect(-v, n));

        // 6. Direct Analytical Lighting (Directional, Point, Spot) -- or ReSTIR DI's resampled
        // result, which shadow-rays every light regardless of count instead of only light index 0
        // (see the brute-force loop below: only `i == 0` ever gets a real shadow term).
        float3 directLighting = float3(0.0);
        if (pushConstant.directLightingMode == 1 && pushConstant.restirDIDiffuseTextureIndex != 0xFFFFFFFF)
        {
            // De-modulated diffuse and specular (ReSTIRDIFinalShading.slang): modulated back with the same factors.
            Texture2D diffuseDITex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.restirDIDiffuseTextureIndex, 0));
            Texture2D specularDITex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.restirDISpecularTextureIndex, 0));
            float3 diffFactor, specFactor;
            NRD_MaterialFactors(n, v, diffuseColor, specularColor, perceptualRoughness, diffFactor, specFactor);
            directLighting = NRD_UnpackSignal(pushConstant.restirDIDenoiserMode, diffuseDITex.Load(int3(pixelCoord, 0))) * diffFactor +
                             NRD_UnpackSignal(pushConstant.restirDIDenoiserMode, specularDITex.Load(int3(pixelCoord, 0))) * specFactor;
        }
        else
        {
            LightData* lights = (LightData*)ubo->lightDataReference;

            for (uint i = 0; i < ubo->lightCount; i++)
            {
                LightData light = lights[i];
                uint lightType = (uint)light.position.w;

                float3 l;
                float attenuation = 1.0;
                float maxDist = 1e4;

                if (lightType == 0) // Directional Light
                {
                    l = normalize(light.direction.xyz);
                    maxDist = 1e4;
                }
                else // Point (1) or Spot (2)
                {
                    float3 lightVec = light.position.xyz - worldPos;
                    float distance = length(lightVec);
                    l = distance > 0.0001 ? lightVec / distance : float3(0.0, 1.0, 0.0);
                    maxDist = distance;

                    float range = light.direction.w;
                    if (range > 0.0)
                    {
                        float dOverR = distance / range;
                        float window = max(1.0 - pow(dOverR, 4.0), 0.0);
                        attenuation = window / (distance * distance);
                    }
                    else
                    {
                        attenuation = 1.0 / (distance * distance);
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
                {
                    continue;
                }

                float NdotL_raw = dot(n, l);
                bool isDoubleSided = (materialSample.a > 0.5);
                if (isDoubleSided && NdotL_raw < 0.0)
                {
                    // Translucent thin cloth / foliage: sun transmits through to the back face
                    NdotL_raw = -NdotL_raw * 0.6;
                }

                if (NdotL_raw <= 0.0)
                {
                    continue;
                }

                float lightShadow = 1.0;
                if (ubo->enableRTShadows != 0 && ubo->tlasDeviceAddress != 0)
                {
                    if (i == 0 && pushConstant.shadowMaskTextureIndex != 0 && pushConstant.shadowMaskTextureIndex != 0xFFFFFFFF)
                    {
                        // Primary directional light uses the decoupled shadow mask / NRD SIGMA if available
                        lightShadow = shadowFactor;
                    }
                    else if (lightType == 0 || (attenuation * light.color.w > 0.001))
                    {
                        // Any additional directional lights (moon, secondary suns) or any punctual light within range
                        float3 shadowNormal = (dot(n, l) < 0.0) ? -n : n;
                        float3 shadowOrigin = worldPos + shadowNormal * 0.02;
                        if (trace_shadow_ray(ubo, shadowOrigin, l, maxDist))
                        {
                            lightShadow = 0.0;
                        }
                    }
                }

                float3 h = normalize(l + v);

                float NdotL = clamp(NdotL_raw, 0.001, 1.0);
                float NdotH = clamp(dot(n, h), 0.0, 1.0);
                float LdotH = clamp(dot(l, h), 0.0, 1.0);
                float VdotH = clamp(dot(v, h), 0.0, 1.0);

                PBRInfo pbrInputs = PBRInfo(
                    NdotL,
                    NdotV,
                    NdotH,
                    LdotH,
                    VdotH,
                    perceptualRoughness,
                    metallic,
                    specularEnvironmentR0,
                    specularEnvironmentR90,
                    alphaRoughness,
                    diffuseColor,
                    specularColor
                );

                float3 lightRadiance = light.color.rgb * light.color.w * attenuation * lightShadow;
                directLighting += evaluateDirectLight(pbrInputs, lightRadiance);
            }
        }

        // 7. Image-Based Lighting (IBL) & Decoupled RT Reflections
        PBRInfo iblInputs = PBRInfo(
            1.0,
            NdotV,
            1.0,
            1.0,
            1.0,
            perceptualRoughness,
            metallic,
            specularEnvironmentR0,
            specularEnvironmentR90,
            alphaRoughness,
            diffuseColor,
            specularColor
        );
        IBLResult ibl = getIBLContribution(ubo, iblInputs, n, reflection);
        float3 ambientDiffuse = ibl.diffuse;
        float3 ambientSpecular = ibl.specular;

        float specularOcclusion = 1.0;
        // Diffuse Global Illumination: ReSTIR GI (Screen-Space Resampling) or DDGI Probes
        if ((pushConstant.diffuseGIMode == 2 || pushConstant.debugMode == 16) && pushConstant.restirGIDiffuseTextureIndex != 0xFFFFFFFF)
        {
            Texture2D restirTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.restirGIDiffuseTextureIndex, 0));
            float3 restirDiffuse = NRD_UnpackSignal(pushConstant.restirGIDenoiserMode, restirTex.Load(int3(pixelCoord, 0)));
            ambientDiffuse = restirDiffuse * diffuseColor;

            // Prevent outdoor skybox specular from leaking onto dark indoor surfaces
            float restirLuma = dot(restirDiffuse, float3(0.2126, 0.7152, 0.0722));
            float skyLuma = max(ubo->scaleIBLAmbient, 0.01);
            specularOcclusion = saturate(restirLuma / skyLuma + 0.15);
            ambientSpecular *= specularOcclusion;
        }
        else if ((pushConstant.diffuseGIMode == 1 || ubo->enableDDGI != 0) && ubo->ddgiIrradianceTextureIndex != 0xFFFFFFFF && ubo->ddgiDistanceTextureIndex != 0xFFFFFFFF)
        {
            Texture2D ddgiIrrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiIrradianceTextureIndex, 0));
            Texture2D ddgiDistTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiDistanceTextureIndex, 0));
            SamplerState ddgiSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));

            float3 ddgiIrradiance = sample_ddgi_diffuse(ubo, worldPos, n, ddgiIrrTex, ddgiDistTex, ddgiSampler);
            ambientDiffuse = ddgiIrradiance * diffuseColor;

            // Prevent outdoor skybox specular from leaking onto dark indoor surfaces
            float ddgiLuma = dot(ddgiIrradiance, float3(0.2126, 0.7152, 0.0722));
            float skyLuma = max(ubo->scaleIBLAmbient, 0.01);
            specularOcclusion = saturate(ddgiLuma / skyLuma);
            ambientSpecular *= specularOcclusion;
        }

        // Sample Decoupled RT Reflection (Denoised via NRD REBLUR/RELAX or Raw 1-SPP)
        float3 reflColor = float3(0.0);
        uint reflDenoiserMode = (pushConstant.nrdShadowsEnabled >> 1) & 3;
        float maxRoughness = (reflDenoiserMode != 0) ? 0.85 : 0.55;
        bool hasRTReflection = (ubo->enableRTReflections != 0 && perceptualRoughness < maxRoughness && ubo->brdfLutIndex != 0xFFFFFFFF);
        if (hasRTReflection)
        {
            Texture2D reflTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.reflectionTextureIndex, 0));
            float4 reflSample = reflTex.Load(int3(pixelCoord, 0));
            reflColor = NRD_UnpackSignal(reflDenoiserMode, reflSample);

            SamplerState samplerBrdf = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_BRDFLUT, 0));
            Texture2D brdfTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->brdfLutIndex, 0));
            float3 brdf = brdfTex.Sample(samplerBrdf, float2(NdotV, 1.0 - perceptualRoughness)).rgb;

            // Specular Fresnel factor: only ~0.04 (4%) for dielectrics (plants, stone), ~0.9-1.0 for metals
            float3 specularFactor = specularColor * brdf.x + brdf.y;
            float3 rtSpecular = reflColor * specularFactor;

            // If the reflection ray missed geometry (sampled outdoor skybox) and we are indoors / in dark area,
            // attenuate the sky reflection by local specular occlusion!
            if (reflSample.a >= 1000.0)
            {
                rtSpecular *= specularOcclusion;
            }

            // Smooth fade out into environment IBL as roughness reaches maxRoughness
            float rtWeight = saturate((maxRoughness - perceptualRoughness) / 0.15);
            ambientSpecular = lerp(ambientSpecular, rtSpecular, rtWeight);
        }

        float3 iblColor = ambientDiffuse + ambientSpecular;

        // =========================================================================
        // DEBUG OUTPUT SELECTION
        // =========================================================================
        // Mode 1: Base Color (Matching Sascha Willems Inputs -> Base color)
        if (pushConstant.debugMode == 1 || pushConstant.debugMode == 20 || pushConstant.debugMode == 21) // 20/21: colors written by the G-buffer
        {
            return float4(albedoSample.rgb, 1.0);
        }

        // Mode 2: Normal Input Texture (Matching Sascha Willems Inputs -> Normal)
        // Renders the raw periwinkle blue/purple normal map texture with scratches!
        if (pushConstant.debugMode == 2)
        {
            float3 normInput = normalSample.rgb;
            // Sascha Willems material_pbr.frag:367 applies SRGBtoLINEAR(outColor)
            return float4(pow(normInput, 2.2), 1.0);
        }

        // Mode 3: Occlusion (Matching Sascha Willems Inputs -> Occlusion)
        if (pushConstant.debugMode == 3)
        {
            float ao = albedoSample.a;
            return float4(pow(ao, 2.2), pow(ao, 2.2), pow(ao, 2.2), 1.0);
        }

        // Mode 4: Emissive (Matching Sascha Willems Inputs -> Emissive)
        if (pushConstant.debugMode == 4)
        {
            return float4(emissionSample.rgb, 1.0);
        }

        // Mode 5: Metallic (Matching Sascha Willems Inputs -> Metallic)
        if (pushConstant.debugMode == 5)
        {
            float m = materialSample.g;
            return float4(pow(m, 2.2), pow(m, 2.2), pow(m, 2.2), 1.0);
        }

        // Mode 6: Roughness (Matching Sascha Willems Inputs -> Roughness)
        if (pushConstant.debugMode == 6)
        {
            float r = materialSample.r;
            return float4(pow(r, 2.2), pow(r, 2.2), pow(r, 2.2), 1.0);
        }

        // Mode 7: Perturbed World Shading Normal (3D Directional Rainbow Sphere)
        if (pushConstant.debugMode == 7)
        {
            float3 n = normalize(normalSample.rgb);
            return float4(n * 0.5 + 0.5, 1.0);
        }

        // Mode 8: Direct Analytical Lights Only (HDR)
        if (pushConstant.debugMode == 8)
        {
            return float4(directLighting, 1.0);
        }

        // Mode 9: IBL (Ambient) Only (HDR)
        if (pushConstant.debugMode == 9)
        {
            return float4(iblColor, 1.0);
        }

        // Mode 10: Reconstructed World Position
        if (pushConstant.debugMode == 10)
        {
            return float4(frac(worldPos), 1.0);
        }

        // Mode 11: Entity ID
        if (pushConstant.debugMode == 11)
        {
            Texture2D<int4> entityTex = Texture2D<int4>.Handle(uint2(ubo->imageHeapIndexOffset + ubo->entityGBufferTextureIndex, 0));
            int entityID = entityTex.Load(int3(pixelCoord, 0)).r;
            return float4(idToColor(entityID), 1.0);
        }

        // Mode 13: RT Shadow Mask (White = Lit, Black = Shadowed)
        if (pushConstant.debugMode == 13)
        {
            return float4(shadowFactor, shadowFactor, shadowFactor, 1.0);
        }

        // Mode 14: RT Reflections Debug View (Hit Radiance or Denoised Reflection)
        if (pushConstant.debugMode == 14)
        {
            return float4(reflColor, 1.0);
        }

        // Mode 16: Indirect Diffuse GI Only (DDGI Probes)
        if (pushConstant.debugMode == 16)
        {
            return float4(ambientDiffuse, 1.0);
        }

        // =========================================================================
        // Mode 0: FULL PBR LIT SCENE (Direct + IBL + AO + Emission + Reflections)
        // =========================================================================
        float3 finalColor = directLighting + iblColor;
        finalColor = lerp(finalColor, finalColor * occlusion, 1.0);
        finalColor += emissionSample.rgb;

        return float4(finalColor, 1.0);
    }
