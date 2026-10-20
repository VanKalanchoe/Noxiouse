#pragma once

#include "PathPayload.slang"
#include "PathTracerBridge.slang"

[[vk::push_constant]]
ConstantBuffer<PushConstantPathTracer> pushConstant;

[shader("miss")]
void missMain(inout PathPayload payload : SV_RayPayload)
{
    PathState path = PathPayload::unpack(payload);

    float3 rayDir = WorldRayDirection();

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

    // Sample environment cubemap
    // RTXPT defaults EnvironmentMapDiffuseSampleMIPLevel to 2. Keep mirror/delta and camera
    // visibility sharp while reducing high-frequency variance after a diffuse scatter.
    float environmentLod = path.getVertexIndex() > 0 &&
        !path.hasFlag(PathFlags::specular) && !path.hasFlag(PathFlags::delta)
        ? float(pushConstant.environmentDiffuseMip) : 0.0;
    float3 envRadiance = Bridge::sampleSkybox(rayDir, ubo, pushConstant.skyboxTextureIndex, environmentLod);
    // The sun's (and moon's) disc, and rect lights' faces, where nothing else adds their light: seen directly, through glass from
    // the camera, and in mirrors. Not through glass after a rough vertex: that vertex's NEE already let them through the glass.
    bool rectLight = false;
    const bool seenThroughGlassAfterScatter = path.hasFlag(PathFlags::transmission) && !path.hasFlag(PathFlags::deltaOnlyPath);
    if (path.getVertexIndex() == 0 || path.hasFlag(PathFlags::deltaOnlyPath) || (path.hasFlag(PathFlags::delta) && !seenThroughGlassAfterScatter))
    {
        envRadiance += directional_light_discs(ubo, rayDir);
        float rectT = 1e30;
        const float3 rectRadiance = ray_rect_lights(ubo, WorldRayOrigin(), rayDir, rectT);
        if (rectT < 1e30)
        {
            envRadiance = rectRadiance; // in front of the sky
            rectLight = true;
        }
    }

    if (any(envRadiance > float3(0.0)))
    {
        float misWeight = 1.0;
        if (!rectLight && path.getVertexIndex() > 0 && path.bsdfScatterPdf > 0.0 && pushConstant.skyboxTextureIndex != 0xFFFFFFFF)
        {
            EnvMapSampler sampler = Bridge::environmentSampler(ubo, pushConstant.skyboxTextureIndex, pushConstant.environmentImportanceIndex);
            float lightPdf = sampler.MIPDescentEvalPdf(rayDir) / float(ubo->lightCount + 1);
            float bsdfPdf = path.bsdfScatterPdf;
            misWeight = bsdfPdf*bsdfPdf / max(bsdfPdf*bsdfPdf + lightPdf*lightPdf, 1e-30);
        }

        envRadiance = FireflyFilter(envRadiance, pushConstant.fireflyFilterThreshold, path.fireflyFilterK);
        float3 envContrib = path.thp * envRadiance * misWeight;
        path.L.rgb += envContrib;

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

    path.terminate();
    payload = PathPayload::pack(path);
}
