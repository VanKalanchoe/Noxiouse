// Hybrid ray tracing passes: RT shadows (+ NRD SIGMA), RT reflections (+ NRD REBLUR/RELAX), DDGI probe update, screen-probe GI.
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"

namespace Nox
{
    void Renderer::addNRDGuidesPass()
    {
        // NRD's view Z and packed normal/roughness, once for every NRD denoiser of the frame (shadows, reflections, GI, direct
        // lighting, path tracing): whichever of them runs reads guides from this frame's G-buffer.
        const bool anyDenoiser = m_nrdShadowsEnabled || m_nrdReflectionDenoiser != Nox::NRDReflectionDenoiser::Off ||
                                 m_nrdGIDenoiser != Nox::NRDDiffuseDenoiser::Off || m_nrdDIDenoiser != Nox::NRDDiffuseDenoiser::Off ||
                                 m_nrdPTDenoiser != Nox::NRDDiffuseDenoiser::Off;
        if (!anyDenoiser || !isNRDInitialized() || !m_nrdGuidesPipeline)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_renderGraph.AddPass("NRD Guides", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                // The reflection layer's normal and roughness (gbuffer_reflection_layer): the lobe bits and the lobes.
                builder.Read(resources.GBufferMaterial);
                builder.Read(resources.GBufferLobes);
                builder.Read(resources.GBufferSpecular);
                builder.ColorTarget(resources.ViewZ, NRI_LOAD_OP_DONT_CARE, NRI_STORE_OP_STORE);
                builder.ColorTarget(resources.NRDNormalRoughness, NRI_LOAD_OP_DONT_CARE, NRI_STORE_OP_STORE);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_nrdGuidesPipeline);
                cmd.setCullMode(NRI_CULL_MODE_NONE);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                for (uint32_t attachment = 0; attachment < 2; ++attachment)
                {
                    cmd.setColorBlendEnable(attachment, false);
                    cmd.setColorWriteMask(attachment, nri::ColorComponent::R | nri::ColorComponent::G |
                                                      nri::ColorComponent::B | nri::ColorComponent::A);
                }

                shaderio::PushConstantNRDGuides push{};
                push.invViewProj = uniformData.invViewProj;
                push.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                push.depthTextureIndex = context.Slot(res->Depth);
                push.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                push.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                push.viewportSize = glm::vec2(static_cast<float>(m_frame.renderExtent.width), static_cast<float>(m_frame.renderExtent.height));
                cmd.pushData(&push, sizeof(push));
                nri::drawMeshTasks(cmd, 1, 1, 1);
            });
    }

    nri::Extent2D Renderer::reducedRTExtent() const
    {
        const uint32_t divisor = std::max(uniformData.rtSubsample, 1u);
        return { (m_frame.renderExtent.width + divisor - 1) / divisor, (m_frame.renderExtent.height + divisor - 1) / divisor };
    }

    RGTexture Renderer::createReducedRTTarget(const char* name, nri::ImageFormat format)
    {
        const nri::Extent2D extent = reducedRTExtent();
        RGTextureDesc desc;
        desc.Size = RGSize::Absolute;
        desc.Width = extent.width;
        desc.Height = extent.height;
        desc.Format = format;
        return m_renderGraph.CreateTexture(name, desc);
    }

    void Renderer::addRTUpsamplePass(const char* name, RGTexture source, RGTexture target, nri::Pipeline* pipeline, uint32_t mode)
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const nri::Extent2D sourceExtent = reducedRTExtent();
        m_renderGraph.AddPass(name, RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(source);
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.ColorTarget(target, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f });
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, source, pipeline, mode, sourceExtent](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
                cmd.setCullMode(NRI_CULL_MODE_NONE);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);

                shaderio::PushConstantRTUpsample push{};
                push.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                push.sourceTextureIndex = context.Slot(source);
                push.depthTextureIndex = context.Slot(res->Depth);
                push.normalTextureIndex = context.Slot(res->GBufferNormal);
                push.mode = mode;
                push.viewportSize = glm::vec2(static_cast<float>(m_frame.renderExtent.width), static_cast<float>(m_frame.renderExtent.height));
                push.sourceSize = glm::vec2(static_cast<float>(sourceExtent.width), static_cast<float>(sourceExtent.height));
                cmd.pushData(&push, sizeof(shaderio::PushConstantRTUpsample));
                nri::drawMeshTasks(cmd, 1, 1, 1);
            });
    }

    void Renderer::addRTShadowPasses()
    {
        // Only the hybrid RT shadow path needs this; ReSTIR DI traces its own selected light, and raster PBR/IBL must not
        // pay for a full-resolution ray-query pass.
        // The soft shadow mask is the sun's (updateLightBuffer); without a directional light every light traces its own rays in the
        // deferred lighting.
        // With the sun's cascaded shadow maps the mask is theirs (addShadowMapPasses).
        if (uniformData.enableRTShadows == 0 || !m_shadowMaskPipeline || m_shadowMaskLightIndex == ~0u || m_frame.shadowMapsAdded)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_frame.rtShadowsAdded = true;

        // At a lower ray tracing resolution the rays go into a reduced target, brought back below.
        const bool reduced = uniformData.rtSubsample > 1 && m_rtUpsamplePipelines[1];
        const RGTexture traceTarget = reduced ? createReducedRTTarget("RT Shadows (Reduced)", NRI_FORMAT_R16G16_SFLOAT) : resources.RawShadowMask;
        const nri::Extent2D traceExtent = reduced ? reducedRTExtent() : m_frame.renderExtent;

        // 1-SPP RT shadow -> raw shadow mask (NRD's guides: addNRDGuidesPass).
        m_renderGraph.AddPass("RT Shadows", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferMaterial);
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                builder.ColorTarget(traceTarget, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 65504.0f, 1.0f, 0.0f, 0.0f });
                builder.SetRenderArea(traceExtent);
            },
            [this, res = &resources](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                constexpr uint32_t attachmentCount = 1;
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_shadowMaskPipeline);
                cmd.setCullMode(NRI_CULL_MODE_NONE);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                for (uint32_t a = 0; a < attachmentCount; ++a)
                {
                    cmd.setColorBlendEnable(a, false);
                    cmd.setColorWriteMask(a, nri::ColorComponent::R | nri::ColorComponent::G |
                                             nri::ColorComponent::B | nri::ColorComponent::A);
                }

                glm::mat4 viewProj = uniformData.proj * uniformData.view;
                shaderio::PushConstantShadowMask shadowPush{};
                shadowPush.invViewProj = uniformData.invViewProj;
                shadowPush.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                shadowPush.depthTextureIndex = context.Slot(res->Depth);
                shadowPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                shadowPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                shadowPush.viewportSize = glm::vec2(rw, rh);
                shadowPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                cmd.pushData(&shadowPush, sizeof(shaderio::PushConstantShadowMask));

                nri::drawMeshTasks(cmd, 1, 1, 1);
            });
        // The penumbra distance does not average: each pixel takes its best matching traced sample.
        if (reduced)
            addRTUpsamplePass("RT Shadows Upsample", traceTarget, resources.RawShadowMask, &m_rtUpsamplePipelines[1], 1);

        if (!m_nrdShadowsEnabled || !isNRDInitialized())
            return;

        m_frame.nrdShadowsAdded = true;

        // NRD SIGMA: raw shadow mask -> denoised shadow mask.
        m_renderGraph.AddPass("NRD Shadows", RGPassFlags::AsyncCompute | RGPassFlags::OwnCommandBuffer,
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
                nri::CommandBuffer& cmd = context.Cmd();
                const glm::vec3 lightDir = m_shadowMaskLightIndex != ~0u ? m_shadowMaskLightDirection : glm::vec3(0.0f, 1.0f, 0.0f);
                const float dir[3] = { lightDir.x, lightDir.y, lightDir.z };
                evaluateNRDShadows(
                    cmd,
                    context.Texture(res->RawShadowMask),
                    context.Texture(res->GBufferVelocity),
                    context.Texture(res->NRDNormalRoughness),
                    context.Texture(res->ViewZ),
                    context.Texture(res->DenoisedShadowMask),
                    dir, glm::vec2(1.0f, 1.0f),
                    static_cast<uint32_t>(m_sceneFrameCounter),
                    m_isFirstFrame || m_frame.resetNRD);
                cmd.bindDescriptorHeaps(&m_resourceHeap, &m_samplerHeap);
            });
    }

    void Renderer::addRTReflectionPasses()
    {
        if (m_frame.runPathTracer || !m_reflectionPipeline || uniformData.enableRTReflections == 0)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_frame.rtReflectionsAdded = true;

        // 1-SPP GGX VNDF reflection -> raw reflection (radiance + hit distance); at a lower ray tracing resolution into a reduced
        // target, brought back below.
        const bool denoiseReflections = m_nrdReflectionDenoiser != Nox::NRDReflectionDenoiser::Off && isNRDInitialized();
        const bool reduced = uniformData.rtSubsample > 1 && m_rtUpsamplePipelines[0];
        const RGTexture traceTarget = reduced ? createReducedRTTarget("RT Reflections (Reduced)", NRI_FORMAT_R16G16B16A16_SFLOAT)
                                              : resources.RawReflection;
        const nri::Extent2D traceExtent = reduced ? reducedRTExtent() : m_frame.renderExtent;
        // The trace pass: into the reduced target, or (smoothFill > 0, after the upsample) every pixel smoother than smoothFill at the
        // render resolution over the upsampled result -- a smooth surface's reflection shifts with the pixel it is traced from, so
        // filling it from the neighbouring traced pixels (which change every frame) blurs it and makes sharp reflections wobble.
        auto addTrace = [&](const char* name, RGTexture target, nri::Extent2D extent, float smoothFill)
        {
            m_renderGraph.AddPass(name, RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.Depth);
                    builder.Read(resources.GBufferNormal);
                    builder.Read(resources.GBufferMaterial);
                    builder.Read(resources.GBufferLobes);    // the reflection layer (gbuffer_reflection_layer)
                    builder.Read(resources.GBufferSpecular);
                    ReadIfValid(builder, resources.GBufferAlbedo);
                    builder.Read(resources.Visibility);
                    ReadIfValid(builder, resources.TLAS);
                    ReadGpuScene(builder, resources);
                    // Hit shading samples the DDGI atlases (through the uniforms) when DDGI runs, and the screen-probe GI where the hit
                    // is on screen (SceneGI.slang).
                    if (m_frame.ddgiAdded)
                    {
                        ReadDDGI(builder, resources, m_frame.ddgiWriteIndex);
                    }
                    if (m_frame.screenProbeGIAdded)
                    {
                        builder.Read(resources.ScreenProbeGI[m_frame.screenProbeGIWriteIndex]);
                        builder.Read(resources.ScreenProbeGIDistance);
                    }
                    if (smoothFill > 0.0f)
                        builder.ColorTarget(target, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                    else
                        builder.ColorTarget(target, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 10000.0f });
                    builder.SetRenderArea(extent);
                },
                [this, res = &resources, denoiseReflections, smoothFill](RGPassContext& context)
                {
                    nri::CommandBuffer& cmd = context.Cmd();
                    const float rw = static_cast<float>(m_frame.renderExtent.width);
                    const float rh = static_cast<float>(m_frame.renderExtent.height);

                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_reflectionPipeline);
                    cmd.setCullMode(NRI_CULL_MODE_NONE);
                    cmd.setDepthTestEnable(false);
                    cmd.setDepthWriteEnable(false);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G |
                                             nri::ColorComponent::B | nri::ColorComponent::A);

                    shaderio::PushConstantReflection reflPush{};
                    reflPush.invViewProj = uniformData.invViewProj;
                    reflPush.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                    reflPush.depthTextureIndex = context.Slot(res->Depth);
                    reflPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                    reflPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                    reflPush.visibilityTextureIndex = context.Slot(res->Visibility);
                    reflPush.viewportSize = glm::vec2(rw, rh);
                    reflPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                    // Packed for the NRD pass that runs (the lighting unpacks the same way), plain without one.
                    reflPush.denoiserMode = denoiseReflections ? static_cast<uint32_t>(m_nrdReflectionDenoiser) : 0u;
                    reflPush.smoothFillRoughness = smoothFill;
                    reflPush.gbufferAlbedoIndex = res->GBufferAlbedo.IsValid() ? context.Slot(res->GBufferAlbedo) : 0xFFFFFFFFu;
                    cmd.pushData(&reflPush, sizeof(shaderio::PushConstantReflection));

                    nri::drawMeshTasks(cmd, 1, 1, 1);
                });
        };
        addTrace("RT Reflections", traceTarget, traceExtent, 0.0f);
        // Radiance blended from the matching samples around, the hit distance from the best one; then the smooth surfaces again at
        // full resolution.
        if (reduced)
        {
            addRTUpsamplePass("RT Reflections Upsample", traceTarget, resources.RawReflection, &m_rtUpsamplePipelines[0], 0);
            addTrace("RT Reflections (Smooth, Full Res)", resources.RawReflection, m_frame.renderExtent, RTFullResolutionSmoothRoughness);
        }

        if (!denoiseReflections)
            return;

        m_frame.nrdReflectionsAdded = true;

        // NRD REBLUR / RELAX specular: raw reflection -> denoised reflection.
        m_renderGraph.AddPass("NRD Reflections", RGPassFlags::AsyncCompute | RGPassFlags::OwnCommandBuffer,
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
                nri::CommandBuffer& cmd = context.Cmd();
                evaluateNRDReflections(
                    cmd, m_nrdReflectionDenoiser,
                    context.Texture(res->RawReflection),
                    context.Texture(res->GBufferVelocity),
                    context.Texture(res->NRDNormalRoughness),
                    context.Texture(res->ViewZ),
                    context.Texture(res->DenoisedReflection),
                    glm::vec2(1.0f, 1.0f),
                    static_cast<uint32_t>(m_sceneFrameCounter),
                    m_isFirstFrame || m_frame.resetNRD);
                cmd.bindDescriptorHeaps(&m_resourceHeap, &m_samplerHeap);
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
        rayData.Format = NRI_FORMAT_R16G16B16A16_SFLOAT;
        resources.DDGIRayData = graph.CreateTexture("DDGI Ray Data", rayData);

        // Probe atlases: 8x8 irradiance / 16x16 distance interior plus a 1-texel border per probe.
        RGTextureDesc irradiance;
        irradiance.Size = RGSize::Absolute;
        irradiance.Width = DDGIProbesPerRow * 10;
        irradiance.Height = probeRows * 10;
        irradiance.Format = NRI_FORMAT_R16G16B16A16_SFLOAT;
        irradiance.Usage = NRI_TEXTURE_USAGE_STORAGE; // written by the blends (compute, DDGIBlendIrradiance.slang)
        RGTextureDesc distance = irradiance;
        distance.Width = DDGIProbesPerRow * 18;
        distance.Height = probeRows * 18;
        distance.Format = NRI_FORMAT_R16G16_SFLOAT;

        const RGTextureHistory irradianceHistory = graph.GetHistoryTexture("DDGI Irradiance", irradiance, 2);
        const RGTextureHistory distanceHistory = graph.GetHistoryTexture("DDGI Distance", distance, 2);
        for (uint32_t i = 0; i < 2; ++i)
        {
            resources.DDGIIrradiance[i] = irradianceHistory.Textures[i];
            resources.DDGIDistance[i] = distanceHistory.Textures[i];
        }
        bool offsetsReset = false;
        if (m_ddgiRelocation && m_ddgiRelocationPipeline)
        {
            // Probe relocation: one texel per probe (its offset in cells), laid out like the atlases' probes.
            RGTextureDesc offsets;
            offsets.Size = RGSize::Absolute;
            offsets.Width = DDGIProbesPerRow;
            offsets.Height = probeRows;
            offsets.Format = NRI_FORMAT_R16G16B16A16_SFLOAT;
            const RGTextureHistory offsetsHistory = graph.GetHistoryTexture("DDGI Probe Offsets", offsets, 2);
            for (uint32_t i = 0; i < 2; ++i)
                resources.DDGIProbeOffsets[i] = offsetsHistory.Textures[i];
            offsetsReset = offsetsHistory.WasReset;
        }

        // Atlases ping-pong: blend reads last frame's atlas and writes the other one, which lighting reads this frame.
        frame.ddgiReadIndex = m_ddgiHistoryIndex;
        frame.ddgiWriteIndex = 1 - m_ddgiHistoryIndex;
        m_ddgiHistoryIndex = frame.ddgiWriteIndex;
        // New atlases (first use, probe grid resized) or a requested reset: the blend ignores the previous atlas.
        frame.ddgiFirstFrame = m_isFirstFrame || irradianceHistory.WasReset || distanceHistory.WasReset || offsetsReset ||
                               graph.WasHistoryReset(DDGIHistoryKey);
    }

    void Renderer::addDDGIPasses()
    {
        if (!m_frame.ddgiAdded)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        const uint32_t totalDDGIProbes = m_frame.totalDDGIProbes;
        const uint32_t probeRows = (totalDDGIProbes + DDGIProbesPerRow - 1) / DDGIProbesPerRow;
        const nri::Extent2D radianceExtent = { m_ddgiRaysPerProbe, totalDDGIProbes };
        const uint32_t readIndex = m_frame.ddgiReadIndex;
        const uint32_t writeIndex = m_frame.ddgiWriteIndex;

        m_renderGraph.PushGroup("DDGI");

        // 1. Trace radiance rays: width = rays per probe, height = probe count.
        m_renderGraph.AddPass("DDGI Radiance", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                // Multi-bounce: hits sample last frame's atlases (this frame's are written by the blends below).
                ReadDDGI(builder, resources, readIndex);
                // The probes trace from where last frame's relocation put them.
                if (resources.DDGIProbeOffsets[readIndex].IsValid())
                    builder.Read(resources.DDGIProbeOffsets[readIndex]);
                builder.ColorTarget(resources.DDGIRayData, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 1000.0f });
                builder.SetRenderArea(radianceExtent);
            },
            [this, res = &resources, totalDDGIProbes, readIndex](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();

                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_ddgiRadiancePipeline);
                cmd.setCullMode(NRI_CULL_MODE_NONE);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G |
                                         nri::ColorComponent::B | nri::ColorComponent::A);

                shaderio::PushConstantDDGIRadiance radPush{};
                radPush.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                radPush.raysPerProbe = m_ddgiRaysPerProbe;
                radPush.probeCountTotal = totalDDGIProbes;
                radPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                radPush.probeOffsetsTextureIndex = res->DDGIProbeOffsets[readIndex].IsValid() && !m_frame.ddgiFirstFrame
                                                       ? context.Slot(res->DDGIProbeOffsets[readIndex]) : 0xFFFFFFFF;
                radPush.prevIrradianceTextureIndex = m_frame.ddgiFirstFrame ? 0xFFFFFFFF : context.Slot(res->DDGIIrradiance[readIndex]);
                radPush.prevDistanceTextureIndex = m_frame.ddgiFirstFrame ? 0xFFFFFFFF : context.Slot(res->DDGIDistance[readIndex]);
                cmd.pushData(&radPush, sizeof(shaderio::PushConstantDDGIRadiance));

                nri::drawMeshTasks(cmd, 1, 1, 1);
            });

        // 1b. Relocate the probes from what their rays saw (DDGIRelocation.slang).
        if (resources.DDGIProbeOffsets[writeIndex].IsValid())
        {
            const nri::Extent2D offsetsExtent = { DDGIProbesPerRow, probeRows };
            m_renderGraph.AddPass("DDGI Relocation", RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.DDGIRayData);
                    builder.Read(resources.DDGIProbeOffsets[readIndex]);
                    builder.ColorTarget(resources.DDGIProbeOffsets[writeIndex], NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f });
                    builder.SetRenderArea(offsetsExtent);
                },
                [this, res = &resources, totalDDGIProbes, readIndex](RGPassContext& context)
                {
                    nri::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_ddgiRelocationPipeline);
                    cmd.setCullMode(NRI_CULL_MODE_NONE);
                    cmd.setDepthTestEnable(false);
                    cmd.setDepthWriteEnable(false);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, nri::ColorComponent::All);

                    shaderio::PushConstantDDGIBlend push{};
                    push.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                    push.rayDataTextureIndex = context.Slot(res->DDGIRayData);
                    push.prevAtlasTextureIndex = context.Slot(res->DDGIProbeOffsets[readIndex]);
                    push.probesPerRow = DDGIProbesPerRow;
                    push.raysPerProbe = m_ddgiRaysPerProbe;
                    push.probeCountTotal = totalDDGIProbes;
                    push.firstFrame = m_frame.ddgiFirstFrame ? 1 : 0;
                    push.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                    cmd.pushData(&push, sizeof(push));
                    nri::drawMeshTasks(cmd, 1, 1, 1);
                });
        }

        // 2. Blend irradiance atlas: one workgroup per probe (DDGIBlendIrradiance.slang).
        // The blends on the compute queue (async compute): compute dispatches over the rays, until the screen probes / lighting read them.
        m_renderGraph.AddPass("DDGI Blend Irradiance", RGPassFlags::AsyncCompute,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.DDGIRayData);
                builder.Read(resources.DDGIIrradiance[readIndex]);
                builder.Write(resources.DDGIIrradiance[writeIndex], RGTextureAccess::StorageWrite);
            },
            [this, res = &resources, totalDDGIProbes, readIndex, writeIndex](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_ddgiBlendIrradiancePipeline);

                shaderio::PushConstantDDGIBlend blendIrrPush{};
                blendIrrPush.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                blendIrrPush.rayDataTextureIndex = context.Slot(res->DDGIRayData);
                blendIrrPush.prevAtlasTextureIndex = context.Slot(res->DDGIIrradiance[readIndex]);
                blendIrrPush.probesPerRow = DDGIProbesPerRow;
                blendIrrPush.raysPerProbe = m_ddgiRaysPerProbe;
                blendIrrPush.probeCountTotal = totalDDGIProbes;
                blendIrrPush.hysteresis = m_ddgiHysteresis;
                blendIrrPush.firstFrame = m_frame.ddgiFirstFrame ? 1 : 0;
                blendIrrPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                blendIrrPush.outputStorageIndex = context.StorageSlot(res->DDGIIrradiance[writeIndex]);
                cmd.pushData(&blendIrrPush, sizeof(shaderio::PushConstantDDGIBlend));
                cmd.dispatch(totalDDGIProbes, 1, 1);
            });

        // 3. Blend distance atlas: one workgroup per probe (DDGIBlendDistance.slang).
        m_renderGraph.AddPass("DDGI Blend Distance", RGPassFlags::AsyncCompute,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.DDGIRayData);
                builder.Read(resources.DDGIDistance[readIndex]);
                builder.Write(resources.DDGIDistance[writeIndex], RGTextureAccess::StorageWrite);
            },
            [this, res = &resources, totalDDGIProbes, readIndex, writeIndex](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_ddgiBlendDistancePipeline);

                shaderio::PushConstantDDGIBlend blendDistPush{};
                blendDistPush.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                blendDistPush.rayDataTextureIndex = context.Slot(res->DDGIRayData);
                blendDistPush.prevAtlasTextureIndex = context.Slot(res->DDGIDistance[readIndex]);
                blendDistPush.probesPerRow = DDGIProbesPerRow;
                blendDistPush.raysPerProbe = m_ddgiRaysPerProbe;
                blendDistPush.probeCountTotal = totalDDGIProbes;
                blendDistPush.hysteresis = m_ddgiHysteresis;
                blendDistPush.firstFrame = m_frame.ddgiFirstFrame ? 1 : 0;
                blendDistPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                blendDistPush.outputStorageIndex = context.StorageSlot(res->DDGIDistance[writeIndex]);
                cmd.pushData(&blendDistPush, sizeof(shaderio::PushConstantDDGIBlend));
                cmd.dispatch(totalDDGIProbes, 1, 1);
            });

        m_renderGraph.PopGroup();
    }

    void Renderer::addScreenProbeGIPasses()
    {
        // Diffuse GI mode 3 on the hybrid path: the probes trace the scene TLAS (DDGI, when it runs, is their world radiance cache).
        FrameGraphState& frame = m_frame;
        const bool run = !frame.runPathTracer && m_rayTracingEnabled && m_diffuseGIMode == 3 && m_hasTLASBuild && m_sceneTLAS &&
                         uniformData.tlasDeviceAddress != 0 && uniformData.sceneRayTracingInstancesReference != 0 &&
                         m_screenProbePlacePipeline && m_screenProbeTracePipeline && m_screenProbeIrradiancePipeline &&
                         m_screenProbeFilterPipeline && m_screenProbeIntegratePipeline;
        if (!run)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const uint32_t tile = std::clamp(m_screenProbeTileSize, 4u, 64u);
        const nri::Extent2D probes = { (frame.renderExtent.width + tile - 1) / tile, (frame.renderExtent.height + tile - 1) / tile };
        const nri::Extent2D rayExtent = { probes.width * 8, probes.height * 8 }; // ScreenProbeRayRes, ScreenProbeIrradianceRes

        RGTextureDesc probeDesc;
        probeDesc.Size = RGSize::Absolute;
        probeDesc.Width = probes.width;
        probeDesc.Height = probes.height;
        probeDesc.Format = NRI_FORMAT_R32G32B32A32_SFLOAT;
        probeDesc.Usage = NRI_TEXTURE_USAGE_STORAGE; // (the compute passes write every target as a storage image)
        resources.ScreenProbePosition = m_renderGraph.CreateTexture("Screen Probe Positions", probeDesc);
        probeDesc.Format = NRI_FORMAT_R16G16B16A16_SFLOAT;
        resources.ScreenProbeNormal = m_renderGraph.CreateTexture("Screen Probe Normals", probeDesc);
        RGTextureDesc atlasDesc = probeDesc;
        atlasDesc.Width = rayExtent.width;
        atlasDesc.Height = rayExtent.height;
        resources.ScreenProbeRays = m_renderGraph.CreateTexture("Screen Probe Rays", atlasDesc);
        RGTextureDesc emissiveDesc = atlasDesc;
        emissiveDesc.Format = NRI_FORMAT_R32G32B32A32_SFLOAT; // rgb + a packed direction (ScreenProbeCommon.slang)
        const RGTexture emissiveSamples = m_renderGraph.CreateTexture("Screen Probe Emissive Samples", emissiveDesc);
        resources.ScreenProbeIrradiance[0] = m_renderGraph.CreateTexture("Screen Probe Irradiance", atlasDesc);
        resources.ScreenProbeIrradiance[1] = m_renderGraph.CreateTexture("Screen Probe Irradiance (Filtered)", atlasDesc);

        // The per-pixel result ping-pongs: this frame blends last frame's into the other one, which the lighting reads.
        RGTextureDesc resultDesc;
        resultDesc.Format = NRI_FORMAT_R16G16B16A16_SFLOAT;
        resultDesc.Usage = NRI_TEXTURE_USAGE_STORAGE;
        const RGTextureHistory history = m_renderGraph.GetHistoryTexture(ScreenProbeHistoryKey, resultDesc, 2);
        resources.ScreenProbeGI[0] = history.Textures[0];
        resources.ScreenProbeGI[1] = history.Textures[1];
        RGTextureDesc distanceDesc;
        distanceDesc.Format = NRI_FORMAT_R32_SFLOAT;
        distanceDesc.Usage = NRI_TEXTURE_USAGE_STORAGE;
        resources.ScreenProbeGIDistance = m_renderGraph.CreateTexture("Screen Probe GI Distance", distanceDesc);
        const uint32_t readIndex = m_screenProbeHistoryIndex;
        const uint32_t writeIndex = 1 - readIndex;
        m_screenProbeHistoryIndex = writeIndex;
        // Last frame's result is only this view's history if it was rendered last frame (not after a pause of the mode, a resize
        // or a reset); the previous depth / normal snapshot (addPreviousFrameCopyPass) is then last frame's too.
        const bool historyValid = !m_isFirstFrame && !history.WasReset && m_screenProbeLastFrame + 1 == m_sceneFrameCounter;
        m_screenProbeLastFrame = m_sceneFrameCounter;
        frame.screenProbeGIAdded = true;
        frame.screenProbeGIWriteIndex = writeIndex;

        shaderio::PushConstantScreenProbe common{};
        common.invViewProj = uniformData.invViewProj;
        // Last frame's pixels were drawn with last frame's jittered projection: the history's depth unprojects with that one.
        if (m_screenProbePrevMatricesValid)
            common.prevInvViewProj = glm::mat4(glm::inverse(glm::dmat4(m_screenProbePrevProj) * glm::dmat4(m_screenProbePrevView)));
        else
            common.prevInvViewProj = glm::inverse(uniformData.prevProj * uniformData.prevView);
        common.currJitterUv = jitterScreenOffsetUv();
        common.prevJitterUv = m_screenProbePrevMatricesValid ? m_screenProbePrevJitterUv : common.currJitterUv;
        m_screenProbePrevProj = uniformData.proj;
        m_screenProbePrevView = uniformData.view;
        m_screenProbePrevJitterUv = common.currJitterUv;
        m_screenProbePrevMatricesValid = true;
        common.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
        common.inputTextureIndex = 0xFFFFFFFF;
        common.historyTextureIndex = 0xFFFFFFFF;
        common.materialTextureIndex = 0xFFFFFFFF;
        common.prevDepthTextureIndex = 0xFFFFFFFF;
        common.prevNormalTextureIndex = 0xFFFFFFFF;
        common.velocityTextureIndex = 0xFFFFFFFF;
        common.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
        common.viewportSize = glm::vec2(static_cast<float>(frame.renderExtent.width), static_cast<float>(frame.renderExtent.height));
        common.probeCountX = probes.width;
        common.probeCountY = probes.height;
        common.tileSize = tile;
        common.historyValid = historyValid ? 1u : 0u;
        common.maxHistory = std::clamp(m_screenProbeMaxHistory, 1u, 256u);
        common.emissiveTextureIndex = 0xFFFFFFFF;
        // The exposure the post process applies (last frame's adapted luminance, as SSGI and the path tracer's firefly filter read it):
        // the trace holds each emissive light sample to ScreenProbeEmissiveMaxMiddleGreys x middle grey at it.
        const RGTexture exposureTexture = m_autoExposure && resources.AutoExposureAdapted.IsValid() && !resources.AutoExposureAdaptedReset
                                              ? resources.AutoExposureAdapted : RGTexture{};
        common.autoExposureTextureIndex = 0xFFFFFFFF;
        common.exposureCompensation = m_exposureCompensation;
        common.autoExposureMinEV = m_autoExposureMinEV;
        common.autoExposureMaxEV = m_autoExposureMaxEV;

        // Every pass is a compute dispatch over its target, 8 x 8 texels a group (ScreenProbeCommon.slang), writing every texel.
        auto dispatch = [](nri::CommandBuffer& cmd, nri::Pipeline& pipeline, nri::Extent2D extent, const shaderio::PushConstantScreenProbe& push)
        {
            cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            cmd.pushData(&push, sizeof(shaderio::PushConstantScreenProbe));
            cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        };
        // Async compute (RenderGraph::SetAsyncCompute): the GI's passes may run on the compute queue, beside the graphics passes that
        // do not touch what they read and write (RT shadows and reflections, the ReSTIR passes, ...).
        const RGPassFlags passFlags = RGPassFlags::AsyncCompute;

        const bool readDDGI = frame.ddgiAdded;
        const uint32_t ddgiIndex = frame.ddgiWriteIndex;
        m_renderGraph.PushGroup("Screen Probe GI");

        // 1. Where the probes stand.
        m_renderGraph.AddPass("Screen Probe Place", passFlags,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Write(resources.ScreenProbePosition, RGTextureAccess::StorageWrite);
                builder.Write(resources.ScreenProbeNormal, RGTextureAccess::StorageWrite);
            },
            [this, res = &resources, common, dispatch, probes](RGPassContext& context)
            {
                shaderio::PushConstantScreenProbe push = common;
                push.depthTextureIndex = context.Slot(res->Depth);
                push.normalTextureIndex = context.Slot(res->GBufferNormal);
                push.outputStorageIndex0 = context.StorageSlot(res->ScreenProbePosition);
                push.outputStorageIndex1 = context.StorageSlot(res->ScreenProbeNormal);
                dispatch(context.Cmd(), m_screenProbePlacePipeline, probes, push);
            });

        // 2. Their rays.
        m_renderGraph.AddPass("Screen Probe Trace", passFlags,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.ScreenProbePosition);
                builder.Read(resources.ScreenProbeNormal);
                if (resources.TLAS.IsValid())
                    builder.Read(resources.TLAS, RGBufferAccess::AccelerationStructureRead);
                ReadGpuScene(builder, resources);
                // The hits' bounce light: this frame's DDGI atlases (through the uniforms).
                if (readDDGI)
                {
                    ReadDDGI(builder, resources, ddgiIndex);
                }
                if (exposureTexture.IsValid())
                    builder.Read(exposureTexture);
                builder.Write(resources.ScreenProbeRays, RGTextureAccess::StorageWrite);
                builder.Write(emissiveSamples, RGTextureAccess::StorageWrite);
            },
            [this, res = &resources, common, dispatch, rayExtent, emissiveSamples, exposureTexture](RGPassContext& context)
            {
                shaderio::PushConstantScreenProbe push = common;
                push.probePositionTextureIndex = context.Slot(res->ScreenProbePosition);
                push.probeNormalTextureIndex = context.Slot(res->ScreenProbeNormal);
                push.outputStorageIndex0 = context.StorageSlot(res->ScreenProbeRays);
                push.outputStorageIndex1 = context.StorageSlot(emissiveSamples);
                push.autoExposureTextureIndex = exposureTexture.IsValid() ? context.Slot(exposureTexture) : 0xFFFFFFFF;
                dispatch(context.Cmd(), m_screenProbeTracePipeline, rayExtent, push);
            });

        // 3. The rays convolved into each probe's irradiance.
        m_renderGraph.AddPass("Screen Probe Irradiance", passFlags,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.ScreenProbeRays);
                builder.Read(emissiveSamples);
                builder.Read(resources.ScreenProbePosition);
                builder.Read(resources.ScreenProbeNormal);
                builder.Write(resources.ScreenProbeIrradiance[0], RGTextureAccess::StorageWrite);
            },
            [this, res = &resources, common, dispatch, rayExtent, emissiveSamples](RGPassContext& context)
            {
                shaderio::PushConstantScreenProbe push = common;
                push.probePositionTextureIndex = context.Slot(res->ScreenProbePosition);
                push.probeNormalTextureIndex = context.Slot(res->ScreenProbeNormal);
                push.inputTextureIndex = context.Slot(res->ScreenProbeRays);
                push.emissiveTextureIndex = context.Slot(emissiveSamples);
                push.outputStorageIndex0 = context.StorageSlot(res->ScreenProbeIrradiance[0]);
                dispatch(context.Cmd(), m_screenProbeIrradiancePipeline, rayExtent, push);
            });

        // 4. Spatial filter between neighbouring probes, ping-pong.
        static constexpr const char* FilterPassNames[] = { "Screen Probe Filter 1", "Screen Probe Filter 2", "Screen Probe Filter 3" };
        const uint32_t filterPasses = std::min(m_screenProbeFilterPasses, 3u);
        uint32_t current = 0;
        for (uint32_t filterPass = 0; filterPass < filterPasses; ++filterPass)
        {
            const uint32_t source = current;
            const uint32_t target = 1 - current;
            m_renderGraph.AddPass(FilterPassNames[filterPass], passFlags,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.ScreenProbeIrradiance[source]);
                    builder.Read(resources.ScreenProbePosition);
                    builder.Read(resources.ScreenProbeNormal);
                    builder.Write(resources.ScreenProbeIrradiance[target], RGTextureAccess::StorageWrite);
                },
                [this, res = &resources, common, dispatch, rayExtent, source, target](RGPassContext& context)
                {
                    shaderio::PushConstantScreenProbe push = common;
                    push.probePositionTextureIndex = context.Slot(res->ScreenProbePosition);
                    push.probeNormalTextureIndex = context.Slot(res->ScreenProbeNormal);
                    push.inputTextureIndex = context.Slot(res->ScreenProbeIrradiance[source]);
                    push.outputStorageIndex0 = context.StorageSlot(res->ScreenProbeIrradiance[target]);
                    dispatch(context.Cmd(), m_screenProbeFilterPipeline, rayExtent, push);
                });
            current = target;
        }

        // 5. Per pixel: the probes around it, accumulated over frames (+ which surface each pixel's result is for).
        const bool readVelocity = frame.gbufferVelocityWritten;
        m_renderGraph.AddPass("Screen Probe Integrate", passFlags,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.ScreenProbePosition);
                builder.Read(resources.ScreenProbeNormal);
                builder.Read(resources.ScreenProbeIrradiance[current]);
                builder.Read(resources.GBufferMaterial);
                if (historyValid)
                {
                    builder.Read(resources.ScreenProbeGI[readIndex]);
                    builder.Read(resources.PrevDepth);
                    builder.Read(resources.PrevNormal);
                }
                if (readVelocity)
                    builder.Read(resources.GBufferVelocity);
                // The fallback where no probe stands on a surface.
                if (readDDGI)
                {
                    ReadDDGI(builder, resources, ddgiIndex);
                }
                builder.Write(resources.ScreenProbeGI[writeIndex], RGTextureAccess::StorageWrite);
                builder.Write(resources.ScreenProbeGIDistance, RGTextureAccess::StorageWrite);
            },
            [this, res = &resources, common, dispatch, current, readIndex, writeIndex, historyValid, readVelocity,
             extent = frame.renderExtent](RGPassContext& context)
            {
                shaderio::PushConstantScreenProbe push = common;
                push.depthTextureIndex = context.Slot(res->Depth);
                push.normalTextureIndex = context.Slot(res->GBufferNormal);
                push.probePositionTextureIndex = context.Slot(res->ScreenProbePosition);
                push.probeNormalTextureIndex = context.Slot(res->ScreenProbeNormal);
                push.inputTextureIndex = context.Slot(res->ScreenProbeIrradiance[current]);
                push.materialTextureIndex = context.Slot(res->GBufferMaterial);
                if (historyValid)
                {
                    push.historyTextureIndex = context.Slot(res->ScreenProbeGI[readIndex]);
                    push.prevDepthTextureIndex = context.Slot(res->PrevDepth);
                    push.prevNormalTextureIndex = context.Slot(res->PrevNormal);
                }
                if (readVelocity)
                    push.velocityTextureIndex = context.Slot(res->GBufferVelocity);
                push.outputStorageIndex0 = context.StorageSlot(res->ScreenProbeGI[writeIndex]);
                push.outputStorageIndex1 = context.StorageSlot(res->ScreenProbeGIDistance);
                dispatch(context.Cmd(), m_screenProbeIntegratePipeline, extent, push);
            });

        m_renderGraph.PopGroup();
    }
}
