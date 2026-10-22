#include "shaderIO.h"
#include "NRDFrontEnd.slang"
#include "DDGICommon.slang"
#include "RayHitShading.slang"
#include "PathTracer/EmissiveLights.slang"

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
float4 noReflection(UniformBufferObject* ubo, uint denoiserMode)
{
    return NRD_PackSignal(denoiserMode, float3(0.0), reflection_miss_distance(ubo), 1.0, 1.0, ubo->unitsPerMeter);
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

// GGX with alpha = roughness^2: the distribution and Smith's masking of one direction.
float ggx_distribution(float alpha, float NdotH)
{
    const float a2 = alpha * alpha;
    const float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265359 * d * d);
}

float ggx_masking(float alpha, float NdotX)
{
    const float a2 = alpha * alpha;
    return 2.0 * NdotX / (NdotX + sqrt(a2 + (1.0 - a2) * NdotX * NdotX));
}

// The solid angle pdf of a direction reflected about a VNDF-sampled normal (Heitz 2018): G1(V) D(H) / (4 N.V).
float ggx_vndf_reflection_pdf(float alpha, float NdotV, float NdotH)
{
    return ggx_masking(alpha, NdotV) * ggx_distribution(alpha, NdotH) / (4.0 * NdotV);
}

uint reflection_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float reflection_random(inout uint state)
{
    state = reflection_hash(state);
    return float(state) * (1.0 / 4294967296.0);
}

// The emissive triangles' light in a glossy reflection, the path tracer's next event estimation for its specular lobe
// (PathTracerNEE.slang, the same triangle lights: EmissiveLights.slang). The VNDF ray alone finds a lamp only by chance: a glossy
// floor or table showed its lamps as a few bright speckles that boiled, or not at all, where the path tracer shows their smooth
// highlight. Resampled from candidates by their contribution, each weighed against the VNDF ray by the power heuristic (the ray
// takes the other weight when it hits a lamp: shadeRayHit's emissionWeight), as RTXPT does. Demodulated like the traced ray: the
// lobe's response over the pre-integrated reflectance the lighting multiplies back (DeferredLighting.slang).
static const uint ReflectionLightCandidates = 4;

float3 reflection_emissive_light(UniformBufferObject* ubo, float3 origin, float3 N, float3 V, float alpha, float3 f0, float3 reflectance,
                                 inout uint rng)
{
    const float NdotV = max(dot(N, V), 1e-4);
    const float nearDistance = 0.1 * ubo->unitsPerMeter; // closer than 10 cm a lamp counts as 10 cm away (no 1/d^2 spike)
    EmissiveLights::Sample chosen = (EmissiveLights::Sample)0;
    float3 chosenContribution = float3(0.0);
    float chosenWeight = 0.0;
    float weightSum = 0.0;
    for (uint candidate = 0; candidate < ReflectionLightCandidates; ++candidate)
    {
        const float4 u = float4(reflection_random(rng), reflection_random(rng), reflection_random(rng), reflection_random(rng));
        const float selectU = reflection_random(rng);
        EmissiveLights::Sample sample = EmissiveLights::sampleTriangles(ubo, origin, u);
        if (!sample.valid)
            continue;
        const float NdotL = dot(N, sample.direction);
        if (NdotL <= 0.0)
            continue;
        const float3 H = normalize(V + sample.direction);
        const float NdotH = saturate(dot(N, H));
        const float VdotH = saturate(dot(V, H));
        const float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - VdotH, 5.0);
        // f cos / pdf: D G2 F / (4 N.V), over the light's pdf, the MIS weight against the VNDF ray.
        const float bsdfPdf = ggx_vndf_reflection_pdf(alpha, NdotV, NdotH);
        const float misWeight = sample.pdf * sample.pdf / max(sample.pdf * sample.pdf + bsdfPdf * bsdfPdf, 1e-30);
        const float lobe = ggx_distribution(alpha, NdotH) * ggx_masking(alpha, NdotV) * ggx_masking(alpha, NdotL) / (4.0 * NdotV);
        const float3 radiance = sample.radiance * min(1.0, sample.distance * sample.distance / (nearDistance * nearDistance));
        const float3 contribution = radiance * fresnel * (lobe * misWeight / sample.pdf);
        const float weight = dot(contribution, float3(0.2126, 0.7152, 0.0722));
        if (!(weight > 0.0) || !isfinite(weight))
            continue;
        weightSum += weight;
        if (selectU * weightSum < weight)
        {
            chosen = sample;
            chosenContribution = contribution;
            chosenWeight = weight;
        }
    }
    if (!(weightSum > 0.0) || !(chosenWeight > 0.0))
        return float3(0.0);
    if (ray_hit_shadowed(ubo, origin, chosen.direction, max(chosen.distance - 0.02 * ubo->unitsPerMeter, 0.0)))
        return float3(0.0);
    const float3 light = chosenContribution * (weightSum / (float(ReflectionLightCandidates) * chosenWeight)) / max(reflectance, float3(1e-3));
    return all(isfinite(light)) ? light : float3(0.0);
}

[shader("fragment")]
float4 fragMain(
    MSOutput input,
    uniform PushConstantReflection pushConstant
) : SV_Target0
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    // At a lower ray tracing resolution one pixel of each block (shaderIO.h); RTUpsample.slang fills in the others.
    // The smooth fill pass traces every pixel itself.
    const bool smoothFill = pushConstant.smoothFillRoughness > 0.0;
    int2 pixelCoord = smoothFill ? int2(input.pos.xy) : rt_trace_pixel(ubo, input.pos.xy, pushConstant.viewportSize);

    // 1. Check Visibility Buffer and Depth
    Texture2D<uint4> visTex = Texture2D<uint4>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.visibilityTextureIndex, 0));
    uint4 rawVis = visTex.Load(int3(pixelCoord, 0));

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelCoord, 0)).r;

    // Reverse-Z: 0.0 is background / skybox -> no reflection to cast
    if (rawVis.x == 0 || rawDepth <= 0.0)
    {
        return noReflection(ubo, pushConstant.denoiserMode);
    }

    // 2. Fetch G-Buffer Normal & Material
    Texture2D normalTex   = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
    Texture2D materialTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferMaterialIndex, 0));

    float4 normalSample   = normalTex.Load(int3(pixelCoord, 0));
    float4 materialSample = materialTex.Load(int3(pixelCoord, 0));

    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6 || materialSample.b >= 0.75) // Invalid normal or Unlit material
    {
        return noReflection(ubo, pushConstant.denoiserMode);
    }

    float3 normal = normalSample.rgb * rsqrt(lenSqN);
    float roughness = clamp(materialSample.r, 0.001, 1.0);
    if (smoothFill && roughness >= pushConstant.smoothFillRoughness)
        discard; // rough enough: the upsampled reduced-resolution result stays

    // Denoised (NRD, or DLSS Ray Reconstruction, which takes the 1-sample reflection as it takes the path tracer's) up to rough
    // surfaces; raw only the glossy ones. DeferredLighting.slang fades the same way.
    float maxTraceRoughness = (pushConstant.denoiserMode != 0 || ubo->rayReconstructionActive != 0) ? 0.85 : 0.55;
    if (roughness > maxTraceRoughness || ubo->enableRTReflections == 0 || ubo->tlasDeviceAddress == 0)
    {
        return noReflection(ubo, pushConstant.denoiserMode);
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
        return noReflection(ubo, pushConstant.denoiserMode);
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
        // The noise by the block, not the traced pixel: traced pixels 2 apart got nearly the same gradient noise.
        const float2 noisePixel = (smoothFill || ubo->rtSubsample <= 1u) ? float2(pixelCoord) : floor(input.pos.xy);
        float ign = interleaved_gradient_noise(noisePixel, pushConstant.frameIndex);
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

    // The emissive triangles sampled as lights for a glossy lobe (reflection_emissive_light); the VNDF ray's hit on one is weighed
    // against that sampling (emissionWeight below). A mirror's lobe is a single direction: its ray alone.
    const bool sampleLights = roughness >= 0.01 && EmissiveLights::hasTriangleLights(ubo) && ubo->brdfLutIndex != 0xFFFFFFFF &&
                              pushConstant.gbufferAlbedoIndex != 0xFFFFFFFF;
    const float alpha = max(roughness * roughness, 0.001); // as sample_ggx_vndf
    float3 lightSample = float3(0.0);
    if (sampleLights)
    {
        // The surface's specular color and pre-integrated reflectance, as DeferredLighting.slang has them.
        Texture2D albedoTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferAlbedoIndex, 0));
        const float3 baseColor = albedoTex.Load(int3(pixelCoord, 0)).rgb;
        const float3 f0 = lerp(float3(0.04), baseColor, saturate(materialSample.g));
        Texture2D brdfTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->brdfLutIndex, 0));
        const float2 brdf = brdfTex.SampleLevel(SamplerState.Handle(uint2(SamplerIndex::SAMPLER_BRDFLUT, 0)),
                                                float2(clamp(dot(normal, V), 0.001, 1.0), 1.0 - max(roughness, 0.04)), 0).rg;
        uint rng = reflection_hash(uint(pixelCoord.x) * 1973u + uint(pixelCoord.y) * 9277u + pushConstant.frameIndex * 26699u) | 1u;
        lightSample = reflection_emissive_light(ubo, rayOrigin, normal, V, alpha, f0, f0 * brdf.x + brdf.y, rng);
    }

    RayDesc reflectionRayDesc;
    reflectionRayDesc.Origin = rayOrigin;
    reflectionRayDesc.Direction = R;
    reflectionRayDesc.TMin = 0.005 * ubo->unitsPerMeter;
    reflectionRayDesc.TMax = 100000.0 * ubo->unitsPerMeter; // 100 km: it used to stop at 10000 units (100 m in centimeters)

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

        // Glass and blended surfaces are looked through, as every other ray of the hybrid passes does (and the path tracer's
        // reflection rays do, through its glass): a window committed here was shaded as a lit white wall, so floors, tables and
        // wood reflected bright panels where the street or the room should be.
        if (lut.alphaMode == 2 || ray_see_through(lut.transmissionFactor, lut.alphaMode))
            continue;
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
        return NRD_PackSignal(pushConstant.denoiserMode, skyColor + lightSample, reflection_miss_distance(ubo), abs(mul(ubo->view, float4(worldPos, 1.0)).z), roughness, ubo->unitsPerMeter);
    }
    if (!hit)
        return NRD_PackSignal(pushConstant.denoiserMode, lightSample, reflection_miss_distance(ubo), abs(mul(ubo->view, float4(worldPos, 1.0)).z), roughness, ubo->unitsPerMeter);

    // Ray Hit: the hit's lighting (RayHitShading.slang), its emission weighed against the light sampling (power heuristic).
    float hitT = rq.CommittedRayT();
    float emissionWeight = 1.0;
    if (sampleLights)
    {
        const float lightPdf = EmissiveLights::trianglePdfAlong(ubo, rayOrigin, R, rq.CommittedRayInstanceCustomIndex(), rq.CommittedPrimitiveIndex(), hitT);
        if (lightPdf > 0.0)
        {
            const float bsdfPdf = ggx_vndf_reflection_pdf(alpha, max(dot(normal, V), 1e-4), saturate(dot(normal, normalize(V + R))));
            emissionWeight = bsdfPdf * bsdfPdf / max(bsdfPdf * bsdfPdf + lightPdf * lightPdf, 1e-30);
        }
    }
    float3 finalRadiance = shadeRayHit(ubo, rq.CommittedRayInstanceCustomIndex(), rq.CommittedPrimitiveIndex(), rq.CommittedTriangleBarycentrics(),
                                       rayOrigin, R, hitT, emissionWeight) + lightSample;

    // Incoming radiance (de-modulated: the lighting multiplies the specular factor back) with the ray's hit distance,
    // packed for the denoiser (NRDFrontEnd.slang).
    return NRD_PackSignal(pushConstant.denoiserMode, finalRadiance, hitT, abs(mul(ubo->view, float4(worldPos, 1.0)).z), roughness, ubo->unitsPerMeter);
}
