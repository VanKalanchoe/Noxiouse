// Hybrid ray tracing passes: RT shadows (+ NRD SIGMA), RT reflections (+ NRD REBLUR/RELAX), DDGI probe update.
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"

namespace Nox
{
    void Renderer::addRTShadowPasses()
    {
        // Only the hybrid RT shadow path needs this; ReSTIR DI traces its own selected light, and raster PBR/IBL must not
        // pay for a full-resolution ray-query pass.
        if (uniformData.enableRTShadows == 0 || !m_shadowMaskPipeline)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_frame.rtShadowsAdded = true;

        // 1-SPP RT shadow -> raw shadow mask, plus NRD's view Z and packed normal/roughness guides.
        m_renderGraph.AddPass("RT Shadows", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                builder.ColorTarget(resources.RawShadowMask, NRI::LoadOP::clear, NRI::StoreOP::store, { 65504.0f, 1.0f, 0.0f, 0.0f });
                builder.ColorTarget(resources.ViewZ, NRI::LoadOP::clear, NRI::StoreOP::store, { 500000.0f, 0.0f, 0.0f, 0.0f });
                builder.ColorTarget(resources.NRDNormalRoughness, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                constexpr uint32_t attachmentCount = 3;
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_shadowMaskPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                for (uint32_t a = 0; a < attachmentCount; ++a)
                {
                    cmd.setColorBlendEnable(a, false);
                    cmd.setColorWriteMask(a, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                             NRI::ColorComponent::B | NRI::ColorComponent::A);
                }

                glm::mat4 viewProj = uniformData.proj * uniformData.view;
                shaderio::PushConstantShadowMask shadowPush{};
                shadowPush.invViewProj = glm::inverse(viewProj);
                shadowPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                shadowPush.depthTextureIndex = context.Slot(res->Depth);
                shadowPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                shadowPush.viewportSize = glm::vec2(rw, rh);
                shadowPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                cmd.pushData(&shadowPush, sizeof(shaderio::PushConstantShadowMask));

                cmd.drawMeshTasks(1, 1, 1);
            });

        if (!m_nrdShadowsEnabled || !m_device->isNRDInitialized())
            return;

        m_frame.nrdShadowsAdded = true;

        // NRD SIGMA: raw shadow mask -> denoised shadow mask.
        m_renderGraph.AddPass("NRD Shadows", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.RawShadowMask);
                builder.Read(resources.GBufferVelocity);
                builder.Read(resources.NRDNormalRoughness);
                builder.Read(resources.ViewZ);
                builder.Write(resources.DenoisedShadowMask);
                builder.RecordExclusive("NRD");
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                NRI::NRDShadowParams nrdParams{};
                nrdParams.inShadowData = &context.Texture(res->RawShadowMask);
                nrdParams.inMotionVectors = &context.Texture(res->GBufferVelocity);
                nrdParams.inNormalRoughness = &context.Texture(res->NRDNormalRoughness);
                nrdParams.inViewZ = &context.Texture(res->ViewZ);
                nrdParams.outDenoisedShadow = &context.Texture(res->DenoisedShadowMask);
                nrdParams.commandBuffer = &cmd;

                nrdParams.view = uniformData.view;
                nrdParams.proj = uniformData.nonJitteredProj;
                nrdParams.prevView = uniformData.prevView;
                nrdParams.prevProj = uniformData.prevProj;

                glm::vec3 lightDir = glm::vec3(0.0f, 1.0f, 0.0f);
                if (!m_lightBufferObjects.empty())
                {
                    lightDir = glm::normalize(glm::vec3(m_lightBufferObjects[0].direction));
                }
                nrdParams.lightDirection[0] = lightDir.x;
                nrdParams.lightDirection[1] = lightDir.y;
                nrdParams.lightDirection[2] = lightDir.z;

                nrdParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                nrdParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                nrdParams.resetHistory = m_isFirstFrame || m_frame.resetNRD;

                m_device->evaluateNRDShadows(nrdParams);
                // NRD binds its own descriptors.
                cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
            });
    }

    void Renderer::addRTReflectionPasses()
    {
        if (m_frame.runPathTracer || !m_reflectionPipeline || uniformData.enableRTReflections == 0)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_frame.rtReflectionsAdded = true;

        // 1-SPP GGX VNDF reflection -> raw reflection (radiance + hit distance).
        m_renderGraph.AddPass("RT Reflections", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferMaterial);
                builder.Read(resources.Visibility);
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                // Hit shading samples the DDGI atlases (through the uniforms) when DDGI runs.
                if (m_frame.ddgiAdded)
                {
                    builder.Read(resources.DDGIIrradiance[m_frame.ddgiWriteIndex]);
                    builder.Read(resources.DDGIDistance[m_frame.ddgiWriteIndex]);
                }
                builder.ColorTarget(resources.RawReflection, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 10000.0f });
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_reflectionPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                         NRI::ColorComponent::B | NRI::ColorComponent::A);

                glm::mat4 viewProj = uniformData.proj * uniformData.view;
                shaderio::PushConstantReflection reflPush{};
                reflPush.invViewProj = glm::inverse(viewProj);
                reflPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                reflPush.depthTextureIndex = context.Slot(res->Depth);
                reflPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                reflPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                reflPush.visibilityTextureIndex = context.Slot(res->Visibility);
                reflPush.viewportSize = glm::vec2(rw, rh);
                reflPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                reflPush.denoiserMode = static_cast<uint32_t>(m_nrdReflectionDenoiser);
                cmd.pushData(&reflPush, sizeof(shaderio::PushConstantReflection));

                cmd.drawMeshTasks(1, 1, 1);
            });

        if (m_nrdReflectionDenoiser == NRI::NRDReflectionDenoiser::Off || !m_device->isNRDInitialized())
            return;

        m_frame.nrdReflectionsAdded = true;

        // NRD REBLUR / RELAX specular: raw reflection -> denoised reflection.
        m_renderGraph.AddPass("NRD Reflections", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.RawReflection);
                builder.Read(resources.GBufferVelocity);
                builder.Read(resources.NRDNormalRoughness);
                builder.Read(resources.ViewZ);
                builder.Write(resources.DenoisedReflection);
                builder.RecordExclusive("NRD");
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                NRI::NRDReflectionParams reflParams{};
                reflParams.inSpecularRadianceHitDist = &context.Texture(res->RawReflection);
                reflParams.inMotionVectors = &context.Texture(res->GBufferVelocity);
                reflParams.inNormalRoughness = &context.Texture(res->NRDNormalRoughness);
                reflParams.inViewZ = &context.Texture(res->ViewZ);
                reflParams.outDenoisedSpecular = &context.Texture(res->DenoisedReflection);
                reflParams.commandBuffer = &cmd;

                reflParams.view = uniformData.view;
                reflParams.proj = uniformData.nonJitteredProj;
                reflParams.prevView = uniformData.prevView;
                reflParams.prevProj = uniformData.prevProj;

                reflParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                reflParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                reflParams.resetHistory = m_isFirstFrame || m_frame.resetNRD;

                m_device->evaluateNRDReflections(reflParams, m_nrdReflectionDenoiser);
                cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
            });
    }

    void Renderer::prepareDDGIFrame(FrameGraphResources& resources)
    {
        // DDGI ray-traces every probe against the scene TLAS, so it needs the hybrid RT master toggle too.
        FrameGraphState& frame = m_frame;
        const uint32_t totalDDGIProbes = frame.totalDDGIProbes;
        const bool runDDGI = !frame.runPathTracer &&
                             m_rayTracingEnabled &&
                             (m_ddgiEnabled || m_debugMode == 16 || m_debugMode == 17) &&
                             m_ddgiRadiancePipeline && m_ddgiBlendIrradiancePipeline && m_ddgiBlendDistancePipeline &&
                             (totalDDGIProbes > 0);
        if (!runDDGI)
            return;

        RenderGraph& graph = m_renderGraph;
        frame.ddgiAdded = true;

        const uint32_t probeRows = (totalDDGIProbes + DDGIProbesPerRow - 1) / DDGIProbesPerRow;

        RGTextureDesc rayData;
        rayData.Size = RGSize::Absolute;
        rayData.Width = m_ddgiRaysPerProbe; // one row of rays per probe
        rayData.Height = totalDDGIProbes;
        rayData.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        resources.DDGIRayData = graph.CreateTexture("DDGI Ray Data", rayData);

        // Probe atlases: 8x8 irradiance / 16x16 distance interior plus a 1-texel border per probe.
        RGTextureDesc irradiance;
        irradiance.Size = RGSize::Absolute;
        irradiance.Width = DDGIProbesPerRow * 10;
        irradiance.Height = probeRows * 10;
        irradiance.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        RGTextureDesc distance = irradiance;
        distance.Width = DDGIProbesPerRow * 18;
        distance.Height = probeRows * 18;
        distance.Format = NRI::ImageFormat::R16G16_SFLOAT;

        const RGTextureHistory irradianceHistory = graph.GetHistoryTexture("DDGI Irradiance", irradiance, 2);
        const RGTextureHistory distanceHistory = graph.GetHistoryTexture("DDGI Distance", distance, 2);
        for (uint32_t i = 0; i < 2; ++i)
        {
            resources.DDGIIrradiance[i] = irradianceHistory.Textures[i];
            resources.DDGIDistance[i] = distanceHistory.Textures[i];
        }

        // Atlases ping-pong: blend reads last frame's atlas and writes the other one, which lighting reads this frame.
        frame.ddgiReadIndex = m_ddgiHistoryIndex;
        frame.ddgiWriteIndex = 1 - m_ddgiHistoryIndex;
        m_ddgiHistoryIndex = frame.ddgiWriteIndex;
        // New atlases (first use, probe grid resized) or a requested reset: the blend ignores the previous atlas.
        frame.ddgiFirstFrame = m_isFirstFrame || irradianceHistory.WasReset || distanceHistory.WasReset ||
                               graph.WasHistoryReset(DDGIHistoryKey);
    }

    void Renderer::addDDGIPasses()
    {
        if (!m_frame.ddgiAdded)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        const uint32_t totalDDGIProbes = m_frame.totalDDGIProbes;
        const uint32_t probeRows = (totalDDGIProbes + DDGIProbesPerRow - 1) / DDGIProbesPerRow;
        const NRI::Extent2D radianceExtent = { m_ddgiRaysPerProbe, totalDDGIProbes };
        const NRI::Extent2D irradianceExtent = { DDGIProbesPerRow * 10, probeRows * 10 };
        const NRI::Extent2D distanceExtent = { DDGIProbesPerRow * 18, probeRows * 18 };
        const uint32_t readIndex = m_frame.ddgiReadIndex;
        const uint32_t writeIndex = m_frame.ddgiWriteIndex;

        m_renderGraph.PushGroup("DDGI");

        // 1. Trace radiance rays: width = rays per probe, height = probe count.
        m_renderGraph.AddPass("DDGI Radiance", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                // Multi-bounce: hits sample the atlases bound in the uniforms.
                builder.Read(resources.DDGIIrradiance[writeIndex]);
                builder.Read(resources.DDGIDistance[writeIndex]);
                builder.ColorTarget(resources.DDGIRayData, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1000.0f });
                builder.SetRenderArea(radianceExtent);
            },
            [this, totalDDGIProbes](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_ddgiRadiancePipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                         NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantDDGIRadiance radPush{};
                radPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                radPush.raysPerProbe = m_ddgiRaysPerProbe;
                radPush.probeCountTotal = totalDDGIProbes;
                radPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                cmd.pushData(&radPush, sizeof(shaderio::PushConstantDDGIRadiance));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // 2. Blend irradiance atlas.
        m_renderGraph.AddPass("DDGI Blend Irradiance", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.DDGIRayData);
                builder.Read(resources.DDGIIrradiance[readIndex]);
                builder.ColorTarget(resources.DDGIIrradiance[writeIndex], NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                builder.SetRenderArea(irradianceExtent);
            },
            [this, res = &resources, totalDDGIProbes, readIndex](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_ddgiBlendIrradiancePipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                         NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantDDGIBlend blendIrrPush{};
                blendIrrPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                blendIrrPush.rayDataTextureIndex = context.Slot(res->DDGIRayData);
                blendIrrPush.prevAtlasTextureIndex = context.Slot(res->DDGIIrradiance[readIndex]);
                blendIrrPush.probesPerRow = DDGIProbesPerRow;
                blendIrrPush.raysPerProbe = m_ddgiRaysPerProbe;
                blendIrrPush.probeCountTotal = totalDDGIProbes;
                blendIrrPush.hysteresis = m_ddgiHysteresis;
                blendIrrPush.firstFrame = m_frame.ddgiFirstFrame ? 1 : 0;
                blendIrrPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                cmd.pushData(&blendIrrPush, sizeof(shaderio::PushConstantDDGIBlend));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // 3. Blend distance atlas.
        m_renderGraph.AddPass("DDGI Blend Distance", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.DDGIRayData);
                builder.Read(resources.DDGIDistance[readIndex]);
                builder.ColorTarget(resources.DDGIDistance[writeIndex], NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                builder.SetRenderArea(distanceExtent);
            },
            [this, res = &resources, totalDDGIProbes, readIndex](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_ddgiBlendDistancePipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G);

                shaderio::PushConstantDDGIBlend blendDistPush{};
                blendDistPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                blendDistPush.rayDataTextureIndex = context.Slot(res->DDGIRayData);
                blendDistPush.prevAtlasTextureIndex = context.Slot(res->DDGIDistance[readIndex]);
                blendDistPush.probesPerRow = DDGIProbesPerRow;
                blendDistPush.raysPerProbe = m_ddgiRaysPerProbe;
                blendDistPush.probeCountTotal = totalDDGIProbes;
                blendDistPush.hysteresis = m_ddgiHysteresis;
                blendDistPush.firstFrame = m_frame.ddgiFirstFrame ? 1 : 0;
                blendDistPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                cmd.pushData(&blendDistPush, sizeof(shaderio::PushConstantDDGIBlend));

                cmd.drawMeshTasks(1, 1, 1);
            });

        m_renderGraph.PopGroup();
    }
}
