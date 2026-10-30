#include "shaderIO.h"

// Ray-traced ambient occlusion: the same hemisphere kernel and per-pixel rotation SSAO.slang uses (so radius/power
// settings mean the same thing and the two are a fair visual/perf comparison), but each sample is a real traced ray
// -- any hit within `radius` occludes it -- instead of reprojecting into the depth buffer. Writes the same
// resources.SSAORaw slot SSAO does; SSAOBlur.slang denoises whichever of the two actually ran.

struct RTAOPayload
{
    uint dummy;
};
groupshared RTAOPayload gPayload;

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
    uniform PushConstantRTAO pushConstant
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

struct RTAOOutput
{
    float ao : SV_Target0;
};

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float2 intersection_uv(GpuRayTracingInstance lut, uint primIndex, float2 barycentrics)
{
    uint32_t* indices = (uint32_t*)lut.indexBufferAddress;
    uint i0 = indices[primIndex * 3 + 0];
    uint i1 = indices[primIndex * 3 + 1];
    uint i2 = indices[primIndex * 3 + 2];

    PackedVertex* vertices = (PackedVertex*)lut.vertexBufferAddress;
    float2 uv0 = vertices[i0].uv0;
    float2 uv1 = vertices[i1].uv0;
    float2 uv2 = vertices[i2].uv0;

    float w0 = 1.0 - barycentrics.x - barycentrics.y;
    float w1 = barycentrics.x;
    float w2 = barycentrics.y;

    return w0 * uv0 + w1 * uv1 + w2 * uv2;
}

// Any-hit-and-stop occlusion test with alpha evaluation. Returns the hit distance, or a negative value when the
// ray found nothing within maxDist -- not just whether it hit at all. Any hit anywhere inside the trace radius
// counting as FULL occlusion (what a plain bool return means to the caller) is why increasing the radius past a
// tiny value made everything darken sharply: a real occluder 2 world-units away and one sitting right at the edge
// of a 50-unit radius weighted identically. SSAO already avoids exactly this with its own distance-based
// rangeCheck; the caller applies the equivalent falloff to this distance instead.
float trace_occlusion_ray(UniformBufferObject* ubo, float3 rayOrigin, float3 rayDir, float maxDist)
{
    if (ubo->tlasDeviceAddress == 0)
        return -1.0;

    RayDesc rayDesc;
    rayDesc.Origin = rayOrigin;
    rayDesc.Direction = rayDir;
    // Keep a nonempty trace interval. A fixed shadow bias prevents self-intersection acne.
    rayDesc.TMin = 0.005 * ubo->unitsPerMeter;
    rayDesc.TMax = maxDist;

    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
    let rayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
                   RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
    sq.TraceRayInline(accelerationStructure, rayFlags, 0xFF, rayDesc);

    GpuRayTracingInstance* instanceLUTBuffer = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    SamplerState textureSampler = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

    while (sq.Proceed())
    {
        uint instanceID = sq.CandidateRayInstanceCustomIndex();
        uint primIndex = sq.CandidatePrimitiveIndex();

        GpuRayTracingInstance lut = instanceLUTBuffer[instanceID];

        // Real glass (KHR_materials_transmission) is standardly authored as alphaMode OPAQUE in glTF -- transmission
        // is a separate dimension from coverage, not alpha/blend -- so the alpha-cutout test below (identical to
        // trace_shadow_ray's) never sees it as anything but a solid occluder. Without this, anything visible
        // through or near glass (a display case, a window) went fully black: every AO ray toward it committed a
        // hit against the glass itself instead of continuing through to whatever is actually behind it.
        if (ray_see_through(lut.transmissionFactor, lut.alphaMode))
        {
            continue;
        }

        if (lut.alphaMode == 0 || !RayHitAlphaTested(lut))
        {
            sq.CommitNonOpaqueTriangleHit();
        }
        else
        {
            float2 uv = intersection_uv(lut, primIndex, sq.CandidateTriangleBarycentrics());
            if (RayHitAlpha(ubo, lut, textureSampler, uv, RayAlphaTestLod, primIndex, sq.CandidateTriangleBarycentrics()) >= lut.alphaCutoff)
            {
                sq.CommitNonOpaqueTriangleHit();
            }
        }
    }

    return sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? sq.CommittedRayT() : -1.0;
}

[shader("fragment")]
RTAOOutput fragMain(MSOutput input, uniform PushConstantRTAO pushConstant)
{
    RTAOOutput output;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    // At a lower ray tracing resolution one pixel of each block (shaderIO.h); RTUpsample.slang fills in the others.
    int2 pixelCoord = rt_trace_pixel(ubo, input.pos.xy, pushConstant.viewportSize);

    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float depth = depthTex.Load(int3(pixelCoord, 0)).r;
    if (depth <= 0.0)
    {
        output.ao = 1.0;
        return output;
    }

    float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 worldPosH = mul(pushConstant.invViewProj, float4(ndc, depth, 1.0));
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.normalTextureIndex, 0));
    float3 rawNormal = normalTex.Load(int3(pixelCoord, 0)).rgb;
    float lenN = length(rawNormal);
    float3 N = lenN > 1e-6 ? (rawNormal / lenN) : float3(0.0, 1.0, 0.0);

    // Without DLSS Ray Reconstruction the rotation is fixed per pixel: the occlusion only gets a spatial blur (addOcclusionBlurPass),
    // no temporal accumulation, so rotating it every frame was new noise every frame. Under RR it changes every frame, as RR needs
    // its noisy inputs to (it accumulates them over frames): a pattern fixed to the screen is image detail to it, and slid over the
    // surfaces as noise whenever the camera moved.
    const float2 noiseSeed = float2(pixelCoord) +
                             (ubo->rayReconstructionActive != 0 ? float(ubo->frameIndex % 64u) * float2(5.588238, 3.45367) : float2(0.0));
    float angle = hash12(noiseSeed) * 6.28318530718;
    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 tangent0 = normalize(cross(up, N));
    float3 bitangent0 = cross(N, tangent0);
    float3 tangent = tangent0 * cos(angle) + bitangent0 * sin(angle);
    float3 bitangent = -tangent0 * sin(angle) + bitangent0 * cos(angle);
    float3x3 TBN = float3x3(tangent, bitangent, N);

    float4* kernel = (float4*)pushConstant.kernelReference;
    // Off the surface by 1 cm (depth-buffer precision), or by the LOD's simplification error where that is more (shaderIO.h): either
    // would let rays self-intersect the surface (black acne).
    float originBias = raster_surface_bias(ubo, worldPos, 0.01 * ubo->unitsPerMeter);
    float3 rayOrigin = worldPos + N * originBias;
    float occlusion = 0.0;

    for (uint i = 0; i < pushConstant.kernelSize; i++)
    {
        // Cosine-weighted, stratified over the pixel's rays (the kernel's first entries were 4 fixed directions).
        const float stratum = (float(i) + hash12(noiseSeed + float(i) * 17.17)) / float(max(pushConstant.kernelSize, 1u));
        const float phi = frac(float(i) * 0.6180339887 + hash12(noiseSeed.yx + 3.7)) * 6.28318530718;
        const float sinTheta = sqrt(saturate(stratum));
        float3 rayDir = normalize(mul(float3(sinTheta * cos(phi), sinTheta * sin(phi), sqrt(saturate(1.0 - stratum))), TBN));
        float hitDist = trace_occlusion_ray(ubo, rayOrigin, rayDir, pushConstant.radius);
        if (hitDist >= 0.0)
        {
            // A hit right next to the surface fully occludes; one out near the edge of the radius barely
            // registers -- the same intent as SSAO's rangeCheck, so radius means the same thing for both.
            occlusion += 1.0 - saturate(hitDist / pushConstant.radius);
        }
    }

    float ao = 1.0 - (occlusion / float(pushConstant.kernelSize));
    output.ao = pow(saturate(ao), pushConstant.power);
    return output;
}
