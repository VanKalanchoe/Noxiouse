#include "shaderIO.h"

    struct ShadowPayload
    {
        uint dummy;
    };
    groupshared ShadowPayload gPayload;

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
        uniform PushConstantShadowMask pushConstant
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

    static const float M_PI = 3.141592653589793;
    static const float GOLDEN_ANGLE = 2.399963229728653;

    float2 intersection_uv(InstanceLUT lut, uint primIndex, float2 barycentrics)
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

    bool trace_shadow_ray(UniformBufferObject* ubo, float3 rayOrigin, float3 rayDir, float maxDist, out float hitT)
    {
        hitT = maxDist;
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

        InstanceLUT* instanceLUTBuffer = (InstanceLUT*)ubo->instanceLUTReference;
        SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

        while (sq.Proceed())
        {
            uint instanceID = sq.CandidateRayInstanceCustomIndex();
            uint primIndex = sq.CandidatePrimitiveIndex();

            InstanceLUT lut = instanceLUTBuffer[instanceID];

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

        bool hit = (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT);
        if (hit)
        {
            hitT = sq.CommittedRayT();
        }
        return hit;
    }

    float interleaved_gradient_noise(float2 pixelCoord, uint frameIndex)
    {
        float2 p = pixelCoord + float(frameIndex % 64) * float2(5.588238, 3.45367);
        return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
    }

    float2 sample_vogel_disk(uint sampleIndex, uint sampleCount, float phiOffset, float rOffset)
    {
        float r = sqrt((float(sampleIndex) + rOffset) / float(sampleCount));
        float theta = float(sampleIndex) * GOLDEN_ANGLE + phiOffset * (2.0 * M_PI);
        return float2(r * cos(theta), r * sin(theta));
    }

    void build_orthonormal_basis(float3 n, out float3 b1, out float3 b2)
    {
        float sign = n.z >= 0.0 ? 1.0 : -1.0;
        float a = -1.0 / (sign + n.z);
        float b = n.x * n.y * a;
        b1 = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
        b2 = float3(b, sign + n.y * n.y * a, -n.y);
    }

    float3 encode_normal_roughness_101010(float3 n, float roughness)
    {
        n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);

        float3 r;
        r.y = n.y * 0.5 + 0.5;
        r.x = n.x * 0.5 + r.y;
        r.y -= n.x * 0.5;

        roughness = clamp(roughness, 1.5 / 512.0, 1.0);
        float s = n.z < 0.0 ? -roughness : roughness;
        r.z = s * 0.5 + 0.5;

        return r;
    }

    static const float NRD_FP16_MAX = 65504.0;

    struct ShadowMaskOutput
    {
        float2 shadowData         : SV_Target0; // NRD IN_PENUMBRA in R, raw visibility in G
        float  viewZ              : SV_Target1; // linear view depth (positive)
        float4 nrdNormalRoughness : SV_Target2; // NRD R10_G10_B10_A2_UNORM oct-packed normal + roughness
    };

    [shader("fragment")]
    ShadowMaskOutput fragMain(
        MSOutput input,
        uniform PushConstantShadowMask pushConstant
    )
    {
        ShadowMaskOutput output;
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        int2 pixelCoord = int2(input.pos.xy);

        Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
        float rawDepth = depthTex.Load(int3(pixelCoord, 0)).r;

        // Reverse-Z: 0.0 is sky/background -> fully lit, maximum distance, infinite viewZ
        if (rawDepth <= 0.0)
        {
            output.shadowData = float2(NRD_FP16_MAX, 1.0);
            output.viewZ = 500000.0;
            output.nrdNormalRoughness = float4(encode_normal_roughness_101010(float3(0.0, 1.0, 0.0), 1.0), 0.0);
            return output;
        }

        float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
        float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        float4 ndc = float4(ndc_xy, rawDepth, 1.0);
        float4 worldPosH = mul(pushConstant.invViewProj, ndc);
        float3 worldPos = worldPosH.xyz / worldPosH.w;

        // Linear View-Z (depth from camera along view direction)
        float4 viewPosH = mul(ubo->view, float4(worldPos, 1.0));
        output.viewZ = abs(viewPosH.z);

        Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
        float4 normalSample = normalTex.Load(int3(pixelCoord, 0));
        float lenSqN = dot(normalSample.rgb, normalSample.rgb);
        float3 normal = lenSqN > 1e-6 ? normalSample.rgb * rsqrt(lenSqN) : float3(0.0, 1.0, 0.0);
        float roughness = clamp(normalSample.a, 0.01, 1.0);
        output.nrdNormalRoughness = float4(encode_normal_roughness_101010(normal, roughness), 0.0);

        if (ubo->lightCount == 0 || ubo->enableRTShadows == 0 || ubo->tlasDeviceAddress == 0)
        {
            output.shadowData = float2(NRD_FP16_MAX, 1.0);
            return output;
        }

        LightData* lights = (LightData*)ubo->lightDataReference;
        LightData light = lights[0]; // Primary shadow caster (Directional / Sun)

        uint lightType = (uint)light.position.w;
        float sourceRadius = light.spotParams.z;
        uint numSamples = max(1u, (uint)light.spotParams.w);

        float3 unjitteredLightDir;
        float unjitteredMaxDist = 1e4;

        if (lightType == 0) // Directional
        {
            unjitteredLightDir = normalize(light.direction.xyz);
        }
        else // Point / Spot
        {
            float3 lightVec = light.position.xyz - worldPos;
            float dist = length(lightVec);
            unjitteredLightDir = dist > 1e-4 ? lightVec / dist : float3(0, 1, 0);
            unjitteredMaxDist = dist;
        }

        // Backfacing surface relative to light direction: 0.0 distance to occluder (NRD rule: NoL <= 0 is 0.0)
        float NoL = dot(normal, unjitteredLightDir);
        if (NoL <= 0.0)
        {
            output.shadowData = float2(0.0, 0.0);
            return output;
        }

        float3 rayOrigin = worldPos + normal * 0.02;

        if (sourceRadius <= 1e-4)
        {
            float hitT;
            bool hit = trace_shadow_ray(ubo, rayOrigin, unjitteredLightDir, unjitteredMaxDist, hitT);
            float penumbra = hit ? 0.0 : NRD_FP16_MAX;
            float rawVis = hit ? 0.0 : 1.0;
            output.shadowData = float2(penumbra, rawVis);
            return output;
        }

        float ign = interleaved_gradient_noise(float2(pixelCoord), pushConstant.frameIndex);
        float phiOffset = ign;
        float rOffset = frac(ign * 1.6180339887 + 0.314159);

        float hitT;
        float3 rayDir;
        float maxDist = unjitteredMaxDist;

        if (lightType == 0) // Directional (Cone sampling)
        {
            float tanRadius = tan(sourceRadius);
            float3 T, B;
            build_orthonormal_basis(unjitteredLightDir, T, B);
            float2 disk = sample_vogel_disk(0, 1, phiOffset, rOffset);
            rayDir = normalize(unjitteredLightDir + (disk.x * T + disk.y * B) * tanRadius);
        }
        else // Point / Spot (Disk sampling)
        {
            float3 lightCenterPos = light.position.xyz;
            float3 lightVec = lightCenterPos - worldPos;
            float lightDist = length(lightVec);
            float3 centerDir = lightDist > 1e-4 ? lightVec / lightDist : float3(0, 1, 0);

            float3 T, B;
            build_orthonormal_basis(centerDir, T, B);
            float2 disk = sample_vogel_disk(0, 1, phiOffset, rOffset);
            float3 samplePosOnLight = lightCenterPos + (disk.x * T + disk.y * B) * sourceRadius;

            float3 toSample = samplePosOnLight - rayOrigin;
            float sampleDist = length(toSample);
            rayDir = sampleDist > 1e-4 ? toSample / sampleDist : centerDir;
            maxDist = sampleDist;
        }

        if (dot(normal, rayDir) <= 0.001)
        {
            output.shadowData = float2(0.0, 0.0);
            return output;
        }

        bool hit = trace_shadow_ray(ubo, rayOrigin, rayDir, maxDist, hitT);
        float penumbra;
        if (hit)
        {
            if (lightType == 0)
            {
                float tanRadius = tan(sourceRadius);
                float penumbraSize = hitT * tanRadius;
                penumbra = min(penumbraSize * 0.5, 32768.0);
            }
            else
            {
                float penumbraSize = sourceRadius * hitT / max(maxDist - hitT, 1e-4);
                penumbra = min(penumbraSize * 0.5, 32768.0);
            }
        }
        else
        {
            penumbra = NRD_FP16_MAX;
        }

        float rawVis = hit ? 0.0 : 1.0;
        output.shadowData = float2(penumbra, rawVis);
        return output;
    }