#include "shaderIO.h"

#define RTXDI_ENABLE_STORE_RESERVOIR 0
#define RTXDI_GI_RESERVOIR_BUFFER g_DummyReservoirBuffer
RWStructuredBuffer<RTXDI_PackedGIReservoir> g_DummyReservoirBuffer;
#include "Rtxdi/GI/Reservoir.hlsli"
#include "Rtxdi/Utils/Math.hlsli"

struct ReSTIRGIPayload
{
    uint dummy;
};
groupshared ReSTIRGIPayload gPayload;

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
    uniform PushConstantReSTIRGIInitial pushConstant
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

// PCG Random Number Generator
uint pcg_hash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float2 pcg2d(uint2 p, uint frame)
{
    uint seed = p.x + p.y * 1920u + frame * 1920u * 1080u;
    uint h1 = pcg_hash(seed);
    uint h2 = pcg_hash(h1);
    return float2(float(h1) / 4294967295.0, float(h2) / 4294967295.0);
}

// Cosine-weighted hemisphere sampling
float3 sample_cosine_hemisphere(float3 n, float2 u)
{
    float r = sqrt(u.x);
    float phi = 2.0 * 3.14159265359 * u.y;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u.x));

    float3 b1, b2;
    build_orthonormal_basis(n, b1, b2);
    return normalize(x * b1 + y * b2 + z * n);
}

// Barycentric UV Interpolation for Alpha Testing
float2 intersection_uv(InstanceLUT lut, uint primIndex, float2 barycentrics)
{
    if (lut.vertexBufferAddress == 0) return float2(0.0);
    uint32_t* indices = (uint32_t*)lut.indexBufferAddress;
    Vertex* vertices = (Vertex*)lut.vertexBufferAddress;

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

    float w0 = 1.0 - barycentrics.x - barycentrics.y;
    float w1 = barycentrics.x;
    float w2 = barycentrics.y;

    return w0 * vertices[i0].uv0 + w1 * vertices[i1].uv0 + w2 * vertices[i2].uv0;
}

struct PSOutput
{
    float4 initialDiffuse : SV_Target0;
};

[shader("fragment")]
PSOutput fragMain(
    MSOutput input,
    uniform PushConstantReSTIRGIInitial pushConstant
)
{
    PSOutput outVal;
    outVal.initialDiffuse = float4(0.0);

    uint2 pixelCoord = uint2(input.pos.xy);
    if (pixelCoord.x >= uint(pushConstant.viewportSize.x) || pixelCoord.y >= uint(pushConstant.viewportSize.y))
    {
        return outVal;
    }

    if (pushConstant.reservoirBufferReference == 0 || pushConstant.matrixReference == 0 ||
        pushConstant.depthTextureIndex == 0xFFFFFFFF || pushConstant.gbufferNormalIndex == 0xFFFFFFFF)
    {
        return outVal;
    }

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    RTXDI_ReservoirBufferParameters resParams;
    resParams.reservoirBlockRowPitch = pushConstant.reservoirBlockRowPitch;
    resParams.reservoirArrayPitch = pushConstant.reservoirArrayPitch;

    uint pointer = RTXDI_ReservoirPositionToPointer(resParams, pixelCoord, 0);
    RTXDI_PackedGIReservoir* reservoirBuffer = (RTXDI_PackedGIReservoir*)pushConstant.reservoirBufferReference;

    if (ubo->tlasDeviceAddress == 0 || ubo->instanceLUTReference == 0)
    {
        reservoirBuffer[pointer] = (RTXDI_PackedGIReservoir)0;
        return outVal;
    }

    // 1. Read Depth Buffer (Reverse-Z: 1.0 = near, 0.0 = infinity)
    Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
    float rawDepth = depthTex.Load(int3(pixelCoord, 0)).r;

    if (rawDepth <= 0.00001)
    {
        // Sky / void: Store empty reservoir
        reservoirBuffer[pointer] = (RTXDI_PackedGIReservoir)0;
        return outVal;
    }

    // 2. Read Primary Normal Buffer (m_gbufferNormal stores raw world normals directly in [-1, 1])
    Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.gbufferNormalIndex, 0));
    float4 normalSample = normalTex.Load(int3(pixelCoord, 0));
    float lenSqN = dot(normalSample.rgb, normalSample.rgb);
    if (lenSqN < 1e-6)
    {
        reservoirBuffer[pointer] = (RTXDI_PackedGIReservoir)0;
        return outVal;
    }
    float3 normal = normalSample.rgb * rsqrt(lenSqN);

    // 3. Reconstruct Primary World Position
    float2 uv = (float2(pixelCoord) + 0.5) / pushConstant.viewportSize;
    float2 ndc_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 ndc = float4(ndc_xy, rawDepth, 1.0);
    float4 worldPosH = mul(pushConstant.invViewProj, ndc);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    // 4. Generate Cosine-Weighted Hemisphere Ray
    float2 rngSample = pcg2d(pixelCoord, pushConstant.frameIndex);
    float3 rayDir = sample_cosine_hemisphere(normal, rngSample);
    float NdotL = max(dot(normal, rayDir), 0.0);
    // Floor raised from 1e-4 to 0.05: a near-grazing sample at the old floor could produce a
    // reservoir weight (1/rayPdf) up to ~10,000x. Under a static camera that's invisible because M
    // grows large over many accumulated frames, diluting any single frame's rare outlier via 1/M
    // normalization. Under camera motion, temporal reprojection fails more often (disocclusion), M
    // collapses back toward 1 far more frequently, and that single frame's extreme-weight sample
    // becomes the ENTIRE displayed value, undiluted -- a visible flare that clears once M rebuilds.
    // 0.05 caps the maximum reservoir weight at 20x, which is still enough range for legitimate
    // near-grazing contributions without letting the RIS selection get dominated by an outlier.
    float rayPdf = max(NdotL / 3.14159265359, 0.05);

    // 5. Trace Indirect Diffuse Ray against TLAS
    float3 rayOrigin = worldPos + normal * 0.01;
    RayDesc diffuseRay;
    diffuseRay.Origin = rayOrigin;
    diffuseRay.Direction = rayDir;
    diffuseRay.TMin = 0.005;
    diffuseRay.TMax = 1e4;

    RaytracingAccelerationStructure accelerationStructure = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    let rayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

    rq.TraceRayInline(accelerationStructure, rayFlags, 0xFF, diffuseRay);

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

    float3 hitPos;
    float3 hitNormal;
    float3 hitRadiance = float3(0.0);

    if (!hit)
    {
        // Miss: Sample Environment Cubemap Skybox
        if (ubo->prefilteredMapIndex != 0xFFFFFFFF)
        {
            TextureCube prefilteredTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
            SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
            hitRadiance = prefilteredTex.SampleLevel(samplerPrefiltered, rayDir, 0).rgb * ubo->scaleIBLAmbient;
        }
        else
        {
            hitRadiance = lerp(float3(0.08, 0.08, 0.12), float3(0.4, 0.6, 0.95), saturate(rayDir.y * 0.5 + 0.5)) * ubo->scaleIBLAmbient;
        }
        hitPos = worldPos + rayDir * 1000.0;
        hitNormal = -rayDir;
    }
    else
    {
        // Closest Hit Secondary Surface
        uint committedInstanceID = rq.CommittedRayInstanceCustomIndex();
        uint committedPrimIndex = rq.CommittedPrimitiveIndex();
        float2 committedBary = rq.CommittedTriangleBarycentrics();

        InstanceLUT hitLut = instanceLUTBuffer[committedInstanceID];
        if (hitLut.vertexBufferAddress == 0)
        {
            reservoirBuffer[pointer] = (RTXDI_PackedGIReservoir)0;
            return outVal;
        }

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
        hitNormal = normalize(mul((float3x3)hitLut.normalMatrix, localNormal));

        if (hitLut.doubleSided != 0 && dot(hitNormal, -rayDir) < 0.0)
        {
            hitNormal = -hitNormal;
        }

        float hitT = rq.CommittedRayT();
        hitPos = rayOrigin + rayDir * hitT;

        float3 albedo = hitLut.baseColorFactor.rgb;
        if (hitLut.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D hitTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + hitLut.baseColorTextureIndex, 0));
            albedo *= hitTex.SampleLevel(textureSampler, hitUV, 0).rgb;
        }

        float3 emissive = hitLut.emissiveFactor.rgb * hitLut.emissiveFactor.a;

        // Direct analytical lighting at secondary hit
        float3 directLight = float3(0.0);
        if (ubo->lightDataReference != 0)
        {
            LightData* lights = (LightData*)ubo->lightDataReference;
            for (uint i = 0; i < ubo->lightCount; i++)
        {
            LightData light = lights[i];
            uint lightType = (uint)light.position.w;

            float3 l;
            float dist = 1000.0;
            float attenuation = 1.0;

            if (lightType == 0) // Directional
            {
                l = normalize(light.direction.xyz);
            }
            else // Point or Spot
            {
                float3 lightVec = light.position.xyz - hitPos;
                dist = length(lightVec);
                l = dist > 0.0001 ? lightVec / dist : float3(0.0, 1.0, 0.0);

                float range = light.direction.w;
                if (range > 0.0)
                {
                    if (dist > range) continue;
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

            if (attenuation <= 0.0) continue;

            float NdotL_hit = dot(hitNormal, l);
            if (hitLut.doubleSided != 0)
            {
                NdotL_hit = abs(NdotL_hit);
            }

            if (NdotL_hit <= 0.0) continue;

            // Secondary Shadow Ray Test
            float3 shadowNormal = hitNormal;
            if (hitLut.doubleSided != 0 && dot(hitNormal, l) < 0.0)
            {
                shadowNormal = -hitNormal;
            }
            RayDesc shadowRay;
            shadowRay.Origin = hitPos + shadowNormal * 0.02;
            shadowRay.Direction = l;
            shadowRay.TMin = 0.005;
            shadowRay.TMax = (lightType == 0) ? 1000.0 : dist;

            RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> shadowQuery;
            shadowQuery.TraceRayInline(accelerationStructure,
                RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                0xFF, shadowRay);

            while (shadowQuery.Proceed())
            {
                uint sInst = shadowQuery.CandidateRayInstanceCustomIndex();
                uint sPrim = shadowQuery.CandidatePrimitiveIndex();
                InstanceLUT sLut = instanceLUTBuffer[sInst];
                if (sLut.alphaMode == 0 || sLut.baseColorTextureIndex == 0xFFFFFFFF)
                {
                    shadowQuery.CommitNonOpaqueTriangleHit();
                }
                else
                {
                    float2 sUV = intersection_uv(sLut, sPrim, shadowQuery.CandidateTriangleBarycentrics());
                    Texture2D sTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + sLut.baseColorTextureIndex, 0));
                    if (sTex.SampleLevel(textureSampler, sUV, 0).a >= sLut.alphaCutoff)
                    {
                        shadowQuery.CommitNonOpaqueTriangleHit();
                    }
                }
            }

            if (shadowQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            {
                directLight += light.color.rgb * light.color.w * NdotL_hit * attenuation;
            }
        }
    }

        // Continue the path beyond the secondary hit, matching RTXPT's ReSTIR GI integration: the
        // full path tracer keeps running past the primary GI vertex (this secondary hit) with its
        // usual NEE + Russian roulette, and everything collected from here onward becomes the
        // reservoir's radiance. albedo/PI * directLight above is this vertex's own NEE contribution
        // (unchanged); indirectLight below is vertex2+ (throughput starts at this vertex's albedo,
        // since the primary GI ray's own BRDF/pdf weighting is applied later by the reservoir weight
        // and, at composite time, by the PRIMARY surface's own albedo -- see RTXPT GIFinalShading.hlsl:
        // "the PDF component is applied here, the BRDF component is applied in final shading").
        float3 indirectLight = float3(0.0);
        {
            float3 throughput = albedo;
            float3 curPos = hitPos;
            float3 curNormal = hitNormal;

            const uint MAX_EXTRA_BOUNCES = 4;
            for (uint b = 0; b < MAX_EXTRA_BOUNCES; b++)
            {
                if (b >= 2)
                {
                    float2 rrU = pcg2d(pixelCoord + uint2(9001u + b * 97u, 5003u + b * 131u), pushConstant.frameIndex);
                    float p = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
                    if (rrU.x >= p) break;
                    throughput /= p;
                }

                float2 rngB = pcg2d(pixelCoord + uint2(1337u * (b + 1u), 7331u * (b + 1u)), pushConstant.frameIndex);
                float3 bounceDir = sample_cosine_hemisphere(curNormal, rngB);

                RayDesc bRay;
                bRay.Origin = curPos + curNormal * 0.02;
                bRay.Direction = bounceDir;
                bRay.TMin = 0.005;
                bRay.TMax = 1e4;

                RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> brq;
                brq.TraceRayInline(accelerationStructure, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, bRay);

                while (brq.Proceed())
                {
                    uint bInst = brq.CandidateRayInstanceCustomIndex();
                    uint bPrim = brq.CandidatePrimitiveIndex();
                    InstanceLUT bLut = instanceLUTBuffer[bInst];
                    if (bLut.alphaMode == 0 || bLut.baseColorTextureIndex == 0xFFFFFFFF)
                    {
                        brq.CommitNonOpaqueTriangleHit();
                    }
                    else
                    {
                        float2 bUV = intersection_uv(bLut, bPrim, brq.CandidateTriangleBarycentrics());
                        Texture2D bTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + bLut.baseColorTextureIndex, 0));
                        if (bTex.SampleLevel(textureSampler, bUV, 0).a >= bLut.alphaCutoff)
                        {
                            brq.CommitNonOpaqueTriangleHit();
                        }
                    }
                }

                if (brq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                {
                    float3 skyCol;
                    if (ubo->prefilteredMapIndex != 0xFFFFFFFF)
                    {
                        TextureCube prefilteredTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
                        SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));
                        skyCol = prefilteredTex.SampleLevel(samplerPrefiltered, bounceDir, 0).rgb * ubo->scaleIBLAmbient;
                    }
                    else
                    {
                        skyCol = lerp(float3(0.08, 0.08, 0.12), float3(0.4, 0.6, 0.95), saturate(bounceDir.y * 0.5 + 0.5)) * ubo->scaleIBLAmbient;
                    }
                    indirectLight += throughput * skyCol;
                    break;
                }

                uint cInst = brq.CommittedRayInstanceCustomIndex();
                uint cPrim = brq.CommittedPrimitiveIndex();
                float2 cBary = brq.CommittedTriangleBarycentrics();
                float cT = brq.CommittedRayT();

                InstanceLUT cLut = instanceLUTBuffer[cInst];
                if (cLut.vertexBufferAddress == 0) break;

                uint32_t* cIndices = (uint32_t*)cLut.indexBufferAddress;
                Vertex* cVertices = (Vertex*)cLut.vertexBufferAddress;
                uint ci0 = (cLut.indexBufferAddress != 0) ? cIndices[cPrim * 3 + 0] : (cPrim * 3 + 0);
                uint ci1 = (cLut.indexBufferAddress != 0) ? cIndices[cPrim * 3 + 1] : (cPrim * 3 + 1);
                uint ci2 = (cLut.indexBufferAddress != 0) ? cIndices[cPrim * 3 + 2] : (cPrim * 3 + 2);

                float cw0 = 1.0 - cBary.x - cBary.y;
                float2 cUV = cw0 * cVertices[ci0].uv0 + cBary.x * cVertices[ci1].uv0 + cBary.y * cVertices[ci2].uv0;
                float3 cLocalNormal = normalize(cw0 * cVertices[ci0].normal + cBary.x * cVertices[ci1].normal + cBary.y * cVertices[ci2].normal);
                float3 cNormal = normalize(mul((float3x3)cLut.normalMatrix, cLocalNormal));
                if (cLut.doubleSided != 0 && dot(cNormal, -bounceDir) < 0.0) cNormal = -cNormal;

                float3 cPos = bRay.Origin + bounceDir * cT;
                float3 cAlbedo = cLut.baseColorFactor.rgb;
                if (cLut.baseColorTextureIndex != 0xFFFFFFFF)
                {
                    Texture2D cTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + cLut.baseColorTextureIndex, 0));
                    cAlbedo *= cTex.SampleLevel(textureSampler, cUV, 0).rgb;
                }

                float3 cDirect = float3(0.0);
                if (ubo->lightDataReference != 0)
                {
                    LightData* lights = (LightData*)ubo->lightDataReference;
                    for (uint li = 0; li < ubo->lightCount; li++)
                    {
                        LightData light = lights[li];
                        uint lightType = (uint)light.position.w;
                        float3 l2 = (lightType == 0) ? normalize(light.direction.xyz) : normalize(light.position.xyz - cPos);
                        float dist2 = (lightType == 0) ? 1000.0 : length(light.position.xyz - cPos);
                        float attenuation = 1.0;
                        if (lightType != 0)
                        {
                            float range = light.direction.w;
                            attenuation = (range > 0.0)
                                ? (max(1.0 - pow(dist2 / range, 4.0), 0.0) / max(dist2 * dist2, 0.001))
                                : (1.0 / max(dist2 * dist2, 0.001));
                            if (lightType == 2)
                            {
                                float cd = dot(normalize(light.direction.xyz), -l2);
                                float angScale = light.spotParams.x;
                                float angOffset = light.spotParams.y;
                                float ang = saturate(cd * angScale + angOffset);
                                attenuation *= ang * ang;
                            }
                        }

                        float cNdotL = dot(cNormal, l2);
                        if (cLut.doubleSided != 0) cNdotL = abs(cNdotL);
                        if (cNdotL <= 0.001 || attenuation <= 0.0) continue;

                        RayDesc csRay;
                        csRay.Origin = cPos + cNormal * 0.02;
                        csRay.Direction = l2;
                        csRay.TMin = 0.005;
                        csRay.TMax = (lightType == 0) ? 1000.0 : dist2;

                        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> csq;
                        csq.TraceRayInline(accelerationStructure, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, csRay);
                        while (csq.Proceed())
                        {
                            uint sInst = csq.CandidateRayInstanceCustomIndex();
                            uint sPrim = csq.CandidatePrimitiveIndex();
                            InstanceLUT sLut = instanceLUTBuffer[sInst];
                            if (sLut.alphaMode == 0 || sLut.baseColorTextureIndex == 0xFFFFFFFF)
                            {
                                csq.CommitNonOpaqueTriangleHit();
                            }
                            else
                            {
                                float2 sUV = intersection_uv(sLut, sPrim, csq.CandidateTriangleBarycentrics());
                                Texture2D sTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + sLut.baseColorTextureIndex, 0));
                                if (sTex.SampleLevel(textureSampler, sUV, 0).a >= sLut.alphaCutoff)
                                {
                                    csq.CommitNonOpaqueTriangleHit();
                                }
                            }
                        }
                        if (csq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                        {
                            cDirect += light.color.rgb * light.color.w * attenuation * cNdotL;
                        }
                    }
                }

                float3 cEmissive = cLut.emissiveFactor.rgb * cLut.emissiveFactor.a;
                indirectLight += throughput * ((cAlbedo / 3.14159265359) * cDirect + cEmissive);

                throughput *= cAlbedo;
                curPos = cPos;
                curNormal = cNormal;
            }
        }

        // Reflected radiance from the secondary surface towards the primary hit
        hitRadiance = (albedo / 3.14159265359) * directLight + indirectLight + emissive;
    }

    // 6. Pack Initial Reservoir (M = 1, weightSum = 1.0 / rayPdf, age = 0)
    RTXDI_GIReservoir reservoir = RTXDI_MakeGIReservoir(hitPos, hitNormal, hitRadiance, rayPdf);
    reservoirBuffer[pointer] = RTXDI_PackGIReservoir(reservoir, 0);

    outVal.initialDiffuse = float4(hitRadiance, 1.0);
    return outVal;
}
