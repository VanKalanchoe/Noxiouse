#pragma once

#include "PathPayload.slang"
#include "PathTracerBridge.slang"
#include "../PathTracerSignals.slang"
#include "../HeightFog.slang"

[[vk::push_constant]]
ConstantBuffer<PushConstantPathTracer> pushConstant;

[shader("raygeneration")]
void rayGenMain()
{
    uint2 pixelPos = DispatchRaysIndex().xy;
    if (pixelPos.x >= uint(pushConstant.viewportSize.x) || pixelPos.y >= uint(pushConstant.viewportSize.y))
        return;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    RaytracingAccelerationStructure as = RaytracingAccelerationStructure(ubo->tlasDeviceAddress);

    uint rngState = Bridge::initRNG(pixelPos, pushConstant.frameIndex);

    float pixelSpreadAngle;
    Ray cameraRay = Bridge::computeCameraRay(pixelPos, rngState, pushConstant, ubo, pixelSpreadAngle);
    PathState path = PathState::init(pixelPos, pixelSpreadAngle);
    path.origin = cameraRay.origin;
    path.dir = cameraRay.dir;

    // Primary misses leave valid neutral RR guides instead of stale data from the previous frame.
    if (pushConstant.rrDiffuseAlbedoStorageIndex != 0xFFFFFFFF)
    {
        RWTexture2D<float4> rrDiffuse = RWTexture2D<float4>.Handle(uint2(pushConstant.rrDiffuseAlbedoStorageIndex, 0));
        RWTexture2D<float4> rrSpecular = RWTexture2D<float4>.Handle(uint2(pushConstant.rrSpecularAlbedoStorageIndex, 0));
        RWTexture2D<float4> rrNormalRoughness = RWTexture2D<float4>.Handle(uint2(pushConstant.rrNormalRoughnessStorageIndex, 0));
        RWTexture2D<float> rrDepth = RWTexture2D<float>.Handle(uint2(pushConstant.rrDepthStorageIndex, 0));
        RWTexture2D<float2> rrMotion = RWTexture2D<float2>.Handle(uint2(pushConstant.rrMotionStorageIndex, 0));
        RWTexture2D<float2> rrSpecularMotion = RWTexture2D<float2>.Handle(uint2(pushConstant.rrSpecularMotionStorageIndex, 0));
        RWTexture2D<float> rrSpecularHitDistance = RWTexture2D<float>.Handle(uint2(pushConstant.rrSpecularHitDistanceStorageIndex, 0));
        rrDiffuse[pixelPos] = float4(0.0);
        rrSpecular[pixelPos] = float4(0.0);
        rrNormalRoughness[pixelPos] = float4(0.0, 0.0, 1.0, 0.0);
        rrDepth[pixelPos] = 0.0; // reverse-Z invalid/background value
        // A primary miss is the sky: it moves with the camera's ROTATION only (a direction, w = 0), not its position.
        // Zero here told DLSS-RR the sky never moves, so it smeared and shimmered whenever the camera turned.
        const float4 skyCurrClip = mul(ubo->nonJitteredProj, mul(ubo->view, float4(cameraRay.dir, 0.0)));
        const float4 skyPrevClip = mul(ubo->prevProj, mul(ubo->prevView, float4(cameraRay.dir, 0.0)));
        const float2 skyMotion = (skyPrevClip.xy / skyPrevClip.w - skyCurrClip.xy / skyCurrClip.w) * float2(0.5, -0.5);
        rrMotion[pixelPos] = skyMotion;
        rrSpecularMotion[pixelPos] = skyMotion;
        rrSpecularHitDistance[pixelPos] = 0.0; // none (RTXPT's default)
    }

    // Iterative Wavefront/Bounce Loop, then the branch the first smooth glass left (PathTracerHit.slang), if any.
    for (;;)
    {
    while (path.isActive())
    {
        RayDesc ray;
        ray.Origin = path.origin;
        ray.Direction = path.dir;
        // Scatter origins are already moved off the surface with RTXPT's scale-aware ULP offset.
        // Keeping the old 1 mm TMin would still skip the painting immediately behind Bistro's glass.
        ray.TMin = 0.0;
        ray.TMax = 1e9;

        PathPayload payload = PathPayload::pack(path);
        TraceRay(
            as,
            RAY_FLAG_NONE,
            0xFF,
            0, // rayContributionToHitGroupIndex
            1, // multiplierForGeometryContributionToHitGroupIndex
            0, // missShaderIndex
            ray,
            payload
        );
        path = PathPayload::unpack(payload);
    }
    if (!path.hasBranch())
        break;
    path.resumeBranch();
    }

    // RTXPT's last steps on RR's guides (PostProcess.hlsl DENOISER_PREPARE_INPUTS, MIX_STABLE_RADIANCE): the stable radiance -- the
    // emitters and sky the camera sees directly, through glass and in mirrors, noise-free -- pulls the diffuse albedo guide towards
    // its own tone-mapped colour, as far as it outshines the albedo; RR then keeps it as detail (a window's sky reflection, a lamp)
    // instead of filtering it as lighting over the surface the guides describe. RTXPT applies its Reinhard to radiance in its units, in
    // which the Bistro sits near exposure 1; here the radiance as exposed, the same picture at any light scale. And no pixel has both
    // albedo guides near 0.
    if (pushConstant.rrDiffuseAlbedoStorageIndex != 0xFFFFFFFF)
    {
        RWTexture2D<float4> rrDiffuse = RWTexture2D<float4>.Handle(uint2(pushConstant.rrDiffuseAlbedoStorageIndex, 0));
        RWTexture2D<float4> rrSpecular = RWTexture2D<float4>.Handle(uint2(pushConstant.rrSpecularAlbedoStorageIndex, 0));
        float3 diffuseAlbedo = rrDiffuse[pixelPos].rgb;
        const float3 specularAlbedo = rrSpecular[pixelPos].rgb;
        const float exposure = scene_exposure(ubo, pushConstant.autoExposureTextureIndex, pushConstant.exposureCompensation,
                                              pushConstant.autoExposureMinEV, pushConstant.autoExposureMaxEV);
        const float3 stable = max(path.stableL * exposure, float3(0.0));
        const float stablePeak = max(1e-7, max(stable.x, max(stable.y, stable.z))); // RTXPT's ReinhardMax
        const float3 stableAlbedo = sqrt(stable * (stablePeak / (stablePeak + 1.0) / stablePeak));
        const float stableAlbedoAverage = Average(stableAlbedo);
        const float3 stableAlbedoGreyMix = lerp(stableAlbedo, float3(0.5), 0.2); // never 0
        diffuseAlbedo = lerp(diffuseAlbedo, stableAlbedoGreyMix,
                             stableAlbedoAverage / (Average(diffuseAlbedo) + sqrt(stableAlbedoAverage) + 1e-7));
        if (Average(diffuseAlbedo + specularAlbedo) < 0.05)
            diffuseAlbedo += 0.05;
        rrDiffuse[pixelPos] = float4(saturate(diffuseAlbedo), 1.0);

        // RTXPT's specular motion vectors (PostProcess.hlsl, "compute specular MVs from hitT"): where the guide surface is smooth
        // (roughness below 0.25) and its specular hit distance is known, what it reflects moves as its mirror image does, not with the
        // surface -- RR moved reflections with the floor, the table, the puddle under them, and they smeared as the camera moved. A
        // glass split wrote its own (the pane as the mirror, PathTracerHit.slang writeSplitRRGuides).
        if (!path.hasFlag(PathFlags::rrGuidesDone))
        {
            const float specularHitDistance = RWTexture2D<float>.Handle(uint2(pushConstant.rrSpecularHitDistanceStorageIndex, 0))[pixelPos];
            const float4 normalRoughness = RWTexture2D<float4>.Handle(uint2(pushConstant.rrNormalRoughnessStorageIndex, 0))[pixelPos];
            const float depth = RWTexture2D<float>.Handle(uint2(pushConstant.rrDepthStorageIndex, 0))[pixelPos];
            if (specularHitDistance > 1e-3 && normalRoughness.w < 0.25 && depth > 0.0)
            {
                // The guide surface's point on this pixel's ray (its depth was written with the jittered projection).
                const float2 ndc = (float2(pixelPos) + 0.5) / pushConstant.viewportSize * float2(2.0, -2.0) + float2(-1.0, 1.0);
                const float4 world = mul(ubo->invViewProj, float4(ndc, depth, 1.0));
                const float3 hitPosition = world.xyz / world.w;
                const float3 rayDirection = normalize(hitPosition - ubo->cameraWorldPos.xyz);
                const float3 reflection = reflect(rayDirection, normalize(normalRoughness.xyz)) * specularHitDistance;
                RWTexture2D<float2>.Handle(uint2(pushConstant.rrSpecularMotionStorageIndex, 0))[pixelPos] =
                    Bridge::rrSpecularMotionVector(ubo, hitPosition, rayDirection, reflection);
            }
        }
    }

    // The air and the height fog between the camera and what its ray hit first (the sky: the height fog alone, as the skybox
    // has it), on each sample before it is accumulated or denoised -- it is linear (color * transmittance + scattered light).
    {
        Texture2D<float> depthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
        const float depth = depthTex.Load(int3(pixelPos, 0));
        float4 fog;
        if (depth > 0.0)
        {
            const float2 ndc = (float2(pixelPos) + 0.5) / pushConstant.viewportSize * float2(2.0, -2.0) + float2(-1.0, 1.0);
            const float4 world = mul(ubo->invViewProj, float4(ndc, depth, 1.0));
            fog = sceneFogTerms(ubo, world.xyz / world.w);
        }
        else
        {
            fog = heightFogTerms(ubo, cameraRay.dir, 1e30);
        }
        path.L.rgb = path.L.rgb * fog.a + fog.rgb;
        path.diffuseRadiance *= fog.a;
        path.specularRadiance *= fog.a;
    }

    // De-modulation factors and NRD packing
    float3 surfaceDiffFactor, surfaceSpecFactor;
    float surfaceRoughness;
    PT_SurfaceFactors(ubo, int2(pixelPos), pushConstant.depthTextureIndex, pushConstant.gbufferAlbedoIndex, pushConstant.gbufferNormalIndex,
                      pushConstant.gbufferMaterialIndex, surfaceDiffFactor, surfaceSpecFactor, surfaceRoughness);

    float4 outDiffuse = NRD_PackSignal(pushConstant.denoiserMode, path.diffuseRadiance / surfaceDiffFactor,
                                       path.primaryLobe == LobeType::DiffuseReflection ? path.firstBounceHitDist : 0.0, path.primaryViewZ, 1.0, ubo->unitsPerMeter);
    float4 outSpecular = NRD_PackSignal(pushConstant.denoiserMode, path.specularRadiance / surfaceSpecFactor,
                                        (path.primaryLobe & (LobeType::Specular | LobeType::Delta)) != 0 ? path.firstBounceHitDist : 0.0, path.primaryViewZ, surfaceRoughness, ubo->unitsPerMeter);

    float4 outColor;
    if (pushConstant.denoiserMode != NRD_SIGNAL_RAW)
    {
        // With NRD denoising, the accumulation texture stores the primary emission, and PTComposite adds back denoised diffuse + specular
        float3 primaryEmission = max(path.L.rgb - (path.diffuseRadiance + path.specularRadiance), float3(0.0));
        outColor = float4(primaryEmission, 1.0);
    }
    else
    {
        float3 radiance = path.L.rgb;
        // RTXPT's DLSSRR_PrepareInputs default clamps the RR input to
        // DLSSRRBrightnessClampK (4096) * pre-exposed middle gray (0.18 at default exposure).
        if (pushConstant.rrDiffuseAlbedoStorageIndex != 0xFFFFFFFF)
        {
            const float rrMaxRadiance = 4096.0 * 0.18 / scene_exposure(ubo, pushConstant.autoExposureTextureIndex, pushConstant.exposureCompensation,
                                                                       pushConstant.autoExposureMinEV, pushConstant.autoExposureMaxEV);
            float peak = max(radiance.x, max(radiance.y, radiance.z));
            if (peak > rrMaxRadiance)
                radiance *= rrMaxRadiance / peak;
        }
        if (pushConstant.debugMode == 18 || pushConstant.sampleCount <= 1)
        {
            outColor = float4(radiance, 1.0);
        }
        else
        {
            // Progressive Accumulation
            Texture2D accumTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.accumulationTextureIndex, 0));
            float4 prevColor = accumTex.Load(int3(pixelPos, 0));
            float blendFactor = 1.0 / float(pushConstant.sampleCount);
            outColor = float4(lerp(prevColor.rgb, radiance, blendFactor), 1.0);
        }
    }

    RWTexture2D<float4> outAccum = RWTexture2D<float4>.Handle(uint2(pushConstant.outputAccumIndex, 0));
    RWTexture2D<float4> outDiffTex = RWTexture2D<float4>.Handle(uint2(pushConstant.outputDiffuseIndex, 0));
    RWTexture2D<float4> outSpecTex = RWTexture2D<float4>.Handle(uint2(pushConstant.outputSpecularIndex, 0));

    outAccum[pixelPos] = outColor;
    outDiffTex[pixelPos] = outDiffuse;
    outSpecTex[pixelPos] = outSpecular;

}
