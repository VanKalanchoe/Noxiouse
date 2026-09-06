#include "shaderIO.h"

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

    struct PBRInfo
    {
        float NdotL;                  // cos angle between normal and light direction
        float NdotV;                  // cos angle between normal and view direction
        float NdotH;                  // cos angle between normal and half vector
        float LdotH;                  // cos angle between light direction and half vector
        float VdotH;                  // cos angle between view direction and half vector
        float perceptualRoughness;    // roughness value, as authored by the model creator
        float metalness;              // metallic value at the surface
        float3 reflectance0;          // full reflectance color (normal incidence angle)
        float3 reflectance90;         // reflectance color at grazing angle
        float alphaRoughness;         // roughness mapped to a more linear change
        float3 diffuseColor;          // color contribution from diffuse lighting
        float3 specularColor;         // color contribution from specular lighting
    };

    static const float M_PI = 3.141592653589793;
    static const float c_MinRoughness = 0.04;

    // Basic Lambertian diffuse
    float3 diffuse(PBRInfo pbrInputs)
    {
        return pbrInputs.diffuseColor / M_PI;
    }

    // Fresnel reflectance (Schlick)
    float3 specularReflection(PBRInfo pbrInputs)
    {
        return pbrInputs.reflectance0 + (pbrInputs.reflectance90 - pbrInputs.reflectance0) * pow(clamp(1.0 - pbrInputs.VdotH, 0.0, 1.0), 5.0);
    }

    // Geometric occlusion (Smith GGX correlated)
    float geometricOcclusion(PBRInfo pbrInputs)
    {
        float NdotL = pbrInputs.NdotL;
        float NdotV = pbrInputs.NdotV;
        float r = pbrInputs.alphaRoughness;

        float attenuationL = 2.0 * NdotL / (NdotL + sqrt(r * r + (1.0 - r * r) * (NdotL * NdotL)));
        float attenuationV = 2.0 * NdotV / (NdotV + sqrt(r * r + (1.0 - r * r) * (NdotV * NdotV)));
        return attenuationL * attenuationV;
    }

    // Normal distribution (GGX Trowbridge-Reitz)
    float microfacetDistribution(PBRInfo pbrInputs)
    {
        float roughnessSq = pbrInputs.alphaRoughness * pbrInputs.alphaRoughness;
        float f = (pbrInputs.NdotH * roughnessSq - pbrInputs.NdotH) * pbrInputs.NdotH + 1.0;
        return roughnessSq / (M_PI * f * f);
    }

    // Image Based Lighting (IBL)
    float3 getIBLContribution(UniformBufferObject* ubo, PBRInfo pbrInputs, float3 n, float3 reflection)
    {
        float lod = (pbrInputs.perceptualRoughness * ubo->prefilteredCubeMipLevels);

        SamplerState samplerBrdf = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_BRDFLUT, 0));
        SamplerState samplerIrradiance = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));

        // Sample BRDF LUT (2D Texture from heap)
        Texture2D brdfTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->brdfLutIndex, 0));
        float3 brdf = brdfTex.Sample(samplerBrdf, float2(pbrInputs.NdotV, 1.0 - pbrInputs.perceptualRoughness)).rgb;

        // Sample Irradiance Map (Cube Texture from heap)
        TextureCube irradianceTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        float4 irradianceSample = irradianceTex.Sample(samplerIrradiance, n);
        float3 diffuseLight = irradianceSample.rgb;

        // Sample Prefiltered Environment Map with Mip Level LOD (Cube Texture from heap)
        TextureCube prefilteredTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
        float4 prefilteredSample = prefilteredTex.SampleLevel(samplerPrefiltered, reflection, lod);
        float3 specularLight = prefilteredSample.rgb;

        float3 diffuse = diffuseLight * pbrInputs.diffuseColor;
        float3 specular = specularLight * (pbrInputs.specularColor * brdf.x + brdf.y);

        diffuse *= ubo->scaleIBLAmbient;
        specular *= ubo->scaleIBLAmbient;

        return diffuse + specular;
    }

    [shader("fragment")]
    float4 fragMain(
        MSOutput input,
        uniform PushConstantDeferredLighting pushConstant
    ) : SV_Target0
    {
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        int2 pixelCoord = int2(input.pos.xy);

        // 1. Check Visibility Buffer to distinguish background from geometry
        Texture2D<uint4> visTex = Texture2D<uint4>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.visibilityTextureIndex, 0));
        uint4 rawVis = visTex.Load(int3(pixelCoord, 0));
        uint2 visData = rawVis.xy;

       if (visData.x == 0)
        {
            // When inspecting material debug modes (Albedo, Normals, Roughness), show neutral dark gray
            if (pushConstant.debugMode != 0)
            {
                return float4(0.12, 0.12, 0.18, 1.0);
            }
            // In full PBR lit mode (0), discard so the Skybox pass can render into the background!
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

        // 3. Reconstruct World Position from Depth
        float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
        float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

        Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
        float depth = depthTex.Load(int3(pixelCoord, 0)).r;

        float4 ndc = float4(ndc_xy, depth, 1.0);
        float4 worldPosH = mul(pushConstant.invViewProj, ndc);
        float3 worldPos = worldPosH.xyz / worldPosH.w;

        // 4. Material Attributes
        float3 baseColor = albedoSample.rgb;
        float occlusion = albedoSample.a;
        float3 n = normalize(normalSample.rgb);
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
        float NdotV = clamp(abs(dot(n, v)), 0.001, 1.0);
        float3 reflection = -normalize(reflect(v, n));

        // 6. Direct Analytical Lighting (Directional, Point, Spot)
        float3 directLighting = float3(0.0);
        LightData* lights = (LightData*)ubo->lightDataReference;

        for (uint i = 0; i < ubo->lightCount; i++)
        {
            LightData light = lights[i];
            uint lightType = (uint)light.position.w;

            float3 l;
            float attenuation = 1.0;

            if (lightType == 0) // Directional Light
            {
                l = normalize(light.direction.xyz);
            }
            else // Point (1) or Spot (2)
            {
                float3 lightVec = light.position.xyz - worldPos;
                float distance = length(lightVec);
                l = distance > 0.0001 ? lightVec / distance : float3(0.0, 1.0, 0.0);

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

            float3 h = normalize(l + v);

            float NdotL = clamp(dot(n, l), 0.001, 1.0);
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

            float3 F = specularReflection(pbrInputs);
            float G = geometricOcclusion(pbrInputs);
            float D = microfacetDistribution(pbrInputs);

            float3 diffuseContrib = (1.0 - F) * diffuse(pbrInputs);
            float3 specContrib = F * G * D / (4.0 * NdotL * NdotV);

            float3 lightRadiance = light.color.rgb * light.color.w * attenuation;
            directLighting += NdotL * lightRadiance * (diffuseContrib + specContrib);
        }

        // 7. Image-Based Lighting (IBL)
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
        float3 iblColor = getIBLContribution(ubo, iblInputs, n, reflection);

        // =========================================================================
        // DEBUG OUTPUT SELECTION
        // =========================================================================
        // Mode 1: Base Color (Matching Sascha Willems Inputs -> Base color)
        if (pushConstant.debugMode == 1)
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

        // Mode 8: Direct Analytical Lights Only
        if (pushConstant.debugMode == 8)
        {
            TonemapParameters tonemapParams;
            tonemapParams.exposure = ubo->exposure;
            tonemapParams.gamma = ubo->gamma;
            tonemapParams.mode = (int)TonemapMode::KhronosPbrNeutral;
            return float4(toneMap(directLighting, tonemapParams), 1.0);
        }

        // Mode 9: IBL (Ambient) Only
        if (pushConstant.debugMode == 9)
        {
            TonemapParameters tonemapParams;
            tonemapParams.exposure = ubo->exposure;
            tonemapParams.gamma = ubo->gamma;
            tonemapParams.mode = (int)TonemapMode::KhronosPbrNeutral;
            return float4(toneMap(iblColor, tonemapParams), 1.0);
        }

        // Mode 10: Reconstructed World Position
        if (pushConstant.debugMode == 10)
        {
            return float4(frac(worldPos), 1.0);
        }

        // =========================================================================
        // Mode 0: FULL PBR LIT SCENE (Direct + IBL + AO + Emission + Tonemap)
        // =========================================================================
        float3 finalColor = directLighting + iblColor;
        finalColor = lerp(finalColor, finalColor * occlusion, 1.0);
        finalColor += emissionSample.rgb;

        TonemapParameters tonemapParams;
        tonemapParams.exposure = ubo->exposure;
        tonemapParams.gamma = ubo->gamma;
        tonemapParams.mode = (int)TonemapMode::KhronosPbrNeutral;
        finalColor = toneMap(finalColor, tonemapParams);

        return float4(finalColor, 1.0);
    }