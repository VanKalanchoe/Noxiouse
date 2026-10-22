#pragma once

#include "PathPayload.slang"
#include "PathTracerBridge.slang"
#include "PathTracerNEE.slang"
#include "../MipFeedback.slang"

[[vk::push_constant]]
ConstantBuffer<PushConstantPathTracer> pushConstant;

// DLSS Ray Reconstruction's guides for a pixel: the surface's albedos, normal / roughness, depth and motion.
void writeRRGuides(uint2 pixel, Bridge::SurfaceData surface, UniformBufferObject* ubo, bool albedoGuides)
{
    const float3 position = surface.shadingData.posW;
    if (pushConstant.rrDepthStorageIndex != 0xFFFFFFFF)
    {
        float4 depthClip = mul(ubo->proj, mul(ubo->view, float4(position, 1.0)));
        float4 currClip = mul(ubo->nonJitteredProj, mul(ubo->view, float4(position, 1.0)));
        float4 prevClip = mul(ubo->prevProj, mul(ubo->prevView, float4(position, 1.0)));
        float2 motion = (prevClip.xy / prevClip.w - currClip.xy / currClip.w) * float2(0.5, -0.5);
        RWTexture2D<float>.Handle(uint2(pushConstant.rrDepthStorageIndex, 0))[pixel] = depthClip.z / depthClip.w;
        RWTexture2D<float2>.Handle(uint2(pushConstant.rrMotionStorageIndex, 0))[pixel] = motion;
        RWTexture2D<float2>.Handle(uint2(pushConstant.rrSpecularMotionStorageIndex, 0))[pixel] = motion;
    }
    if (albedoGuides && pushConstant.rrDiffuseAlbedoStorageIndex != 0xFFFFFFFF)
    {
        RWTexture2D<float4>.Handle(uint2(pushConstant.rrDiffuseAlbedoStorageIndex, 0))[pixel] = float4(surface.rrDiffuseAlbedo, 1.0);
        RWTexture2D<float4>.Handle(uint2(pushConstant.rrSpecularAlbedoStorageIndex, 0))[pixel] = float4(surface.rrSpecularAlbedo, 1.0);
        RWTexture2D<float4>.Handle(uint2(pushConstant.rrNormalRoughnessStorageIndex, 0))[pixel] = float4(surface.shadingData.N, surface.rrRoughness);
    }
}

// RTXPT's realtime firefly filter threshold: 0.1 * sqrt(the luminance the exposure maps to middle grey) * 1000 (Sample.cpp,
// ToneMappingPass::GetPreExposedGray). It was fixed at the manual-exposure value (exposure 1): with auto exposure the light near
// lamps and every bounce of it was clamped several times harder than RTXPT does -- lamp-lit walls and rooms came out dark.
float fireflyThreshold(UniformBufferObject* ubo)
{
    const float exposure = scene_exposure(ubo, pushConstant.autoExposureTextureIndex, pushConstant.exposureCompensation,
                                          pushConstant.autoExposureMinEV, pushConstant.autoExposureMaxEV);
    return pushConstant.fireflyFilterThreshold * sqrt(0.18 / exposure);
}

// A smooth thin pane whose transmission dominates (a window): what RR is guided by is behind it.
bool isGuideGlass(GpuRayTracingInstance instance)
{
    const bool thin = (instance.pathTracerFlags & PathTracerThinSet) != 0 ? (instance.pathTracerFlags & PathTracerThin) != 0
                                                                          : instance.thickness <= 0.0;
    return thin && instance.transmissionFactor >= 0.5 && instance.roughnessFactor < 0.08;
}

// RTXPT's guides through glass: the camera sees through a smooth thin pane whose transmission dominates, so RR's guides are the
// surface behind it (RTXPT's stable plane on the transmission branch), every frame -- whichever lobe this frame's path sampled.
// They followed the sampled lobe before: the pane in some frames, the room in the others, and RR smeared what is behind. A ray
// straight on past such panes (up to a few) finds that surface; it is loaded as the hit shader loads one.
void writeGuidesBehindGlass(uint2 pixel, float3 origin, float3 direction, float rayConeWidth, UniformBufferObject* ubo)
{
    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);
    GpuRayTracingInstance* instances = (GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = 1e9;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(tlas, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
    while (query.Proceed())
    {
        const uint slot = query.CandidateRayInstanceCustomIndex();
        const GpuRayTracingInstance candidate = instances[slot];
        if (isGuideGlass(candidate))
            continue;
        if (candidate.alphaMode == 0 || Bridge::AlphaTest(slot, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), ubo))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return; // the sky behind: the pane's own guides stay
    const uint slot = query.CommittedRayInstanceCustomIndex();
    const float4x4 world = ((GpuTransform*)ubo->sceneTransformsReference)[slot].world;
    const float3x4 objToWorld = float3x4(world[0], world[1], world[2]);
    const Bridge::SurfaceData behind = Bridge::loadSurface(slot, query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(),
                                                           direction, rayConeWidth, ubo, objToWorld);
    writeRRGuides(pixel, behind, ubo, true);
}

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

    // A rect light's face in front of the surface, seen by the camera or in a mirror (its own sampling covers every other path).
    if (vertexIdx == 1 || path.hasFlag(PathFlags::delta))
    {
        float rectT = rayT;
        const float3 rectRadiance = ray_rect_lights(ubo, rayOrigin, rayDir, rectT);
        if (rectT < rayT)
        {
            const float3 rectContrib = path.thp * rectRadiance;
            path.L.rgb += rectContrib;
            if (vertexIdx > 1)
            {
                if (path.primaryLobe == LobeType::DiffuseReflection)
                    path.diffuseRadiance += rectContrib;
                else
                    path.specularRadiance += rectContrib;
            }
            path.terminate();
            payload = PathPayload::pack(path);
            return;
        }
    }

    // 1. Load surface geometry and material
    Bridge::SurfaceData surface = Bridge::loadSurface(
        InstanceID(),
        PrimitiveIndex(),
        attrib.barycentrics,
        rayDir,
        path.rayCone.getWidth(),
        ubo,
        ObjectToWorld3x4()
    );


    // Texture streaming: the mips the surfaces the camera sees (directly, in mirrors, through glass) ask for, with the ray cone LOD
    // the samples use. Raster feedback alone asked for the render resolution's mips without the path tracer's sharper bias, and
    // never for what is seen in a mirror.
    if ((vertexIdx == 1 || path.hasFlag(PathFlags::deltaOnlyPath)) && ubo->mipFeedbackReference != 0 &&
        isMipFeedbackPixel(int2(path.id & 0xFFFFu, path.id >> 16), ubo->mipFeedbackFrame))
    {
        GpuRayTracingInstance lut = ((GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference)[InstanceID()];
        const uint textures[5] = { lut.baseColorTextureIndex, lut.metallicRoughnessTextureIndex, lut.normalTextureIndex,
                                   lut.emissiveTextureIndex, lut.transmissionTextureIndex };
        for (uint t = 0; t < 5; ++t)
        {
            if (textures[t] == 0xFFFFFFFF || textures[t] >= MipFeedbackSlots)
                continue;
            Texture2D texture = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + textures[t], 0));
            uint width, height;
            texture.GetDimensions(width, height);
            reportMipFeedbackLod(ubo, textures[t], log2(max(surface.uvFootprint, 1e-20)) + 0.5 * log2(float(width) * float(height)) +
                                                   ubo->pathTracerTexLodBias);
        }
    }

    // A segment through a volume (it ended on the volume's inside): what the volume absorbed on the way.
    if (any(surface.absorptionCoefficient > 0.0))
        path.thp *= exp(-surface.absorptionCoefficient * rayT);

    // RR's specular hit distance (RTXPT's specHitT): how far the ray leaving the guide surface went -- the length of its reflection
    // ray. It used to be this segment's length whenever the guide surface was not the first hit: behind a pane, the few millimetres
    // from the glass to the menu, telling RR the menu's reflections lay on the menu itself.
    if (path.hasFlag(PathFlags::rrSpecHitPending))
    {
        path.setFlag(PathFlags::rrSpecHitPending, false);
        if (pushConstant.rrSpecularHitDistanceStorageIndex != 0xFFFFFFFF)
            RWTexture2D<float>.Handle(uint2(pushConstant.rrSpecularHitDistanceStorageIndex, 0))[uint2(path.id & 0xFFFFu, path.id >> 16)] = rayT;
    }

    // RTXPT exports depth and motion from its chosen path surface rather than from a separate raster pass.
    // Use jitter-free matrices for motion and the jittered clip transform for the depth supplied to Streamline.
    bool rrVisibleSurface = vertexIdx == 1 || (vertexIdx > 1 && path.hasFlag(PathFlags::deltaOnlyPath));
    // A window the camera sees (directly or through other glass): the guides are what is behind it, traced once at the first pane;
    // panes further in write nothing (the traced surface already stands for them).
    const bool guideGlass = rrVisibleSurface && pushConstant.rrDepthStorageIndex != 0xFFFFFFFF &&
                            isGuideGlass(((GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference)[InstanceID()]);
    if (guideGlass && vertexIdx > 1)
        rrVisibleSurface = false;
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
        rrDepth[pixel] = depthClip.z / depthClip.w;
        rrMotion[pixel] = motion;
        // This is RTXPT's fallback for pixels without a reliable mirror hit. Stable-plane reflection motion can
        // replace it later without changing the Streamline interface.
        rrSpecularMotion[pixel] = motion;
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
        if (path.hasFlag(PathFlags::deltaOnlyPath) && !guideGlass && pushConstant.rrDiffuseAlbedoStorageIndex != 0xFFFFFFFF)
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

    if (guideGlass && vertexIdx == 1)
        writeGuidesBehindGlass(uint2(path.id & 0xFFFFu, path.id >> 16), surface.shadingData.posW, rayDir, path.rayCone.getWidth(), ubo);

    // 2. Collect surface emission (with MIS if previous bounce used NEE)
    if (any(surface.emission > float3(0.0)))
    {
        // Weighed against the previous vertex's emissive light sampling (PathTracerNEE.slang): power heuristic with the pdf that
        // sampling had for this point. Seen from the camera, in mirrors and through glass from the camera (pdf 0: a delta event)
        // only the BSDF finds it: full weight. Through glass after a rough vertex the pdf is that vertex's, as in the miss shader.
        float misWeight = 1.0;
        if (vertexIdx > 1 && path.bsdfScatterPdf > 0.0)
        {
            const float lightPdf = EmissiveLights::branchProbability(ubo, pushConstant.skyboxTextureIndex != 0xFFFFFFFF) *
                                   (EmissiveLights::hasTriangleLights(ubo)
                                       ? EmissiveLights::trianglePdf(ubo, rayOrigin, InstanceID(), PrimitiveIndex(), rayT, surface.cosFace)
                                       : EmissiveLights::pdf(ubo, InstanceID(), surface.triangleArea, rayT, surface.cosFace));
            const float bsdfPdf = path.bsdfScatterPdf;
            misWeight = bsdfPdf * bsdfPdf / max(bsdfPdf * bsdfPdf + lightPdf * lightPdf, 1e-30);
        }
        // What the camera sees directly, in mirrors and through glass is not clamped (RTXPT's stable planes have no firefly filter):
        // bright bulbs seen through a window kept only ~38.
        const float3 weightedEmission = surface.emission * misWeight;
        float3 filteredEmission = (vertexIdx == 1 || path.hasFlag(PathFlags::deltaTree))
            ? weightedEmission : FireflyFilter(weightedEmission, fireflyThreshold(ubo), path.fireflyFilterK);
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

    // Emissive meshes: a scene has many (every lit window, lamp, sign), a point is lit by the few near it. More candidates let the
    // resampling find those -- on the surface the camera sees only: every candidate loads a triangle and a texel, and 12 at every
    // vertex of every path cost more than the rest of the path tracer.
    const uint candidateSamples = (ubo->emissiveLightCount > 0 && vertexIdx == 1) ? max(pushConstant.neeCandidateSamples, 8u)
                                                                                  : pushConstant.neeCandidateSamples;
    // No light sampling on a surface with only delta lobes (smooth glass, mirrors): no direction of light could be weighed, RTXPT
    // skips it (PathTracerNEE.hlsli) -- the light beyond is found by the path going on.
    const bool hasNonDeltaLobes = (FalcorBSDF::getLobes(surface.bsdf.data) & LobeType::NonDelta) != 0;
    float3 directDiffuse = float3(0.0), directSpecular = float3(0.0);
    float3 directLight = !hasNonDeltaLobes ? float3(0.0) : PathTracer::evaluateNEE(
        surface.shadingData, surface.bsdf, as, ubo, rngState,
        candidateSamples, pushConstant.neeFullSamples,
        pushConstant.skyboxTextureIndex, pushConstant.environmentImportanceIndex,
        directDiffuse, directSpecular);

    if (any(directLight > float3(0.0)))
    {
        float3 filteredDirect = FireflyFilter(directLight, fireflyThreshold(ubo), path.fireflyFilterK);
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

    // RTXPT's stable planes: at the first smooth glass the camera's path meets (seen directly or through other glass), both of its
    // delta lobes are followed every frame, each with its whole weight -- this path goes on through it (and keeps exporting the
    // surface behind as RR's guide), the reflection is traced once this path ends (PathTracerRayGen.slang). Picking one at random
    // gave each pixel the full, unweighted mirror image one frame in ~25 and the picture behind the others: RR read the panes as
    // mirrors (a framed menu behind glass vanished under its reflection) and the reflection boiled.
    if (path.hasFlag(PathFlags::deltaOnlyPath) && !hasNonDeltaLobes && !path.hasBranch())
    {
        DeltaLobe lobes[cMaxDeltaLobes];
        int lobeCount;
        float nonDeltaPart;
        surface.bsdf.evalDeltaLobes(surface.shadingData, lobes, lobeCount, nonDeltaPart);
        const DeltaLobe transmitted = lobes[0];
        const DeltaLobe reflected = lobes[1];
        if (nonDeltaPart <= 0.0 && any(transmitted.thp > 0.0) && any(reflected.thp > 0.0))
        {
            PathState branch = path;
            branch.setFlag(PathFlags::deltaOnlyPath, false); // not RR's guide
            branch.setFlag(PathFlags::transmission, false);
            branch.setFlag(PathFlags::specular, true);
            branch.setFlag(PathFlags::delta, true);
            path.branchFlagsAndVertexIndex = branch.flagsAndVertexIndex;
            path.branchOrigin = ComputeRayOrigin(surface.shadingData.posW,
                                                 dot(reflected.dir, surface.shadingData.N) < 0.0 ? -surface.shadingData.N : surface.shadingData.N);
            path.branchDir = reflected.dir;
            path.branchThp = path.thp * reflected.thp;
            path.branchRayCone = path.rayCone.widthSpreadAngleFP16;

            sampleResult.wo = transmitted.dir;
            sampleResult.weight = transmitted.thp;
            sampleResult.pdf = 0.0;
            sampleResult.lobe = LobeType::DeltaTransmission;
            sampleResult.lobeP = 1.0;
            valid = true;
        }
    }

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
    {
        path.setFlag(PathFlags::deltaOnlyPath, false);
        path.setFlag(PathFlags::rrSpecHitPending, true); // this is RR's guide surface: the next segment is its specular hit distance
    }
    path.setFlag(PathFlags::transmission, (sampleResult.lobe & LobeType::Transmission) != 0);
    path.setFlag(PathFlags::specular, (sampleResult.lobe & LobeType::Specular) != 0);
    path.setFlag(PathFlags::delta, (sampleResult.lobe & LobeType::Delta) != 0);
    if ((sampleResult.lobe & LobeType::Delta) == 0)
        path.setFlag(PathFlags::deltaTree, false);

    // Update throughput
    path.thp *= sampleResult.weight;
    // RTXPT: the scatter pdf of this vertex, 0 after a delta event (glass, mirror) -- light met beyond it then counts fully
    // (RTXPT's "bias" of light through windows that NEE also reaches; matched here, it is a large part of its interiors' light).
    path.bsdfScatterPdf = sampleResult.pdf;
    path.fireflyFilterK = ComputeNewScatterFireflyFilterK(path.fireflyFilterK, sampleResult.pdf, sampleResult.lobeP);
    if (sampleResult.pdf > 0.0)
        path.rayCone = RayCone::make(path.rayCone.getWidth(),
                                    path.rayCone.getSpreadAngle() + ComputeRayConeSpreadExpansion(sampleResult.pdf));
    // RTXPT counts a rough specular bounce (roughness > 0.25) as a diffuse one too.
    if (((sampleResult.lobe & LobeType::Diffuse) != 0 || ((sampleResult.lobe & LobeType::Specular) != 0 && surface.rrRoughness > 0.25)) &&
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
