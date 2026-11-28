#pragma once

#include "PathPayload.slang"
#include "PathTracerBridge.slang"
#include "PathTracerPushConstants.slang"
#include "EmissiveLights.slang"

// A path's ray that hit nothing: the sky, the sun's and moon's discs, rect lights' faces -- written into `path`, which ends here. One
// function for both ways of tracing: the ray tracing pipeline's miss (missMain below) and the compute path tracer's ray query
// (PathTracerCompute.slang).
void pathTracerMiss(inout PathState path, float3 rayOrigin, float3 rayDir)
{

    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);

    // Sample environment cubemap
    // RTXPT defaults EnvironmentMapDiffuseSampleMIPLevel to 2. Keep mirror/delta and camera
    // visibility sharp while reducing high-frequency variance after a diffuse scatter.
    float environmentLod = path.getVertexIndex() > 0 &&
        !path.hasFlag(PathFlags::specular) && !path.hasFlag(PathFlags::delta)
        ? float(pushConstant.environmentDiffuseMip) : 0.0;
    float3 envRadiance = Bridge::sampleSkybox(rayDir, ubo, pushConstant.skyboxTextureIndex, environmentLod);
    // The sun's (and moon's) disc, and rect lights' faces, where nothing else adds their light: seen directly, through glass from
    // the camera, and in mirrors. Not through glass after a rough vertex: that vertex's NEE already let them through the glass.
    // Without light samples (Light Samples 0) every path that reaches them adds them.
    bool rectLight = false;
    const bool seenThroughGlassAfterScatter = path.hasFlag(PathFlags::transmission) && !path.hasFlag(PathFlags::deltaOnlyPath);
    if (path.getVertexIndex() == 0 || path.hasFlag(PathFlags::deltaOnlyPath) || (path.hasFlag(PathFlags::delta) && !seenThroughGlassAfterScatter) ||
        pushConstant.neeFullSamples == 0)
    {
        envRadiance += directional_light_discs(ubo, rayDir);
        float rectT = 1e30;
        const float3 rectRadiance = ray_rect_lights(ubo, rayOrigin, rayDir, rectT);
        if (rectT < 1e30)
        {
            envRadiance = rectRadiance; // in front of the sky
            rectLight = true;
        }
    }

    if (any(envRadiance > float3(0.0)))
    {
        float misWeight = 1.0;
        if (!rectLight && path.getVertexIndex() > 0 && path.bsdfScatterPdf > 0.0 && pushConstant.skyboxTextureIndex != 0xFFFFFFFF &&
            pushConstant.neeFullSamples > 0)
        {
            EnvMapSampler sampler = Bridge::environmentSampler(ubo, pushConstant.skyboxTextureIndex, pushConstant.environmentImportanceIndex);
            // The environment's share of the light samples: what the emissive meshes leave (PathTracerNEE.slang), over the lights.
            float lightPdf = sampler.MIPDescentEvalPdf(rayDir) * (1.0 - EmissiveLights::branchProbability(ubo, true)) / float(ubo.lightCount + 1);
            float bsdfPdf = path.bsdfScatterPdf;
            misWeight = bsdfPdf*bsdfPdf / max(bsdfPdf*bsdfPdf + lightPdf*lightPdf, 1e-30);
        }

        // Seen by the camera directly, in mirrors or through glass: not clamped (RTXPT's stable planes have no firefly filter).
        if (path.getVertexIndex() > 0 && !path.hasFlag(PathFlags::deltaTree))
            envRadiance = FireflyFilter(envRadiance, pushConstant.fireflyFilterThreshold *
                                        sqrt(0.18 / scene_exposure(ubo, pushConstant.autoExposureTextureIndex, pushConstant.exposureCompensation,
                                                                    pushConstant.autoExposureMinEV, pushConstant.autoExposureMaxEV)),
                                        path.fireflyFilterK);
        float3 envContrib = path.thp * envRadiance * misWeight;
        path.L.rgb += envContrib;
        if (path.hasFlag(PathFlags::deltaTree))
            path.stableL += envContrib; // RTXPT's stable radiance: the sky seen directly, through glass, in mirrors

        if (path.getVertexIndex() == 0)
        {
            // Primary sky stays in the unfiltered emission/background signal.
        }
        else
        {
            if (path.primaryLobe == LobeType::DiffuseReflection)
                path.diffuseRadiance += envContrib;
            else
                path.specularRadiance += envContrib;
        }
    }

    if (path.getVertexIndex() == 1) path.firstBounceHitDist = 65504.0;

    // A specular reflection off RR's guide surface that reaches the sky: its image is at infinity (Bridge::rrSkySpecularDistance).
    if (path.hasFlag(PathFlags::rrSpecHitPending) && pushConstant.rrSpecularHitDistanceStorageIndex != 0xFFFFFFFF)
    {
        path.setFlag(PathFlags::rrSpecHitPending, false);
        RWTexture2D<float>.Handle(uint2(SafeHeapSlot(pushConstant.rrSpecularHitDistanceStorageIndex), 0))[uint2(path.id & 0xFFFFu, path.id >> 16)] =
            Bridge::rrSkySpecularDistance(ubo);
    }

    path.terminate();
}

[shader("miss")]
void missMain(inout PathPayload payload)
{
    PathState path = PathPayload::unpack(payload);
    pathTracerMiss(path, WorldRayOrigin(), WorldRayDirection());
    payload = PathPayload::pack(path);
}
