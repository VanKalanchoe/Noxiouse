#include "shaderIO.h"
#include "DDGICommon.slang"

struct ReflectionPayload
{
    uint dummy;
};
groupshared ReflectionPayload gPayload;

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
    uniform PushConstantReflection pushConstant
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

// Orthonormal basis construction (Duff et al. 2017)
void build_orthonormal_basis(float3 n, out float3 b1, out float3 b2)
{
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float b = n.x * n.y * a;
    b1 = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    b2 = float3(b, sign + n.y * n.y * a, -n.y);
}

// Interleaved Gradient Noise (Jimenez 2014)
float interleaved_gradient_noise(float2 pixelCoord, uint frameIndex)
{
    float2 p = pixelCoord + float(frameIndex % 64) * float2(5.588238, 3.45367);
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

// NRD REBLUR YCoCg conversion (matching NRD.hlsli:398)
float3 LinearToYCoCg(float3 color)
{
    float Y = dot(color, float3(0.25, 0.5, 0.25));
    float Co = dot(color, float3(0.5, 0.0, -0.5));
    float Cg = dot(color, float3(-0.25, 0.5, -0.25));
    return float3(Y, Co, Cg);
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
    float phi = 2.0 * 3.14159265359 * u.y;
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

// Intersection UV interpolation
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
    float2 uv0 = vertices[i0].uv0;
    float2 uv1 = vertices[i1].uv0;
    float2 uv2 = vertices[i2].uv0;

    float w0 = 1.0 - barycentrics.x - barycentrics.y;
    float w1 = barycentrics.x;
    float w2 = barycentrics.y;

    return w0 * uv0 + w1 * uv1 + w2 * uv2;
}

[shader("fragment")]
float4 fragMain(
    MSOutput input,
    uniform PushConstantReflection pushConstant
) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    int2 pixelCoord = int2(input.pos.xy);

    // 1. Check Visibility Buffer and Depth
    Texture2D<uint4> visTex = Texture2D<uint4>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.visibilityTextureIndex, 0));
    uint4 rawVis = visTex.Load(int3(pixelCoord, 0));

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelCoord, 0)).r;

    // Reverse-Z: 0.0 is background / skybox -> no reflection to cast
    if (rawVis.x == 0 || rawDepth <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 10000.0);
    }

    // 2. Fetch G-Buffer Normal & Material
    Texture2D normalTex   = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
    Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferMaterialIndex, 0));

    float4 normalSample   = normalTex.Load(int3(pixelCoord, 0));
    float4 materialSample = materialTex.Load(int3(pixelCoord, 0));

    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6 || materialSample.b >= 2.0) // Invalid normal or Unlit material
    {
        return float4(0.0, 0.0, 0.0, 10000.0);
    }

    float3 normal = normalSample.rgb * rsqrt(lenSqN);
    float roughness = clamp(materialSample.r, 0.001, 1.0);

    // Skip very rough surfaces (roughness > 0.85) to conserve ray tracing performance
    if (roughness > 0.85 || ubo->enableRTReflections == 0 || ubo->tlasDeviceAddress == 0)
    {
        return float4(0.0, 0.0, 0.0, 10000.0);
    }

    // 3. Reconstruct World Position
    float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
    float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 ndc = float4(ndc_xy, rawDepth, 1.0);
    float4 worldPosH = mul(pushConstant.invViewProj, ndc);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    float3 V = normalize(ubo->cameraWorldPos.xyz - worldPos);
    if (dot(normal, V) <= 0.001)
    {
        return float4(0.0, 0.0, 0.0, 10000.0);
    }

    // 4. GGX VNDF Importance-Sampled Reflection Direction
    float3 R;
    if (roughness < 0.01)
    {
        // Smooth mirror surface: exact reflection
        R = reflect(-V, normal);
    }
    else
    {
        // Glossy surface: GGX VNDF jitter
        float ign = interleaved_gradient_noise(float2(pixelCoord), pushConstant.frameIndex);
        float2 u = float2(ign, frac(ign * 1.6180339887 + 0.314159));
        float3 M = sample_ggx_vndf(V, normal, roughness, u);
        R = reflect(-V, M);

        // If sample is below the macro hemisphere, fallback to mirror ray
        if (dot(normal, R) <= 0.001)
        {
            R = reflect(-V, normal);
        }
    }

    // 5. Trace Reflection Ray against TLAS
    float3 rayOrigin = worldPos + normal * 0.002;
    RayDesc reflectionRayDesc;
    reflectionRayDesc.Origin = rayOrigin;
    reflectionRayDesc.Direction = R;
    reflectionRayDesc.TMin = 0.005;
    reflectionRayDesc.TMax = 1e4;

    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    let rayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

    rq.TraceRayInline(accelerationStructure, rayFlags, 0xFF, reflectionRayDesc);

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

    // 6. Evaluate Reflected Radiance
    if (!hit)
    {
        // Ray Miss: Sample skybox environment cubemap
        TextureCube prefilteredTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
        SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
        float3 skyColor = prefilteredTex.SampleLevel(samplerPrefiltered, R, 0).rgb * ubo->scaleIBLAmbient;
        if (pushConstant.denoiserMode == 1) // NRD REBLUR
        {
            skyColor = LinearToYCoCg(skyColor);
        }
        return float4(skyColor, 10000.0);
    }

    // Ray Hit: Query closest committed hit
    uint committedInstanceID = rq.CommittedRayInstanceCustomIndex();
    uint committedPrimIndex = rq.CommittedPrimitiveIndex();
    float2 committedBary = rq.CommittedTriangleBarycentrics();

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

    if (hitLut.doubleSided != 0 && dot(hitNormal, -R) < 0.0)
    {
        hitNormal = -hitNormal;
    }

    float hitT = rq.CommittedRayT();
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

    // Ambient diffuse light: sample DDGI probes if active, or scene irradiance cubemap
    float3 ambientLight = float3(0.0);
    if (ubo->enableDDGI != 0 && ubo->ddgiIrradianceTextureIndex != 0xFFFFFFFF && ubo->ddgiDistanceTextureIndex != 0xFFFFFFFF)
    {
        Texture2D ddgiIrrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiIrradianceTextureIndex, 0));
        Texture2D ddgiDistTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->ddgiDistanceTextureIndex, 0));
        SamplerState ddgiSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));

        float3 ddgiIrr = sample_ddgi_diffuse(ubo, hitPos, hitNormal, ddgiIrrTex, ddgiDistTex, ddgiSampler);
        ambientLight = (albedo / 3.14159265) * ddgiIrr;
    }
    else if (ubo->irradianceMapIndex != 0xFFFFFFFF)
    {
        TextureCube irradianceTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
        SamplerState samplerIrradiance = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
        ambientLight = irradianceTex.SampleLevel(samplerIrradiance, hitNormal, 0).rgb * (albedo / 3.14159265) * ubo->scaleIBLAmbient * 0.2;
    }
    else
    {
        ambientLight = (albedo / 3.14159265) * 0.05 * ubo->scaleIBLAmbient;
    }

    // Emissive light from hit surface (e.g. Cornell Box ceiling light)
    float3 emissiveLight = hitLut.emissiveFactor.rgb * hitLut.emissiveFactor.a;

    float3 finalRadiance = directLight + ambientLight + emissiveLight;
    if (pushConstant.denoiserMode == 1) // NRD REBLUR
    {
        finalRadiance = LinearToYCoCg(finalRadiance);
    }

    // Returns Specular Radiance in RGB, Hit Distance in A (NRD IN_SPEC_RADIANCE_HITDIST format)
    return float4(finalRadiance, hitT);
}
