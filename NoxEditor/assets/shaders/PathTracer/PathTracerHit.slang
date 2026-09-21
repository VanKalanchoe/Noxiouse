#pragma once

#include "PathPayload.slang"
#include "PathTracerBridge.slang"
#include "PathTracerNEE.slang"

[[vk::push_constant]]
ConstantBuffer<PushConstantPathTracer> pushConstant;

[shader("closesthit")]
void closestHitMain(inout PathPayload payload : SV_RayPayload, in BuiltInTriangleIntersectionAttributes attrib)
{
    PathState path = PathPayload::unpack(payload);

    float3 rayOrigin = WorldRayOrigin();
    float3 rayDir = WorldRayDirection();
    float rayT = RayTCurrent();

    path.incrementVertexIndex();
    path.rayCone = path.rayCone.propagateDistance(rayT);
    path.sceneLength += rayT;

    uint vertexIdx = path.getVertexIndex();

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    RaytracingAccelerationStructure as = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);

    // 1. Load surface geometry and material
    Bridge::SurfaceData surface = Bridge::loadSurface(
        InstanceID(),
        PrimitiveIndex(),
        attrib.barycentrics,
        rayDir,
        path.rayCone.getWidth(),
        ubo
    );


    // RTXPT exports depth and motion from its chosen path surface rather than from a separate raster pass.
    // Use jitter-free matrices for motion and the jittered clip transform for the depth supplied to Streamline.
    bool rrVisibleSurface = vertexIdx == 1 || (vertexIdx > 1 && path.hasFlag(PathFlags::deltaOnlyPath));
    if (pushConstant.rrDepthStorageIndex != 0xFFFFFFFF && rrVisibleSurface)
    {
        uint2 pixel = uint2(path.id & 0xFFFFu, path.id >> 16);
        float4 depthClip = mul(ubo->proj, mul(ubo->view, float4(surface.shadingData.posW, 1.0)));
        float4 currClip = mul(ubo->nonJitteredProj, mul(ubo->view, float4(surface.shadingData.posW, 1.0)));
        float4 prevClip = mul(ubo->prevProj, mul(ubo->prevView, float4(surface.shadingData.posW, 1.0)));
        float2 currNDC = currClip.xy / currClip.w;
        float2 prevNDC = prevClip.xy / prevClip.w;
        float2 motion = (prevNDC - currNDC) * float2(0.5, -0.5);

        RWTexture2D<float> rrDepth = RWTexture2D<float>.Handle(uint2(pushConstant.rrDepthStorageIndex, 0));
        RWTexture2D<float2> rrMotion = RWTexture2D<float2>.Handle(uint2(pushConstant.rrMotionStorageIndex, 0));
        RWTexture2D<float2> rrSpecularMotion = RWTexture2D<float2>.Handle(uint2(pushConstant.rrSpecularMotionStorageIndex, 0));
        RWTexture2D<float> rrSpecularHitDistance = RWTexture2D<float>.Handle(uint2(pushConstant.rrSpecularHitDistanceStorageIndex, 0));
        rrDepth[pixel] = depthClip.z / depthClip.w;
        rrMotion[pixel] = motion;
        // This is RTXPT's fallback for pixels without a reliable mirror hit. Stable-plane reflection motion can
        // replace it later without changing the Streamline interface.
        rrSpecularMotion[pixel] = motion;
        if (vertexIdx >= 2)
            rrSpecularHitDistance[pixel] = rayT;
    }

    // Record primary surface view Z and first bounce hit distance for NRD denoiser
    if (vertexIdx == 1)
    {
        float4 viewPos = mul(ubo->view, float4(surface.shadingData.posW, 1.0));
        path.primaryViewZ = abs(viewPos.z);

        if (pushConstant.rrDiffuseAlbedoStorageIndex != 0xFFFFFFFF)
        {
            RWTexture2D<float4> rrDiffuse = RWTexture2D<float4>.Handle(uint2(pushConstant.rrDiffuseAlbedoStorageIndex, 0));
            RWTexture2D<float4> rrSpecular = RWTexture2D<float4>.Handle(uint2(pushConstant.rrSpecularAlbedoStorageIndex, 0));
            RWTexture2D<float4> rrNormalRoughness = RWTexture2D<float4>.Handle(uint2(pushConstant.rrNormalRoughnessStorageIndex, 0));
            uint2 pixel = uint2(path.id & 0xFFFFu, path.id >> 16);
            rrDiffuse[pixel] = float4(surface.rrDiffuseAlbedo, 1.0);
            rrSpecular[pixel] = float4(surface.rrSpecularAlbedo, 1.0);
            rrNormalRoughness[pixel] = float4(surface.shadingData.N, surface.rrRoughness);
        }
    }
    else if (vertexIdx >= 2)
    {
        if (vertexIdx == 2)
            path.firstBounceHitDist = rayT;

        // A primary transmission event shows this surface through the glass. Feeding RR the glass normal/albedo
        // while the noisy color describes the surface behind it makes RR lock onto and smear the pane. RTXPT solves
        // this with stable planes; use the transmitted plane as the guide until the full multi-plane blend runs here.
        if (path.hasFlag(PathFlags::deltaOnlyPath) && pushConstant.rrDiffuseAlbedoStorageIndex != 0xFFFFFFFF)
        {
            RWTexture2D<float4> rrDiffuse = RWTexture2D<float4>.Handle(uint2(pushConstant.rrDiffuseAlbedoStorageIndex, 0));
            RWTexture2D<float4> rrSpecular = RWTexture2D<float4>.Handle(uint2(pushConstant.rrSpecularAlbedoStorageIndex, 0));
            RWTexture2D<float4> rrNormalRoughness = RWTexture2D<float4>.Handle(uint2(pushConstant.rrNormalRoughnessStorageIndex, 0));
            uint2 pixel = uint2(path.id & 0xFFFFu, path.id >> 16);
            rrDiffuse[pixel] = float4(surface.rrDiffuseAlbedo, 1.0);
            rrSpecular[pixel] = float4(surface.rrSpecularAlbedo, 1.0);
            rrNormalRoughness[pixel] = float4(surface.shadingData.N, surface.rrRoughness);
        }
    }

    // 2. Collect surface emission (with MIS if previous bounce used NEE)
    if (any(surface.emission > float3(0.0)))
    {
        // No competing emissive-triangle sampler exists yet; retain full weight.
        float3 filteredEmission = FireflyFilter(surface.emission, pushConstant.fireflyFilterThreshold, path.fireflyFilterK);
        float3 emissionContrib = path.thp * filteredEmission;
        path.L.rgb += emissionContrib;

        if (vertexIdx == 1)
        {
            // Primary emission
            // Primary emission remains outside NRD's material-demodulated signals.
        }
        else
        {
            if (path.primaryLobe == LobeType::DiffuseReflection)
                path.diffuseRadiance += emissionContrib;
            else
                path.specularRadiance += emissionContrib;
        }
    }

    // Check bounce limit
    if (vertexIdx > pushConstant.maxBounces || path.getCounter(PackedCounters::DiffuseBounces) > pushConstant.maxDiffuseBounces)
    {
        path.terminate();
        payload = PathPayload::pack(path);
        return;
    }

    // 3. Evaluate Next Event Estimation (direct light sampling)
    uint rngState = RTXPTSampling::uniformState(path.id, vertexIdx, pushConstant.frameIndex, RTXPTSampling::EffectNEE);

    float3 directDiffuse, directSpecular;
    float3 directLight = PathTracer::evaluateNEE(
        surface.shadingData, surface.bsdf, as, ubo, rngState,
        pushConstant.neeCandidateSamples, pushConstant.neeFullSamples,
        pushConstant.skyboxTextureIndex, pushConstant.environmentImportanceIndex,
        directDiffuse, directSpecular);

    if (any(directLight > float3(0.0)))
    {
        float3 filteredDirect = FireflyFilter(directLight, pushConstant.fireflyFilterThreshold, path.fireflyFilterK);
        float filterScale = Average(directLight) > 0.0 ? Average(filteredDirect) / Average(directLight) : 1.0;
        directLight = filteredDirect;
        directDiffuse *= filterScale;
        directSpecular *= filterScale;
        float3 directContrib = path.thp * directLight;
        path.L.rgb += directContrib;

        if (vertexIdx == 1)
        {
            path.diffuseRadiance += path.thp * directDiffuse;
            path.specularRadiance += path.thp * directSpecular;
        }
        else
        {
            if (path.primaryLobe == LobeType::DiffuseReflection)
                path.diffuseRadiance += directContrib;
            else
                path.specularRadiance += directContrib;
        }
    }

    // 4. Sample BSDF for next bounce
    float4 bsdfRand;
    // RTXPT keeps low-discrepancy BSDF sampling through the first diffuse vertex, then uses its faster
    // stateless uniform sequence. The important property is that NEE can no longer shift this sequence.
    if (path.getCounter(PackedCounters::DiffuseBounces) < 1u)
        bsdfRand = RTXPTSampling::generate4(path.id, vertexIdx, pushConstant.frameIndex, RTXPTSampling::EffectScatterBSDF);
    else
    {
        uint bsdfState = RTXPTSampling::uniformState(path.id, vertexIdx, pushConstant.frameIndex, RTXPTSampling::EffectScatterBSDF);
        bsdfRand.x = Bridge::rand(bsdfState);
        bsdfRand.y = Bridge::rand(bsdfState);
        bsdfRand.z = Bridge::rand(bsdfState);
        bsdfRand.w = Bridge::rand(bsdfState);
    }

    BSDFSample sampleResult;
    bool valid = surface.bsdf.sample(surface.shadingData, bsdfRand, sampleResult);

    // Upstream RTXPT reports pdf == 0 for valid delta events.
    if (!valid || all(sampleResult.weight <= float3(0.0)) || any(!isfinite(sampleResult.weight)))
    {
        path.terminate();
        payload = PathPayload::pack(path);
        return;
    }

    // Record primary lobe on first bounce
    if (vertexIdx == 1)
    {
        path.primaryLobe = sampleResult.lobe;
    }

    // Keep exporting the surface seen through a stack of glass interfaces. Once this sampled path reflects or
    // scatters instead of transmitting, later vertices are indirect lighting and no longer valid primary guides.
    if (path.hasFlag(PathFlags::deltaOnlyPath) && (sampleResult.lobe & LobeType::Transmission) == 0)
        path.setFlag(PathFlags::deltaOnlyPath, false);
    path.setFlag(PathFlags::transmission, (sampleResult.lobe & LobeType::Transmission) != 0);
    path.setFlag(PathFlags::specular, (sampleResult.lobe & LobeType::Specular) != 0);
    path.setFlag(PathFlags::delta, (sampleResult.lobe & LobeType::Delta) != 0);

    // Update throughput
    path.thp *= sampleResult.weight;
    path.bsdfScatterPdf = sampleResult.pdf;
    path.fireflyFilterK = ComputeNewScatterFireflyFilterK(path.fireflyFilterK, sampleResult.pdf, sampleResult.lobeP);
    if (sampleResult.pdf > 0.0)
        path.rayCone = RayCone::make(path.rayCone.getWidth(),
                                    path.rayCone.getSpreadAngle() + ComputeRayConeSpreadExpansion(sampleResult.pdf));
    if ((sampleResult.lobe & LobeType::Diffuse) != 0 &&
        !(sampleResult.lobe == LobeType::DiffuseTransmission && (vertexIdx % 2) == 1))
        path.incrementCounter(PackedCounters::DiffuseBounces);

    // 5. Russian Roulette early termination (after 2nd bounce)
    if (vertexIdx >= 2)
    {
        float rrVal = sqrt(Luminance(path.thp));
        float prob = saturate(0.85 - rrVal);
        prob = prob * prob;
        prob = saturate(prob + max(0.0, float(vertexIdx) / float(pushConstant.maxBounces) - 0.4));

        uint rrState = RTXPTSampling::uniformState(path.id, vertexIdx, pushConstant.frameIndex, RTXPTSampling::EffectRussianRoulette);
        if (Bridge::rand(rrState) < prob)
        {
            path.terminate();
            payload = PathPayload::pack(path);
            return;
        }

        path.thp /= max(1.0 - prob, 0.01);
    }

    // 6. Setup next ray segment
    float3 offsetNormal = surface.shadingData.N;
    if (dot(sampleResult.wo, offsetNormal) < 0.0)
    {
        offsetNormal = -offsetNormal;
    }

    path.origin = ComputeRayOrigin(surface.shadingData.posW, offsetNormal);
    path.dir = sampleResult.wo;

    payload = PathPayload::pack(path);
}
