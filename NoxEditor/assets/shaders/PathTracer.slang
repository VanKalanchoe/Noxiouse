#include "shaderIO.h"

// NRD REBLUR YCoCg conversion (matching NRD.hlsli:398, same fix already applied to ReSTIR GI/DI) --
// REBLUR's internal filtering assumes IN_DIFF_RADIANCE_HITDIST is already YCoCg-encoded; RELAX takes
// plain linear RGB directly, no conversion needed.
float3 LinearToYCoCg(float3 color)
{
    float Y = dot(color, float3(0.25, 0.5, 0.25));
    float Co = dot(color, float3(0.5, 0.0, -0.5));
    float Cg = dot(color, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
}

    struct PTPayload
    {
        uint dummy;
    };
    groupshared PTPayload gPayload;

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
        uniform PushConstantPathTracer pushConstant
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

    // High-quality PCG Random Number Generator
    uint pcg_hash(inout uint state)
    {
        state = state * 747796405u + 2891336453u;
        uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return (word >> 22u) ^ word;
    }

    float rand(inout uint state)
    {
        return float(pcg_hash(state)) / 4294967296.0;
    }

    // Duff et al. orthonormal basis construction
    void build_orthonormal_basis(float3 n, out float3 b1, out float3 b2)
    {
        float sign = n.z >= 0.0 ? 1.0 : -1.0;
        float a = -1.0 / (sign + n.z);
        float b = n.x * n.y * a;
        b1 = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
        b2 = float3(b, sign + n.y * n.y * a, -n.y);
    }

    // Cosine-weighted hemisphere sampling (Malley's method)
    float3 sample_cosine_hemisphere(float u1, float u2, float3 normal)
    {
        float r = sqrt(u1);
        float theta = 2.0 * M_PI * u2;
        float x = r * cos(theta);
        float y = r * sin(theta);
        float z = sqrt(max(0.0, 1.0 - u1));

        float3 tangent, bitangent;
        build_orthonormal_basis(normal, tangent, bitangent);
        return normalize(tangent * x + bitangent * y + normal * z);
    }

    // GGX Visible Normal Distribution Function (VNDF) Importance Sampling (Heitz 2018)
    float3 sample_ggx_vndf(float3 V, float3 N, float roughness, float2 u)
    {
        float alpha = max(roughness * roughness, 0.001);

        // 1. Build orthonormal basis around macro normal N
        float3 T, B;
        build_orthonormal_basis(N, T, B);

        // 2. Transform V into tangent frame
        float3 V_local = float3(dot(V, T), dot(V, B), dot(V, N));

        // 3. Stretch view vector
        float3 Vh = normalize(float3(alpha * V_local.x, alpha * V_local.y, V_local.z));

        // 4. Orthonormal basis in stretched space
        float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
        float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
        float3 T2 = cross(Vh, T1);

        // 5. Parameterization of projected hemisphere
        float r = sqrt(u.x);
        float phi = 2.0 * M_PI * u.y;
        float t1 = r * cos(phi);
        float t2 = r * sin(phi);
        float s = 0.5 * (1.0 + Vh.z);
        t2 = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * t2;

        // 6. Reprojection onto hemisphere
        float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;

        // 7. Unstretch to get microfacet normal in tangent frame
        float3 Ne_local = normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));

        // 8. Transform back to world space
        float3 M = Ne_local.x * T + Ne_local.y * B + Ne_local.z * N;
        return normalize(M);
    }

    // Cook-Torrance GGX Evaluation Helpers
    float3 fresnel_schlick(float3 F0, float VdotH)
    {
        return F0 + (float3(1.0) - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
    }

    float eval_ndf_ggx(float NdotH, float alphaSq)
    {
        float f = (NdotH * alphaSq - NdotH) * NdotH + 1.0;
        return alphaSq / max(M_PI * f * f, 1e-7);
    }

    float eval_smith_g1(float NdotV, float alphaSq)
    {
        return 2.0 * NdotV / max(NdotV + sqrt(alphaSq + (1.0 - alphaSq) * (NdotV * NdotV)), 1e-7);
    }

    float eval_smith_g2(float NdotV, float NdotL, float alphaSq)
    {
        return eval_smith_g1(NdotV, alphaSq) * eval_smith_g1(NdotL, alphaSq);
    }

    // Tangent-space normal mapping helper
    float3 perturb_normal(InstanceLUT lut, Vertex v0, Vertex v1, Vertex v2, float3 geomNormal, float2 hitUV, SamplerState textureSampler, UniformBufferObject* ubo)
    {
        if (lut.normalTextureIndex == 0xFFFFFFFF)
            return geomNormal;

        float3 dp1 = mul((float3x3)lut.normalMatrix, v1.pos - v0.pos);
        float3 dp2 = mul((float3x3)lut.normalMatrix, v2.pos - v0.pos);
        float2 duv1 = v1.uv0 - v0.uv0;
        float2 duv2 = v2.uv0 - v0.uv0;

        float det = duv1.x * duv2.y - duv2.x * duv1.y;
        if (abs(det) < 1e-12)
            return geomNormal;

        float3 T = dp1 * duv2.y - dp2 * duv1.y;
        float3 B = dp2 * duv1.x - dp1 * duv2.x;
        float3 N = geomNormal;

        float lenT = length(T);
        if (lenT < 1e-6) return geomNormal;

        T = normalize(T - N * dot(N, T));
        float3 B_ortho = normalize(cross(N, T));
        if (dot(B_ortho, B) < 0.0)
            B_ortho = -B_ortho;

        float3x3 TBN = float3x3(T, B_ortho, N);
        Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.normalTextureIndex, 0));
        float3 tangentNormal = normalTex.SampleLevel(textureSampler, hitUV, 0).xyz * 2.0 - 1.0;
        return normalize(mul(tangentNormal, TBN));
    }

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

    // Ray query shadow test for direct analytical lights (Next Event Estimation)
    bool trace_shadow_ray(UniformBufferObject* ubo, float3 rayOrigin, float3 rayDir, float maxDist)
    {
        if (ubo->tlasDeviceAddress == 0) return false;

        RayDesc shadowRay;
        shadowRay.Origin = rayOrigin;
        shadowRay.Direction = rayDir;
        shadowRay.TMin = 0.01;
        shadowRay.TMax = maxDist;

        RaytracingAccelerationStructure as = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
        sq.TraceRayInline(as, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, shadowRay);

        InstanceLUT* instanceLUTBuffer = (InstanceLUT*)ubo->instanceLUTReference;
        SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

        while (sq.Proceed())
        {
            uint instID = sq.CandidateRayInstanceCustomIndex();
            uint primIndex = sq.CandidatePrimitiveIndex();
            InstanceLUT lut = instanceLUTBuffer[instID];

            if (lut.alphaMode == 0 || lut.baseColorTextureIndex == 0xFFFFFFFF)
            {
                sq.CommitNonOpaqueTriangleHit();
            }
            else
            {
                float2 uv = intersection_uv(lut, primIndex, sq.CandidateTriangleBarycentrics());
                Texture2D tex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.baseColorTextureIndex, 0));
                float4 col = tex.SampleLevel(textureSampler, uv, 0);
                if (col.a >= lut.alphaCutoff)
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
        uniform PushConstantPathTracer pushConstant
    ) : SV_Target0
    {
        int2 pixelCoord = int2(input.pos.xy);
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

        // Decorrelated per-pixel seed using coordinate primes and sample counter
        uint rngState = uint(pixelCoord.x * 1973 + pixelCoord.y * 9277) + pushConstant.frameIndex * 26699;

        // Subpixel jitter for anti-aliasing
        float2 jitter = float2(rand(rngState) - 0.5, rand(rngState) - 0.5);
        float2 uv = (float2(pixelCoord) + 0.5 + jitter) / pushConstant.viewportSize;
        float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

        // Primary pinhole camera ray reconstruction (near plane is depth = 1.0 in Reverse-Z)
        float4 nearPosH = mul(pushConstant.invViewProj, float4(ndc_xy, 1.0, 1.0));
        float3 nearPos = nearPosH.xyz / nearPosH.w;

        float3 rayOrigin = ubo->cameraWorldPos.xyz;
        float3 rayDir = normalize(nearPos - rayOrigin);

        float3 throughput = float3(1.0, 1.0, 1.0);
        float3 radiance = float3(0.0, 0.0, 0.0);
        // Primary ray's hit distance, for NRD's hit-distance-based blur radius (same convention already
        // used for ReSTIR DI/GI's alpha channel) -- 1e4 (the ray's TMax) if the primary ray hit nothing.
        float primaryHitDist = 1e4;

        RaytracingAccelerationStructure as = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
        InstanceLUT* instanceLUTBuffer = (InstanceLUT*)ubo->instanceLUTReference;
        SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

        // Always run full path tracing bounces
        uint maxBounces = max(1u, pushConstant.maxBounces);

        // --- Path Tracing Multi-Bounce Loop ---
        for (uint bounce = 0; bounce < maxBounces; bounce++)
        {
            RayDesc ray;
            ray.Origin = rayOrigin;
            ray.Direction = rayDir;
            ray.TMin = 0.005;
            ray.TMax = 1e4;

            RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
            rq.TraceRayInline(as, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);

            while (rq.Proceed())
            {
                uint instID = rq.CandidateRayInstanceCustomIndex();
                uint primIndex = rq.CandidatePrimitiveIndex();
                InstanceLUT lut = instanceLUTBuffer[instID];

                if (lut.alphaMode == 0 || lut.baseColorTextureIndex == 0xFFFFFFFF)
                {
                    rq.CommitNonOpaqueTriangleHit();
                }
                else
                {
                    float2 uvSample = intersection_uv(lut, primIndex, rq.CandidateTriangleBarycentrics());
                    Texture2D tex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.baseColorTextureIndex, 0));
                    float4 col = tex.SampleLevel(textureSampler, uvSample, 0);
                    if (col.a >= lut.alphaCutoff)
                    {
                        rq.CommitNonOpaqueTriangleHit();
                    }
                }
            }

            // Miss: Sample Environment Map / Skybox
            if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            {
                float3 skyRadiance = float3(0.0);
                if (pushConstant.skyboxTextureIndex != 0xFFFFFFFF)
                {
                    TextureCube skyTex = TextureCube.Handle(uint2(pushConstant.skyboxTextureIndex, 0));
                    SamplerState skySamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
                    skyRadiance = skyTex.SampleLevel(skySamp, rayDir, 0.0).rgb;
                }
                else
                {
                    // Smooth physical sky gradient if no cubemap is loaded
                    skyRadiance = lerp(float3(0.08, 0.08, 0.12), float3(0.4, 0.6, 0.95), saturate(rayDir.y * 0.5 + 0.5));
                }

                // Scale ambient environment light by IBL Ambient Scale
                skyRadiance *= ubo->scaleIBLAmbient;

                radiance += skyRadiance * throughput;
                break;
            }

            // Hit geometry
            float hitT = rq.CommittedRayT();
            if (bounce == 0)
                primaryHitDist = hitT;
            uint committedInstID = rq.CommittedRayInstanceCustomIndex();
            uint committedPrimIndex = rq.CommittedPrimitiveIndex();
            float2 bary = rq.CommittedTriangleBarycentrics();

            InstanceLUT lut = instanceLUTBuffer[committedInstID];
            uint32_t* indices = (uint32_t*)lut.indexBufferAddress;
            Vertex* vertices = (Vertex*)lut.vertexBufferAddress;

            uint i0, i1, i2;
            if (lut.indexBufferAddress != 0)
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

            float w0 = 1.0 - bary.x - bary.y;
            float w1 = bary.x;
            float w2 = bary.y;

            float2 hitUV = w0 * vertices[i0].uv0 + w1 * vertices[i1].uv0 + w2 * vertices[i2].uv0;

            // 1. Interpolate local normal, then transform to TRUE WORLD SPACE
            float3 localNormal = normalize(w0 * vertices[i0].normal + w1 * vertices[i1].normal + w2 * vertices[i2].normal);
            float3 hitNormal = normalize(mul((float3x3)lut.normalMatrix, localNormal));

            // 2. Only flip normal if the material is explicitly DOUBLE-SIDED (e.g. foliage/cloth)
            if (lut.doubleSided != 0 && dot(hitNormal, -rayDir) < 0.0)
            {
                hitNormal = -hitNormal;
            }

            float3 hitPos = ray.Origin + ray.Direction * hitT;

            // Sample Material Base Color
            float3 albedo = lut.baseColorFactor.rgb;
            if (lut.baseColorTextureIndex != 0xFFFFFFFF)
            {
                Texture2D colTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.baseColorTextureIndex, 0));
                albedo *= colTex.SampleLevel(textureSampler, hitUV, 0).rgb;
            }

            // Sample Metallic & Roughness
            float metallic = clamp(lut.metallicFactor, 0.0, 1.0);
            float roughness = clamp(lut.roughnessFactor, 0.04, 1.0);
            if (lut.metallicRoughnessTextureIndex != 0xFFFFFFFF)
            {
                Texture2D mrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + lut.metallicRoughnessTextureIndex, 0));
                float4 mrSample = mrTex.SampleLevel(textureSampler, hitUV, 0);
                roughness = clamp(roughness * mrSample.g, 0.04, 1.0);
                metallic = clamp(metallic * mrSample.b, 0.0, 1.0);
            }

            // Perturb normal with tangent-space normal map
            hitNormal = perturb_normal(lut, vertices[i0], vertices[i1], vertices[i2], hitNormal, hitUV, textureSampler, ubo);

            float alpha = roughness * roughness;
            float alphaSq = alpha * alpha;
            float3 F0 = lerp(float3(0.04), albedo, metallic);
            float3 diffuseColor = albedo * (1.0 - metallic);
            float3 V = -rayDir;
            float NdotV = clamp(dot(hitNormal, V), 0.001, 1.0);

            // Hit surface emission (e.g. Cornell Box ceiling light)
            radiance += lut.emissiveFactor.rgb * lut.emissiveFactor.a * throughput;

            // --- Next Event Estimation (NEE): Direct Lights ---
            LightData* lights = (LightData*)ubo->lightDataReference;
            for (uint i = 0; i < ubo->lightCount; i++)
            {
                LightData light = lights[i];
                uint lightType = (uint)light.position.w;

                float3 lightDir;
                float maxDist = 1e4;
                float attenuation = 1.0;

                float angularDiameter = light.spotParams.z; // Angular radius (radians) for Directional; Radius (meters) for Point/Spot

                if (lightType == 0) // Directional Light (Sun Cone Sampling)
                {
                    float3 unjitteredDir = normalize(light.direction.xyz);
                    if (angularDiameter > 1e-4)
                    {
                        // Sample a random point on the sun disk
                        float u1 = rand(rngState);
                        float u2 = rand(rngState);
                        float r = sqrt(u1) * tan(angularDiameter);
                        float theta = 2.0 * M_PI * u2;

                        float3 T, B;
                        build_orthonormal_basis(unjitteredDir, T, B);
                        lightDir = normalize(unjitteredDir + (cos(theta) * T + sin(theta) * B) * r);
                    }
                    else
                    {
                        lightDir = unjitteredDir;
                    }
                }
                else // Point / Spot Light (Sphere Source Sampling)
                {
                    float3 lightPos = light.position.xyz;
                    if (angularDiameter > 1e-4)
                    {
                        // Jitter light position within source sphere radius
                        float u1 = rand(rngState);
                        float u2 = rand(rngState);
                        float r = sqrt(u1) * angularDiameter;
                        float theta = 2.0 * M_PI * u2;

                        float3 toLightBase = normalize(lightPos - hitPos);
                        float3 T, B;
                        build_orthonormal_basis(toLightBase, T, B);
                        lightPos += (cos(theta) * T + sin(theta) * B) * r;
                    }

                    float3 toLight = lightPos - hitPos;
                    float dist = length(toLight);
                    lightDir = dist > 1e-4 ? toLight / dist : float3(0, 1, 0);
                    maxDist = dist;

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
                        float cd = dot(normalize(light.direction.xyz), -lightDir);
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

                // Always trace shadow rays for direct lights (Next Event Estimation)
                float NdotL = dot(hitNormal, lightDir);
                if (NdotL > 0.001)
                {
                    // Robust offset (0.02) along normal prevents self-shadowing on curved meshes
                    float3 shadowOrigin = hitPos + hitNormal * 0.02;
                    bool inShadow = trace_shadow_ray(ubo, shadowOrigin, lightDir, maxDist);

                    if (!inShadow)
                    {
                        // Smooth terminator falloff: eliminates stepped polygon tessellation rings at grazing angles
                        float terminatorFade = saturate(NdotL * 10.0);
                        float3 lightRadiance = light.color.rgb * light.color.w * attenuation;

                        float3 H = normalize(lightDir + V);
                        float NdotH = clamp(dot(hitNormal, H), 0.0, 1.0);
                        float VdotH = clamp(dot(V, H), 0.0, 1.0);

                        float3 F_direct = fresnel_schlick(F0, VdotH);
                        float3 diffuseContrib = (float3(1.0) - F_direct) * diffuseColor / M_PI;

                        float D = eval_ndf_ggx(NdotH, alphaSq);
                        float G = eval_smith_g2(NdotV, NdotL, alphaSq);
                        float3 specContrib = (F_direct * G * D) / max(4.0 * NdotL * NdotV, 1e-6);

                        float3 bsdf = diffuseContrib + specContrib;
                        radiance += bsdf * lightRadiance * (NdotL * terminatorFade) * throughput;
                    }
                }
            }

            // --- Russian Roulette Ray Termination ---
            if (bounce >= 2)
            {
                float p = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
                if (rand(rngState) > p)
                    break;
                throughput /= p;
            }

            // --- Multi-Lobe Indirect Ray Generation (Transmission vs Specular vs Diffuse) ---
            float u_trans = rand(rngState);
            float transFactor = lut.transmissionFactor;

            // Branch 1: Transmission (Glass / Refraction)
            if (transFactor > 0.0 && u_trans < transFactor)
            {
                float eta = (dot(hitNormal, rayDir) < 0.0) ? (1.0 / 1.5) : (1.5 / 1.0); // Air <-> Glass (IOR 1.5)
                float3 refrNormal = (dot(hitNormal, rayDir) < 0.0) ? hitNormal : -hitNormal;
                float3 refrDir = refract(rayDir, refrNormal, eta);

                if (length(refrDir) > 1e-4)
                {
                    rayOrigin = hitPos - refrNormal * 0.02; // Step through geometry
                    rayDir = normalize(refrDir);
                    throughput *= albedo;
                    continue;
                }
            }

            // Branch 2 & 3: Specular Reflection vs Diffuse Reflection
            float3 F_refl = fresnel_schlick(F0, NdotV);
            float fLum = dot(F_refl, float3(0.2126, 0.7152, 0.0722));
            float dLum = dot(diffuseColor, float3(0.2126, 0.7152, 0.0722));
            float specProb = clamp(fLum / max(fLum + dLum, 1e-5), 0.05, 0.98);

            // On metals, specular probability is near 1.0
            if (metallic > 0.9) specProb = 0.98;

            float u_lobe = rand(rngState);
            float u1 = rand(rngState);
            float u2 = rand(rngState);

            if (u_lobe < specProb)
            {
                // Specular Lobe (GGX VNDF Importance Sampling)
                float3 M = sample_ggx_vndf(V, hitNormal, roughness, float2(u1, u2));
                float3 nextDir = reflect(-V, M);

                if (dot(hitNormal, nextDir) <= 0.0)
                {
                    break;
                }

                float VdotM = clamp(dot(V, M), 0.0, 1.0);
                float NdotL_bounce = clamp(dot(hitNormal, nextDir), 0.0, 1.0);
                float3 F_bounce = fresnel_schlick(F0, VdotM);

                // Exact Heitz 2018 VNDF Monte Carlo weight: (F * G1(L)) / specProb
                float G1_L = eval_smith_g1(NdotL_bounce, alphaSq);
                throughput *= (F_bounce * G1_L) / specProb;

                rayOrigin = hitPos + hitNormal * 0.02;
                rayDir = nextDir;
            }
            else
            {
                // Diffuse Lobe (Cosine-Weighted Hemisphere Sampling)
                float3 nextDir = sample_cosine_hemisphere(u1, u2, hitNormal);
                float NdotL_bounce = clamp(dot(hitNormal, nextDir), 0.0, 1.0);
                float3 F_diffuse = fresnel_schlick(F0, NdotL_bounce);

                // Exact Cosine Importance Sampling weight: (1 - F) * diffuseColor / (1 - specProb)
                throughput *= ((float3(1.0) - F_diffuse) * diffuseColor) / (1.0 - specProb);

                rayOrigin = hitPos + hitNormal * 0.02;
                rayDir = nextDir;
            }
            }

        // Mode 18: 1-SPP Interactive Output
        if (pushConstant.debugMode == 18 || pushConstant.sampleCount <= 1)
        {
            float3 outColor = (pushConstant.denoiserMode == 1) ? LinearToYCoCg(radiance) : radiance;
            return float4(outColor, primaryHitDist);
        }

        // Mode 19: Progressive Accumulation (blends previous samples over time)
        Texture2D accumTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.accumulationTextureIndex, 0));
        float4 prevColor = accumTex.Load(int3(pixelCoord, 0));

        float blendFactor = 1.0 / float(pushConstant.sampleCount);
        float3 accumulated = lerp(prevColor.rgb, radiance, blendFactor);

        float3 outColor = (pushConstant.denoiserMode == 1) ? LinearToYCoCg(accumulated) : accumulated;
        return float4(outColor, primaryHitDist);
    }