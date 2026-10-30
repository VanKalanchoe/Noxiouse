#pragma once

#include "PathPayload.slang"
#include "PathTracerBridge.slang"
#include "PathTracerNEE.slang"
#include "../MipFeedback.slang"

[[vk::push_constant]]
ConstantBuffer<PushConstantPathTracer> pushConstant;

// RTXPT's realtime firefly filter threshold: 0.1 * sqrt(the luminance the exposure maps to middle grey) * 1000 (Sample.cpp,
// ToneMappingPass::GetPreExposedGray). It was fixed at the manual-exposure value (exposure 1): with auto exposure the light near
// lamps and every bounce of it was clamped several times harder than RTXPT does -- lamp-lit walls and rooms came out dark.
float fireflyThreshold(UniformBufferObject* ubo)
{
    const float exposure = scene_exposure(ubo, pushConstant.autoExposureTextureIndex, pushConstant.exposureCompensation,
                                          pushConstant.autoExposureMinEV, pushConstant.autoExposureMaxEV);
    return pushConstant.fireflyFilterThreshold * sqrt(0.18 / exposure);
}

// ---- RTXPT's DLSS-RR guides where the camera's path meets smooth glass (RTXPT PostProcess.hlsl, DENOISER_PREPARE_INPUTS, with
// MIX_BY_THROUGHPUT and ALLOW_MIX_NORMALS as it ships). The glass splits the pixel into RTXPT's stable planes: the pane itself (plane
// 0), the surface seen through it and the surface seen in it, each at the end of its delta path. Their albedos, normals and roughness
// are mixed by weights from the planes' throughputs (0.2 each, the pane taking what the other two leave), an even share (0.01) and the
// material's dominant lobe (0.05; RTXPT's PSDDominantDeltaLobe, the transmission for the Bistro's windows); a plane that ends in the
// sky adds nothing. Depth and motion are the dominant plane's (RTXPT exports its surface). The stable radiance is mixed in at the end
// (PathTracerRayGen.slang). Before, the guides were the surface behind alone, and nothing told RR about the reflection on the pane.

// A stable plane goes on through a thin pane that only lets light straight through (RTXPT continues a plane through surfaces with
// delta lobes only, along the transmission), unless the material makes the pane itself the plane (PSDDominantDeltaLobe -1).
bool rrPlanePassesThrough(GpuRayTracingInstance instance)
{
    const bool thin = (instance.pathTracerFlags & PathTracerThinSet) != 0 ? (instance.pathTracerFlags & PathTracerThin) != 0
                                                                          : instance.thickness <= 0.0;
    return thin && instance.transmissionFactor * (1.0 - saturate(instance.metallicFactor)) >= 0.999 &&
           instance.roughnessFactor < 0.08 && (instance.pathTracerFlags & PathTracerPSDDominantSurface) == 0;
}

// Where a stable plane leaving along `direction` ends: the first surface it does not pass through. False: the sky.
bool rrPlaneBase(float3 origin, float3 direction, float rayConeWidth, UniformBufferObject* ubo, out Bridge::SurfaceData base, out float hitT)
{
    base = (Bridge::SurfaceData)0;
    hitT = 0.0;
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
        if (rrPlanePassesThrough(instances[slot]))
            continue;
        if (instances[slot].alphaMode == 0 || Bridge::AlphaTest(slot, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), ubo))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;
    const uint slot = query.CommittedRayInstanceCustomIndex();
    const float4x4 world = ((GpuTransform*)ubo->sceneTransformsReference)[slot].world;
    hitT = query.CommittedRayT();
    base = Bridge::loadSurface(slot, query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(), direction,
                               rayConeWidth, ubo, float3x4(world[0], world[1], world[2]));
    return true;
}

// RR's depth and motion for a point the camera sees (directly, or the virtual point a reflection shows).
void writeRRDepthMotion(uint2 pixel, float3 position, UniformBufferObject* ubo)
{
    const float4 depthClip = mul(ubo->proj, mul(ubo->view, float4(position, 1.0)));
    const float4 currClip = mul(ubo->nonJitteredProj, mul(ubo->view, float4(position, 1.0)));
    const float4 prevClip = mul(ubo->prevProj, mul(ubo->prevView, float4(position, 1.0)));
    const float2 motion = (prevClip.xy / prevClip.w - currClip.xy / currClip.w) * float2(0.5, -0.5);
    RWTexture2D<float>.Handle(uint2(pushConstant.rrDepthStorageIndex, 0))[pixel] = depthClip.z / depthClip.w;
    RWTexture2D<float2>.Handle(uint2(pushConstant.rrMotionStorageIndex, 0))[pixel] = motion;
    RWTexture2D<float2>.Handle(uint2(pushConstant.rrSpecularMotionStorageIndex, 0))[pixel] = motion;
}

// The guides at the glass where the camera's path splits (PathTracerHit's stable-plane split): `pane` the glass, `thp` the path's
// throughput reaching it, the two delta lobes as RTXPT evaluates them (its planes' throughputs).
void writeSplitRRGuides(uint2 pixel, Bridge::SurfaceData pane, float3 thp, DeltaLobe transmitted, DeltaLobe reflected, uint paneFlags,
                        float rayConeWidth, UniformBufferObject* ubo)
{
    const float thpT = saturate(Average(thp * transmitted.thp));
    const float thpR = saturate(Average(thp * reflected.thp));
    float3 weights = float3(saturate(saturate(1.0 - thpT) - thpR), thpT, thpR) * 0.2 + 0.01;
    const uint dominant = (paneFlags & PathTracerPSDDominantSurface) != 0 ? 0u : ((paneFlags & PathTracerPSDDominantReflection) != 0 ? 2u : 1u);
    weights[dominant] += 0.05;
    weights /= weights.x + weights.y + weights.z;

    const float3 paneN = pane.shadingData.N;
    float3 diffuse = weights.x * pane.rrDiffuseAlbedo;
    float3 specular = weights.x * pane.rrSpecularAlbedo;
    float3 normal = weights.x * paneN;
    float roughness = weights.x * pane.rrRoughness;
    float3 dominantPosition = pane.shadingData.posW;

    Bridge::SurfaceData base;
    float hitT;
    const float3 throughOrigin = ComputeRayOrigin(pane.shadingData.posW, dot(transmitted.dir, paneN) < 0.0 ? -paneN : paneN);
    if (rrPlaneBase(throughOrigin, transmitted.dir, rayConeWidth, ubo, base, hitT))
    {
        diffuse += weights.y * base.rrDiffuseAlbedo;
        specular += weights.y * base.rrSpecularAlbedo;
        normal += weights.y * base.shadingData.N;
        roughness += weights.y * base.rrRoughness;
        if (dominant == 1u)
            dominantPosition = base.shadingData.posW;
    }
    const float3 mirrorOrigin = ComputeRayOrigin(pane.shadingData.posW, dot(reflected.dir, paneN) < 0.0 ? -paneN : paneN);
    const bool mirrorHit = rrPlaneBase(mirrorOrigin, reflected.dir, rayConeWidth, ubo, base, hitT);
    // The pane is RTXPT's plane 0, the mirror its specular motion vectors use: what the pane reflects moves as its mirror image does.
    // The guides' own roughness gate (RTXPT: below 0.25) is not applied -- a smooth pane's reflection is a mirror whatever lies behind.
    const float2 specularMotion = Bridge::rrSpecularMotionVector(ubo, pane.shadingData.posW, -pane.shadingData.V,
                                                                 reflected.dir * (mirrorHit ? hitT : Bridge::rrSkySpecularDistance(ubo)));
    if (mirrorHit)
    {
        diffuse += weights.z * base.rrDiffuseAlbedo;
        specular += weights.z * base.rrSpecularAlbedo;
        // Its normal as the mirror shows it (RTXPT turns a plane's normal by the delta path's image transform).
        normal += weights.z * reflect(base.shadingData.N, paneN);
        roughness += weights.z * base.rrRoughness;
        if (dominant == 2u) // the point the reflection shows, straight on along the camera's ray
            dominantPosition = pane.shadingData.posW - pane.shadingData.V * hitT;
    }

    const float normalLength = length(normal);
    RWTexture2D<float4>.Handle(uint2(pushConstant.rrDiffuseAlbedoStorageIndex, 0))[pixel] = float4(diffuse, 1.0);
    RWTexture2D<float4>.Handle(uint2(pushConstant.rrSpecularAlbedoStorageIndex, 0))[pixel] = float4(specular, 1.0);
    RWTexture2D<float4>.Handle(uint2(pushConstant.rrNormalRoughnessStorageIndex, 0))[pixel] =
        float4(normalLength >= 1e-5 ? normal / normalLength : float3(0.0, 0.0, 1.0), roughness);
    if (pushConstant.rrDepthStorageIndex != 0xFFFFFFFF)
    {
        writeRRDepthMotion(pixel, dominantPosition, ubo);
        RWTexture2D<float2>.Handle(uint2(pushConstant.rrSpecularMotionStorageIndex, 0))[pixel] = specularMotion;
    }
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
            if (path.hasFlag(PathFlags::deltaTree))
                path.stableL += rectContrib;
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
            if (textures[t] == 0xFFFFFFFF)
                continue;
            uint width, height;
            MaterialTextureSize(ubo, textures[t], width, height);
            reportMaterialTextureLod(ubo, textures[t], surface.uv, log2(max(surface.uvFootprint, 1e-20)) + 0.5 * log2(float(width) * float(height)) +
                                                                  ubo->pathTracerTexLodBias);
        }
    }

    // A segment through a volume (it ended on the volume's inside): what the volume absorbed on the way.
    if (any(surface.absorptionCoefficient > 0.0))
        path.thp *= exp(-surface.absorptionCoefficient * rayT);

    // Only delta lobes (smooth glass, mirrors): no light sampling here, the stable planes go on through it.
    const bool hasNonDeltaLobes = (FalcorBSDF::getLobes(surface.bsdf.data) & LobeType::NonDelta) != 0;

    // RR's specular hit distance (RTXPT ExportSpecHitTStop): from the guide surface's specular scatter to the first surface that is
    // not a mirror or clear glass (it held minus the scene length at the scatter). PathTracerRayGen.slang turns it into RR's specular
    // motion vectors.
    if (path.hasFlag(PathFlags::rrSpecHitPending) && hasNonDeltaLobes)
    {
        path.setFlag(PathFlags::rrSpecHitPending, false);
        if (pushConstant.rrSpecularHitDistanceStorageIndex != 0xFFFFFFFF)
        {
            RWTexture2D<float> specHitT = RWTexture2D<float>.Handle(uint2(pushConstant.rrSpecularHitDistanceStorageIndex, 0));
            const uint2 pixel = uint2(path.id & 0xFFFFu, path.id >> 16);
            specHitT[pixel] = max(0.0, path.sceneLength + specHitT[pixel]);
        }
    }

    // RTXPT exports depth and motion from its chosen path surface rather than from a separate raster pass (jitter-free matrices
    // for motion, the jittered clip transform for the depth): the surface the camera sees, or through glass the one behind it --
    // until the glass's stable-plane split has written this pixel's guides (writeSplitRRGuides).
    const bool rrVisibleSurface = !path.hasFlag(PathFlags::rrGuidesDone) &&
                                  (vertexIdx == 1 || (vertexIdx > 1 && path.hasFlag(PathFlags::deltaOnlyPath)));
    if (pushConstant.rrDepthStorageIndex != 0xFFFFFFFF && rrVisibleSurface)
        writeRRDepthMotion(uint2(path.id & 0xFFFFu, path.id >> 16), surface.shadingData.posW, ubo);

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

        // Seen through glass that did not split the path (a pane with nothing to reflect): this surface is the guide.
        if (rrVisibleSurface && pushConstant.rrDiffuseAlbedoStorageIndex != 0xFFFFFFFF)
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
        if (vertexIdx == 1 || path.hasFlag(PathFlags::deltaTree))
            path.stableL += emissionContrib; // RTXPT's stable radiance: emitters seen directly, through glass, in mirrors

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
    // resampling find those -- on the surface the camera sees only (directly, through glass, in a mirror: a menu behind its pane had
    // the fewer candidates of a bounce, and more of its light firefly-clamped away): every candidate loads a triangle and a texel, and
    // 12 at every vertex of every path cost more than the rest of the path tracer.
    const bool cameraSurface = vertexIdx == 1 || path.hasFlag(PathFlags::deltaTree);
    const uint candidateSamples = (ubo->emissiveLightCount > 0 && cameraSurface) ? max(pushConstant.neeCandidateSamples, 8u)
                                                                                 : pushConstant.neeCandidateSamples;
    // No light sampling on a surface with only delta lobes (smooth glass, mirrors): no direction of light could be weighed, RTXPT
    // skips it (PathTracerNEE.hlsli) -- the light beyond is found by the path going on.
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

            // RR's guides for the pixel, from the three stable planes this glass makes (RTXPT's throughputs: the lobes as evaluated).
            if (pushConstant.rrDiffuseAlbedoStorageIndex != 0xFFFFFFFF && !path.hasFlag(PathFlags::rrGuidesDone))
            {
                writeSplitRRGuides(uint2(path.id & 0xFFFFu, path.id >> 16), surface, path.thp, transmitted, reflected,
                                   ((GpuRayTracingInstance*)ubo->sceneRayTracingInstancesReference)[InstanceID()].pathTracerFlags,
                                   path.rayCone.getWidth(), ubo);
                path.setFlag(PathFlags::rrGuidesDone, true);
            }

            sampleResult.wo = transmitted.dir;
            sampleResult.weight = transmitted.thp;
            // The light through a thin pane as RTXPT's paths carry it (FalcorBSDF::sample, SpecularReflectionTransmission): the
            // transmission is taken 1 - F of the time (F at the pane's IoR) at its whole weight, so what is behind arrives times
            // 1 - F. The delta lobes' evaluation leaves that out (it gives the stable planes' throughputs, above); the reflection is
            // F either way.
            if (surface.shadingData.mtl.thinSurface)
                sampleResult.weight *= 1.0 - evalFresnelDielectric(surface.bsdf.data.Eta(),
                                                                   saturate(dot(surface.shadingData.N, surface.shadingData.V)));
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
    // A rough or diffuse transmission (frosted glass, leaves, cloth) ends it too, as RTXPT ends its delta-only path on any non-delta
    // scatter: what such a ray finds is lighting, not the surface the camera sees (and the miss shader would add the sun's disc again).
    if (path.hasFlag(PathFlags::deltaOnlyPath) &&
        ((sampleResult.lobe & LobeType::Transmission) == 0 || (sampleResult.lobe & LobeType::Delta) == 0))
    {
        path.setFlag(PathFlags::deltaOnlyPath, false);
        // This is RR's guide surface. A specular scatter off it (not diffuse, roughness up to 0.35) starts its specular hit distance
        // (RTXPT ExportSpecHitTStart: minus the scene length so far); otherwise there is none.
        if (pushConstant.rrSpecularHitDistanceStorageIndex != 0xFFFFFFFF)
        {
            const bool specularScatter = (sampleResult.lobe & (LobeType::DiffuseReflection | LobeType::DiffuseTransmission)) == 0 &&
                                         surface.rrRoughness <= 0.35;
            RWTexture2D<float>.Handle(uint2(pushConstant.rrSpecularHitDistanceStorageIndex, 0))[uint2(path.id & 0xFFFFu, path.id >> 16)] =
                specularScatter ? -path.sceneLength : 0.0;
            path.setFlag(PathFlags::rrSpecHitPending, specularScatter);
        }
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
