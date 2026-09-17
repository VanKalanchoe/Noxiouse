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

        RGTextureDesc accumulationDesc;
        accumulationDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        const RGTextureHistory accumulation = m_renderGraph.GetHistoryTexture("Path Tracer Accumulation", accumulationDesc, 2);
        resources.PathTracerAccum[0] = accumulation.Textures[0];
        resources.PathTracerAccum[1] = accumulation.Textures[1];

        const bool dlssRRActive = m_dlssEnabled && (m_dlssMode != NRI::UpscaleMode::Off) && m_dlssRayReconstructionEnabled;
        const bool nrdPTActive = m_nrdPTDenoiser != NRI::NRDDiffuseDenoiser::Off;
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

        m_renderGraph.AddPass("Path Trace", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.PathTracerAccum[readIndex]);
                ReadIfValid(builder, resources.TLAS);
                // Optional RTXDI primary-surface lighting (RTXPT-style hybrid).
                if (frame.restirGIAdded)
                    builder.Read(resources.ReSTIRGIRaw);
                if (frame.nrdGIAdded)
                    builder.Read(resources.ReSTIRGIDenoised);
                if (frame.restirDIAdded)
                    builder.Read(resources.ReSTIRDIDirect);
                if (frame.nrdDIAdded)
                    builder.Read(resources.ReSTIRDIDenoised);
                builder.ColorTarget(resources.PathTracerAccum[writeIndex], NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                builder.SetRenderArea(frame.renderExtent);
            },
            [this, res = &resources, readIndex](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_pathTracerPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                glm::mat4 viewProj = uniformData.proj * uniformData.view;
                shaderio::PushConstantPathTracer ptPush{};
                ptPush.invViewProj = glm::inverse(viewProj);
                ptPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                ptPush.viewportSize = glm::vec2(rw, rh);
                ptPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                ptPush.maxBounces = 3;
                ptPush.accumulationTextureIndex = context.Slot(res->PathTracerAccum[readIndex]);
                ptPush.sampleCount = m_frame.pathTracerAccumulate ? m_pathTracerSampleCount : 1;
                ptPush.debugMode = m_frame.pathTracerAccumulate ? 19 : 18;
                ptPush.skyboxTextureIndex = m_environmentCubemap ? m_environmentCubemap->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                ptPush.denoiserMode = static_cast<uint32_t>(m_nrdPTDenoiser);
                // resolveFrameUniforms binds the denoised GI/DI results when their NRD passes run.
                ptPush.restirGIDiffuseTextureIndex = uniformData.restirGIDiffuseTextureIndex;
                ptPush.restirGIDenoiserMode = m_frame.nrdGIAdded ? static_cast<uint32_t>(m_nrdGIDenoiser) : 0u;
                ptPush.restirDIDirectLightingTextureIndex = uniformData.restirDIDirectLightingTextureIndex;
                ptPush.restirDIDenoiserMode = m_frame.nrdDIAdded ? static_cast<uint32_t>(m_nrdDIDenoiser) : 0u;
                cmd.pushData(&ptPush, sizeof(shaderio::PushConstantPathTracer));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // NRD path tracer denoising (fallback for hardware/preference without DLSS-RR).
        if (nrdPTActive && m_device->isNRDInitialized())
        {
            frame.ptNRDAdded = true;

            m_renderGraph.AddPass("NRD PT", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.PathTracerAccum[writeIndex]);
                    builder.Read(resources.GBufferVelocity);
                    builder.Read(resources.NRDNormalRoughness);
                    builder.Read(resources.ViewZ);
                    builder.Write(resources.PathTracerDenoised);
                    builder.RecordExclusive("NRD");
                },
                [this, res = &resources, writeIndex](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();

                    NRI::NRDDiffuseParams ptDenoiseParams{};
                    ptDenoiseParams.inDiffuseRadianceHitDist = &context.Texture(res->PathTracerAccum[writeIndex]);
                    ptDenoiseParams.inMotionVectors = &context.Texture(res->GBufferVelocity);
                    ptDenoiseParams.inNormalRoughness = &context.Texture(res->NRDNormalRoughness);
                    ptDenoiseParams.inViewZ = &context.Texture(res->ViewZ);
                    ptDenoiseParams.outDenoisedDiffuse = &context.Texture(res->PathTracerDenoised);
                    ptDenoiseParams.commandBuffer = &cmd;

                    ptDenoiseParams.view = uniformData.view;
                    ptDenoiseParams.proj = uniformData.nonJitteredProj;
                    ptDenoiseParams.prevView = uniformData.prevView;
                    ptDenoiseParams.prevProj = uniformData.prevProj;

                    ptDenoiseParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                    ptDenoiseParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                    ptDenoiseParams.resetHistory = m_isFirstFrame || m_frame.resetNRD || m_frame.pathTracerCameraMoved;

                    m_device->evaluateNRDDiffusePT(ptDenoiseParams, m_nrdPTDenoiser);
                    cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
                });

            if (m_nrdPTDenoiser == NRI::NRDDiffuseDenoiser::REBLUR && m_ycocgDecodePipeline)
            {
                // REBLUR's output is still YCoCg-encoded (LinearToYCoCg in PathTracer.slang): decode in place before
                // DLSS/tonemapping read it. RELAX never encodes YCoCg.
                m_renderGraph.AddPass("PT YCoCg Decode", RGPassFlags::None,
                    [&](RGBuilder& builder)
                    {
                        builder.Read(resources.PathTracerDenoised);
                        builder.Write(resources.PathTracerDenoised);
                    },
                    [this, res = &resources](RGPassContext& context)
                    {
                        NRI::CommandBuffer& cmd = context.Cmd();
                        cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_ycocgDecodePipeline);
                        shaderio::PushConstantYCoCgDecode decodePush{};
                        decodePush.readTextureIndex = context.Slot(res->PathTracerDenoised);
                        decodePush.writeTextureIndex = context.StorageSlot(res->PathTracerDenoised);
                        decodePush.width = m_frame.renderExtent.width;
                        decodePush.height = m_frame.renderExtent.height;
                        cmd.pushData(&decodePush, sizeof(shaderio::PushConstantYCoCgDecode));
                        cmd.dispatch((decodePush.width + 7) / 8, (decodePush.height + 7) / 8, 1);
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
                    builder.Read(resources.ReSTIRDIDirect);
                if (frame.nrdDIAdded)
                    builder.Read(resources.ReSTIRDIDenoised);
                if (frame.ddgiAdded)
                {
                    builder.Read(resources.DDGIIrradiance[frame.ddgiWriteIndex]);
                    builder.Read(resources.DDGIDistance[frame.ddgiWriteIndex]);
                }
                ReadIfValid(builder, resources.TLAS);
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
                lightingPush.invViewProj = glm::inverse(viewProj);
                lightingPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                lightingPush.visibilityTextureIndex = context.Slot(res->Visibility);
                lightingPush.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                lightingPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                lightingPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                lightingPush.gbufferEmissionIndex = context.Slot(res->GBufferEmission);
                lightingPush.depthTextureIndex = context.Slot(res->Depth);
                lightingPush.viewportSize = glm::vec2(rw, rh);
                lightingPush.debugMode = m_debugMode;
                lightingPush.gbufferVelocityIndex = context.Slot(res->GBufferVelocity);
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
                lightingPush.restirDIDirectLightingTextureIndex = uniformData.restirDIDirectLightingTextureIndex;
                lightingPush.restirDIDenoiserMode = frame.nrdDIAdded ? static_cast<uint32_t>(m_nrdDIDenoiser) : 0;
                cmd.pushData(&lightingPush, sizeof(shaderio::PushConstantDeferredLighting));

                cmd.drawMeshTasks(1, 1, 1);
            });
    }
}
