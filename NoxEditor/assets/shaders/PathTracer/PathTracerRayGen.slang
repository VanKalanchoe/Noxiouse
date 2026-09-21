#pragma once

#include "PathPayload.slang"
#include "PathTracerBridge.slang"
#include "../PathTracerSignals.slang"

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
        rrMotion[pixelPos] = float2(0.0);
        rrSpecularMotion[pixelPos] = float2(0.0);
        rrSpecularHitDistance[pixelPos] = 65504.0;
    }

    // Iterative Wavefront/Bounce Loop
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

    // De-modulation factors and NRD packing
    float3 surfaceDiffFactor, surfaceSpecFactor;
    float surfaceRoughness;
    PT_SurfaceFactors(ubo, int2(pixelPos), pushConstant.depthTextureIndex, pushConstant.gbufferAlbedoIndex, pushConstant.gbufferNormalIndex,
                      pushConstant.gbufferMaterialIndex, surfaceDiffFactor, surfaceSpecFactor, surfaceRoughness);

    float4 outDiffuse = NRD_PackSignal(pushConstant.denoiserMode, path.diffuseRadiance / surfaceDiffFactor,
                                       path.primaryLobe == LobeType::DiffuseReflection ? path.firstBounceHitDist : 0.0, path.primaryViewZ, 1.0);
    float4 outSpecular = NRD_PackSignal(pushConstant.denoiserMode, path.specularRadiance / surfaceSpecFactor,
                                        (path.primaryLobe & (LobeType::Specular | LobeType::Delta)) != 0 ? path.firstBounceHitDist : 0.0, path.primaryViewZ, surfaceRoughness);

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
            const float rrMaxRadiance = 4096.0 * 0.18;
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
