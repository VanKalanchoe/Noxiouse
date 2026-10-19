#include "shaderIO.h"
#include "NRDFrontEnd.slang"
#include "DDGICommon.slang"
#include "RayHitShading.slang"

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

// No reflection traced at a pixel (background, unlit, too rough, facing away): no radiance at "miss" distance, which is
// also what the DLSS-RR hit distance guide reads (RRGuides.slang).
static const float ReflectionMissDistance = 10000.0;
float4 noReflection(uint denoiserMode)
{
    return NRD_PackSignal(denoiserMode, float3(0.0), ReflectionMissDistance, 1.0, 1.0);
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

[shader("fragment")]
float4 fragMain(
    MSOutput input,
    uniform PushConstantReflection pushConstant
) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    // At a lower ray tracing resolution one pixel of each block (shaderIO.h); RTUpsample.slang fills in the others.
    int2 pixelCoord = rt_trace_pixel(ubo, input.pos.xy, pushConstant.viewportSize);

    // 1. Check Visibility Buffer and Depth
    Texture2D<uint4> visTex = Texture2D<uint4>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.visibilityTextureIndex, 0));
    uint4 rawVis = visTex.Load(int3(pixelCoord, 0));

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelCoord, 0)).r;

    // Reverse-Z: 0.0 is background / skybox -> no reflection to cast
    if (rawVis.x == 0 || rawDepth <= 0.0)
    {
        return noReflection(pushConstant.denoiserMode);
    }

    // 2. Fetch G-Buffer Normal & Material
    Texture2D normalTex   = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
    Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferMaterialIndex, 0));

    float4 normalSample   = normalTex.Load(int3(pixelCoord, 0));
    float4 materialSample = materialTex.Load(int3(pixelCoord, 0));

    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6 || materialSample.b >= 2.0) // Invalid normal or Unlit material
    {
        return noReflection(pushConstant.denoiserMode);
    }

    float3 normal = normalSample.rgb * rsqrt(lenSqN);
    float roughness = clamp(materialSample.r, 0.001, 1.0);

    float maxTraceRoughness = (pushConstant.denoiserMode != 0) ? 0.85 : 0.55;
    if (roughness > maxTraceRoughness || ubo->enableRTReflections == 0 || ubo->tlasDeviceAddress == 0)
    {
        return noReflection(pushConstant.denoiserMode);
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
        return noReflection(pushConstant.denoiserMode);
    }

    // 4. GGX VNDF Importance-Sampled Reflection Direction
    float3 R;
    if (roughness < 0.01)
    {
        // Smooth mirror surface: exact reflection without stochastic noise
        R = reflect(-V, normal);
    }
    else
    {
        // Glossy surface with active denoiser (NRD / DLSS-RR): GGX VNDF jitter
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
    // Use robust bias to prevent depth-buffer precision self-intersection (same as DeferredLighting)
    float3 rayOrigin = worldPos + normal * (0.01 * ubo->unitsPerMeter);
    RayDesc reflectionRayDesc;
    reflectionRayDesc.Origin = rayOrigin;
    reflectionRayDesc.Direction = R;
    reflectionRayDesc.TMin = 0.005 * ubo->unitsPerMeter;
    reflectionRayDesc.TMax = 1e4;

    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    let rayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

    rq.TraceRayInline(accelerationStructure, rayFlags, 0xFF, reflectionRayDesc);

    GpuRayTracingInstance* instanceLUTBuffer = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

    while (rq.Proceed())
    {
        uint instanceID = rq.CandidateRayInstanceCustomIndex();
        uint primIndex = rq.CandidatePrimitiveIndex();

        GpuRayTracingInstance lut = instanceLUTBuffer[instanceID];

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
    if (!hit && ubo->prefilteredMapIndex != 0xFFFFFFFF)
    {
        // Ray Miss: Sample skybox environment cubemap at the correct roughness mip level
        TextureCube prefilteredTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
        SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
        float lod = roughness * ubo->prefilteredCubeMipLevels;
        float3 skyColor = prefilteredTex.SampleLevel(samplerPrefiltered, R, lod).rgb * ubo->scaleIBLAmbient;
        return NRD_PackSignal(pushConstant.denoiserMode, skyColor, ReflectionMissDistance, abs(mul(ubo->view, float4(worldPos, 1.0)).z), roughness);
    }
    if (!hit)
        return NRD_PackSignal(pushConstant.denoiserMode, float3(0.0), ReflectionMissDistance, abs(mul(ubo->view, float4(worldPos, 1.0)).z), roughness);

    // Ray Hit: the hit's lighting (RayHitShading.slang)
    float hitT = rq.CommittedRayT();
    float3 finalRadiance = shadeRayHit(ubo, rq.CommittedRayInstanceCustomIndex(), rq.CommittedPrimitiveIndex(), rq.CommittedTriangleBarycentrics(),
                                       rayOrigin, R, hitT);

    // Incoming radiance (de-modulated: the lighting multiplies the specular factor back) with the ray's hit distance,
    // packed for the denoiser (NRDFrontEnd.slang).
    return NRD_PackSignal(pushConstant.denoiserMode, finalRadiance, hitT, abs(mul(ubo->view, float4(worldPos, 1.0)).z), roughness);
}
