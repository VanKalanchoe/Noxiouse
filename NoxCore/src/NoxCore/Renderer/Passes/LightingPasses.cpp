// Lighting passes: plain path tracer (+ NRD, YCoCg decode) and deferred lighting. Exactly one lighting path runs per
// frame: ReSTIR PT (ReSTIRPasses.cpp), else the plain path tracer, else deferred lighting.
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"

namespace Nox
{
    void Renderer::addPathTracerPasses()
    {
        FrameGraphState& frame = m_frame;
        if (frame.restirPTActive || !frame.runPathTracer || !m_pathTracerPipeline)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        frame.pathTracerActive = true;

        RGTextureDesc importanceDesc;
        importanceDesc.Size = RGSize::Absolute;
        importanceDesc.Width = importanceDesc.Height = 512;
        importanceDesc.Format = NRI::ImageFormat::R32_SFLOAT;
        importanceDesc.Usage = NRI::TextureUsage::Storage;
        importanceDesc.MipLevels = 10;
        const auto importanceHistory = m_renderGraph.GetHistoryTexture("PT Environment Importance", importanceDesc, 1);
        const RGTexture environmentImportance = importanceHistory.Textures[0];
        if (importanceHistory.WasReset && m_environmentCubemap)
        {
            for (uint32_t mip = 0; mip < 10; ++mip)
            {
                m_renderGraph.AddPass(mip == 0 ? "PT Environment Importance" : "PT Environment Reduce", RGPassFlags::None,
                    [&, mip](RGBuilder& builder)
                    {
                        if (mip > 0) builder.Read(environmentImportance);
                        builder.Write(environmentImportance, RGTextureAccess::StorageWrite);
                    },
                    [this, environmentImportance, mip](RGPassContext& context)
                    {
                        auto& cmd = context.Cmd();
                        cmd.bindPipeline(NRI::PipelineBindPoint::Compute, mip == 0 ? *m_ptEnvironmentBuildPipeline : *m_ptEnvironmentReducePipeline);
                        shaderio::PushConstantPTEnvironment pc{};
                        pc.sourceIndex = uniformData.imageHeapIndexOffset + (mip == 0 ? m_environmentCubemap->GetDescriptorIndexSlot() : context.Slot(environmentImportance));
                        pc.outputIndex = context.StorageSlot(environmentImportance, mip);
                        pc.sourceMip = mip == 0 ? 0 : mip - 1;
                        pc.dimension = 512 >> mip;
                        cmd.pushData(&pc, sizeof(pc));
                        cmd.dispatch((pc.dimension + 7) / 8, (pc.dimension + 7) / 8, 1);
                    });
            }
        }

        RGTextureDesc accumulationDesc;
        accumulationDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        accumulationDesc.Usage = NRI::TextureUsage::Storage;
        const RGTextureHistory accumulation = m_renderGraph.GetHistoryTexture("Path Tracer Accumulation", accumulationDesc, 2);
        resources.PathTracerAccum[0] = accumulation.Textures[0];
        resources.PathTracerAccum[1] = accumulation.Textures[1];

        const bool dlssRRActive = m_dlssEnabled && (m_dlssMode != NRI::UpscaleMode::Off) &&
            m_dlssRayReconstructionEnabled && m_device->isDLSSRayReconstructionSupported() && m_rrGuidesPipeline;
        const bool dlssActive = m_dlssEnabled && (m_dlssMode != NRI::UpscaleMode::Off) && m_device->isDLSSSupported();
        // NRD denoises the path tracer's output this frame: it is packed for it (and decoded after), plain without it.
        const bool nrdPTActive = m_nrdPTDenoiser != NRI::NRDDiffuseDenoiser::Off && m_device->isNRDInitialized();
        frame.pathTracerCameraMoved = (uniformData.view != m_pathTracerPrevView);

        // With DLSS-RR or NRD denoising 1-SPP per frame via motion vectors, our own progressive accumulation would double
        // up on temporal blending: accumulate only when neither handles it, the camera is static and the accumulation
        // textures still hold last frame's result.
        frame.pathTracerAccumulate = m_pathTracingAccumulation && (m_debugMode != 18) && !dlssRRActive && !nrdPTActive;
        if (frame.pathTracerCameraMoved || !frame.pathTracerAccumulate || accumulation.WasReset)
            m_pathTracerSampleCount = 0;
        m_pathTracerPrevView = uniformData.view;
        m_pathTracerSampleCount++;

        frame.pathTracerWriteIndex = m_pathTracerSampleCount % 2;
        frame.pathTracerReadIndex = 1 - frame.pathTracerWriteIndex;
        const uint32_t writeIndex = frame.pathTracerWriteIndex;
        const uint32_t readIndex = frame.pathTracerReadIndex;

        m_renderGraph.PushGroup("Path Tracer");

        m_renderGraph.AddPass("Path Trace", RGPassFlags::RayTracing,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.PathTracerAccum[readIndex]);
                if (m_environmentCubemap) builder.Read(environmentImportance);
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                // Optional RTXDI primary-surface lighting (RTXPT-style hybrid).
                if (frame.restirGIAdded)
                    builder.Read(resources.ReSTIRGIRaw);
                if (frame.nrdGIAdded)
                    builder.Read(resources.ReSTIRGIDenoised);
                if (frame.restirDIAdded)
                {
                    builder.Read(resources.ReSTIRDIDiffuse);
                    builder.Read(resources.ReSTIRDISpecular);
                }
                if (frame.nrdDIAdded)
                {
                    builder.Read(resources.ReSTIRDIDiffuseDenoised);
                    builder.Read(resources.ReSTIRDISpecularDenoised);
                }
                // The G-buffer surface the NRD signals are de-modulated by.
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferAlbedo);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferMaterial);

                builder.Write(resources.PathTracerAccum[writeIndex], RGTextureAccess::StorageWrite);
                builder.Write(resources.PathTracerDiffuse, RGTextureAccess::StorageWrite);
                builder.Write(resources.PathTracerSpecular, RGTextureAccess::StorageWrite);
                if (dlssRRActive)
                {
                    builder.Write(resources.RRDiffuseAlbedo, RGTextureAccess::StorageWrite);
                    builder.Write(resources.RRSpecularAlbedo, RGTextureAccess::StorageWrite);
                    builder.Write(resources.RRNormalRoughness, RGTextureAccess::StorageWrite);
                    builder.Write(resources.RRPathDepth, RGTextureAccess::StorageWrite);
                    builder.Write(resources.RRPathMotionVectors, RGTextureAccess::StorageWrite);
                    builder.Write(resources.RRSpecularMotionVectors, RGTextureAccess::StorageWrite);
                    builder.Write(resources.RRSpecularHitDistance, RGTextureAccess::StorageWrite);
                }
            },
            [this, res = &resources, readIndex, writeIndex, nrdPTActive, dlssRRActive, dlssActive, environmentImportance](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::RayTracing, *m_pathTracerPipeline);

                glm::mat4 viewProj = uniformData.proj * uniformData.view;
                shaderio::PushConstantPathTracer ptPush{};
                ptPush.invViewProj = uniformData.invViewProj;
                ptPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                ptPush.viewportSize = glm::vec2(rw, rh);
                ptPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                // RTXPT applies its Balanced preset on startup (18 total, 2 diffuse).
                ptPush.maxBounces = 18;
                ptPush.maxDiffuseBounces = 2;
                // RTXPT realtime default: 0.10 * sqrt(pre-exposed middle gray) * 1000.
                // Nox does not pre-expose the path buffers, so use middle gray directly.
                ptPush.fireflyFilterThreshold = 0.10f * std::sqrt(0.18f) * 1000.0f;
                ptPush.environmentImportanceIndex = m_environmentCubemap ? context.Slot(environmentImportance) : 0xFFFFFFFF;
                ptPush.accumulationTextureIndex = context.Slot(res->PathTracerAccum[readIndex]);
                ptPush.sampleCount = m_frame.pathTracerAccumulate ? m_pathTracerSampleCount : 1;
                ptPush.debugMode = m_frame.pathTracerAccumulate ? 19 : 18;
                ptPush.skyboxTextureIndex = m_environmentCubemap ? m_environmentCubemap->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                ptPush.denoiserMode = nrdPTActive ? static_cast<uint32_t>(m_nrdPTDenoiser) : 0u;
                // resolveFrameUniforms binds the denoised GI/DI results when their NRD passes run.
                ptPush.restirGIDiffuseTextureIndex = uniformData.restirGIDiffuseTextureIndex;
                ptPush.restirGIDenoiserMode = m_frame.nrdGIAdded ? static_cast<uint32_t>(m_nrdGIDenoiser) : 0u;
                ptPush.restirDIDiffuseTextureIndex = uniformData.restirDIDiffuseTextureIndex;
                ptPush.restirDISpecularTextureIndex = uniformData.restirDISpecularTextureIndex;
                ptPush.restirDIDenoiserMode = m_frame.nrdDIAdded ? static_cast<uint32_t>(m_nrdDIDenoiser) : 0u;
                ptPush.depthTextureIndex = context.Slot(res->Depth);
                ptPush.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                ptPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                ptPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);

                ptPush.outputAccumIndex = context.StorageSlot(res->PathTracerAccum[writeIndex]);
                ptPush.outputDiffuseIndex = context.StorageSlot(res->PathTracerDiffuse);
                ptPush.outputSpecularIndex = context.StorageSlot(res->PathTracerSpecular);
                ptPush.rrDiffuseAlbedoStorageIndex = dlssRRActive ? context.StorageSlot(res->RRDiffuseAlbedo) : 0xFFFFFFFF;
                ptPush.rrSpecularAlbedoStorageIndex = dlssRRActive ? context.StorageSlot(res->RRSpecularAlbedo) : 0xFFFFFFFF;
                ptPush.rrNormalRoughnessStorageIndex = dlssRRActive ? context.StorageSlot(res->RRNormalRoughness) : 0xFFFFFFFF;
                ptPush.rrDepthStorageIndex = dlssRRActive ? context.StorageSlot(res->RRPathDepth) : 0xFFFFFFFF;
                ptPush.rrMotionStorageIndex = dlssRRActive ? context.StorageSlot(res->RRPathMotionVectors) : 0xFFFFFFFF;
                ptPush.rrSpecularMotionStorageIndex = dlssRRActive ? context.StorageSlot(res->RRSpecularMotionVectors) : 0xFFFFFFFF;
                ptPush.rrSpecularHitDistanceStorageIndex = dlssRRActive ? context.StorageSlot(res->RRSpecularHitDistance) : 0xFFFFFFFF;
                // RTXPT uses a small stochastic camera-ray offset with RR, no undisclosed offset with DLSS-SR.
                ptPush.cameraRayJitterScale = dlssRRActive ? 0.1f : (dlssActive ? 0.0f : 1.0f);
                ptPush.neeCandidateSamples = 5;
                ptPush.neeFullSamples = 1;
                ptPush.environmentDiffuseMip = 2;
                cmd.pushData(&ptPush, sizeof(shaderio::PushConstantPathTracer));

                cmd.traceRays(
                    m_pathTracerSBTState.rayGen,
                    m_pathTracerSBTState.miss,
                    m_pathTracerSBTState.hitGroups,
                    m_pathTracerSBTState.callable,
                    m_frame.renderExtent.width,
                    m_frame.renderExtent.height,
                    1
                );

            });

        // NRD path tracer denoising (fallback for hardware/preference without DLSS-RR).
        if (nrdPTActive)
        {
            frame.ptNRDAdded = true;

            m_renderGraph.AddPass("NRD PT", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.PathTracerDiffuse);
                    builder.Read(resources.PathTracerSpecular);
                    builder.Read(resources.GBufferVelocity);
                    builder.Read(resources.NRDNormalRoughness);
                    builder.Read(resources.ViewZ);
                    builder.Write(resources.PathTracerDiffuseDenoised);
                    builder.Write(resources.PathTracerSpecularDenoised);
                    builder.RecordExclusive("NRD");
                },
                [this, res = &resources](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();

                    NRI::NRDDiffuseParams ptDenoiseParams{};
                    ptDenoiseParams.inDiffuseRadianceHitDist = &context.Texture(res->PathTracerDiffuse);
                    ptDenoiseParams.inSpecularRadianceHitDist = &context.Texture(res->PathTracerSpecular);
                    ptDenoiseParams.inMotionVectors = &context.Texture(res->GBufferVelocity);
                    ptDenoiseParams.inNormalRoughness = &context.Texture(res->NRDNormalRoughness);
                    ptDenoiseParams.inViewZ = &context.Texture(res->ViewZ);
                    ptDenoiseParams.outDenoisedDiffuse = &context.Texture(res->PathTracerDiffuseDenoised);
                    ptDenoiseParams.outDenoisedSpecular = &context.Texture(res->PathTracerSpecularDenoised);
                    ptDenoiseParams.commandBuffer = &cmd;

                    ptDenoiseParams.view = uniformData.view;
                    ptDenoiseParams.proj = uniformData.nonJitteredProj;
                    ptDenoiseParams.prevView = uniformData.prevView;
                    ptDenoiseParams.prevProj = uniformData.prevProj;

                    ptDenoiseParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                    ptDenoiseParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                    ptDenoiseParams.resetHistory = m_isFirstFrame || m_frame.resetNRD || m_frame.pathTracerCameraMoved;

                    m_device->evaluateNRDPathTracing(ptDenoiseParams, m_nrdPTDenoiser);
                    cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
                });

            // The pixel back from its denoised signals: primary emission + diffuse / specular modulated by the G-buffer surface.
            if (m_ptCompositePipeline)
            {
                m_renderGraph.AddPass("PT Composite", RGPassFlags::None,
                    [&](RGBuilder& builder)
                    {
                        builder.Read(resources.PathTracerAccum[writeIndex]);
                        builder.Read(resources.PathTracerDiffuseDenoised);
                        builder.Read(resources.PathTracerSpecularDenoised);
                        builder.Read(resources.Depth);
                        builder.Read(resources.GBufferAlbedo);
                        builder.Read(resources.GBufferNormal);
                        builder.Read(resources.GBufferMaterial);
                        builder.Write(resources.PathTracerDenoised, RGTextureAccess::StorageWrite);
                    },
                    [this, res = &resources, writeIndex](RGPassContext& context)
                    {
                        shaderio::PushConstantPTComposite push{};
                        push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                        push.emissionTextureIndex = context.Slot(res->PathTracerAccum[writeIndex]);
                        push.diffuseTextureIndex = context.Slot(res->PathTracerDiffuseDenoised);
                        push.specularTextureIndex = context.Slot(res->PathTracerSpecularDenoised);
                        push.depthTextureIndex = context.Slot(res->Depth);
                        push.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                        push.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                        push.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                        push.outputStorageIndex = context.StorageSlot(res->PathTracerDenoised);
                        push.denoiserMode = static_cast<uint32_t>(m_nrdPTDenoiser);
                        push.width = m_frame.renderExtent.width;
                        push.height = m_frame.renderExtent.height;

                        NRI::CommandBuffer& cmd = context.Cmd();
                        cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_ptCompositePipeline);
                        cmd.pushData(&push, sizeof(push));
                        cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                    });
            }
        }

        m_renderGraph.PopGroup();
    }

    void Renderer::addDeferredLightingPass()
    {
        if (m_frame.restirPTActive || m_frame.pathTracerActive || !m_deferredLightingPipeline)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const FrameGraphState& frame = m_frame;
        m_frame.deferredLightingAdded = true;

        m_renderGraph.AddPass("Deferred Lighting", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Visibility);
                builder.Read(resources.GBufferAlbedo);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferMaterial);
                builder.Read(resources.GBufferEmission);
                builder.Read(resources.Depth);
                if (frame.gbufferVelocityWritten && m_debugMode == 15)
                    builder.Read(resources.GBufferVelocity);
                // Entity ID debug view (through the uniforms).
                builder.Read(resources.Entity);
                // Only the outputs of features that run this frame.
                if (frame.rtShadowsAdded)
                    builder.Read(resources.RawShadowMask);
                if (frame.nrdShadowsAdded)
                    builder.Read(resources.DenoisedShadowMask);
                if (frame.rtReflectionsAdded)
                    builder.Read(resources.RawReflection);
                if (frame.nrdReflectionsAdded)
                    builder.Read(resources.DenoisedReflection);
                if (frame.restirGIAdded)
                    builder.Read(resources.ReSTIRGIRaw);
                if (frame.nrdGIAdded)
                    builder.Read(resources.ReSTIRGIDenoised);
                if (frame.restirDIAdded)
                {
                    builder.Read(resources.ReSTIRDIDiffuse);
                    builder.Read(resources.ReSTIRDISpecular);
                }
                if (frame.nrdDIAdded)
                {
                    builder.Read(resources.ReSTIRDIDiffuseDenoised);
                    builder.Read(resources.ReSTIRDISpecularDenoised);
                }
                if (frame.ddgiAdded)
                {
                    builder.Read(resources.DDGIIrradiance[frame.ddgiWriteIndex]);
                    builder.Read(resources.DDGIDistance[frame.ddgiWriteIndex]);
                }
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                builder.ColorTarget(resources.HDRScene, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                builder.SetRenderArea(frame.renderExtent);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const FrameGraphState& frame = m_frame;
                const float rw = static_cast<float>(frame.renderExtent.width);
                const float rh = static_cast<float>(frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_deferredLightingPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                glm::mat4 viewProj = uniformData.proj * uniformData.view;
                shaderio::PushConstantDeferredLighting lightingPush{};
                lightingPush.invViewProj = uniformData.invViewProj;
                lightingPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                lightingPush.visibilityTextureIndex = context.Slot(res->Visibility);
                lightingPush.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                lightingPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                lightingPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                lightingPush.gbufferEmissionIndex = context.Slot(res->GBufferEmission);
                lightingPush.depthTextureIndex = context.Slot(res->Depth);
                lightingPush.viewportSize = glm::vec2(rw, rh);
                lightingPush.debugMode = m_debugMode;
                lightingPush.gbufferVelocityIndex = frame.gbufferVelocityWritten && m_debugMode == 15
                    ? context.Slot(res->GBufferVelocity)
                    : 0xFFFFFFFF;
                lightingPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                // The denoised shadow mask / reflection is bound only when its NRD pass ran this frame (it runs only
                // together with its RT pass, e.g. not when the master hybrid RT toggle is off); otherwise the raw result.
                // Only report REBLUR/RELAX when the denoised texture is actually bound: the raw fallback is plain linear
                // RGB, and reporting the denoiser would make the shader wrongly YCoCg-decode it.
                lightingPush.shadowMaskTextureIndex = frame.nrdShadowsAdded
                    ? context.Slot(res->DenoisedShadowMask)
                    : (frame.rtShadowsAdded ? context.Slot(res->RawShadowMask) : 0);
                const uint32_t nrdShadowBit = frame.nrdShadowsAdded ? 1 : 0;
                const uint32_t nrdReflMode = frame.nrdReflectionsAdded ? static_cast<uint32_t>(m_nrdReflectionDenoiser) : 0;
                lightingPush.nrdShadowsEnabled = nrdShadowBit | (nrdReflMode << 1);
                lightingPush.reflectionTextureIndex = frame.nrdReflectionsAdded
                    ? context.Slot(res->DenoisedReflection)
                    : (frame.rtReflectionsAdded ? context.Slot(res->RawReflection) : 0);
                lightingPush.diffuseGIMode = uniformData.diffuseGIMode;
                lightingPush.restirGIDiffuseTextureIndex = uniformData.restirGIDiffuseTextureIndex;
                // The denoiser mode only when the denoised texture is bound (the raw result is plain linear color).
                lightingPush.restirGIDenoiserMode = frame.nrdGIAdded ? static_cast<uint32_t>(m_nrdGIDenoiser) : 0;
                lightingPush.directLightingMode = uniformData.directLightingMode;
                lightingPush.restirDIDiffuseTextureIndex = uniformData.restirDIDiffuseTextureIndex;
                lightingPush.restirDISpecularTextureIndex = uniformData.restirDISpecularTextureIndex;
                lightingPush.restirDIDenoiserMode = frame.nrdDIAdded ? static_cast<uint32_t>(m_nrdDIDenoiser) : 0;
                cmd.pushData(&lightingPush, sizeof(shaderio::PushConstantDeferredLighting));

                cmd.drawMeshTasks(1, 1, 1);
            });
    }
}
