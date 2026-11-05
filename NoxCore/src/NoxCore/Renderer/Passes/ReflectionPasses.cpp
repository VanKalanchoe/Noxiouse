// Screen-space reflections (RS4, docs/Backlog_2026.md "Raster versions of the RT features"; Settings > Screen-Space Reflections): the
// raster version of the RT reflections, measured against them. UE5's / Godot's SSR: the mirror ray marched through this frame's depth
// buffer (SSR.slang), the radiance at the hit from last frame's lit scene -- kept here as a history with a blur mip chain
// (ColorPyramid.slang), written after the forward passes for the next frame -- at the mip the roughness' cone covers. The deferred
// lighting blends the environment in where the trace found nothing. The RT reflections take precedence when they run.
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"

namespace Nox
{
    void Renderer::createSSRPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = { NRI::ImageFormat::R16G16B16A16_SFLOAT };
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/SSR.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = "assets/shaders/SSR.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain", .sourcePath = "assets/shaders/SSR.slang" });
        m_ssrPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::addSSRPass()
    {
        const bool pathTracing = m_pathTracingEnabled || m_debugMode == 18 || m_debugMode == 19;
        if (!m_ssrEnabled || pathTracing || !m_ssrPipeline || !m_colorPyramidCopyPipeline || !m_colorPyramidDownsamplePipeline)
        {
            m_ssrHistoryValid = false;
            return;
        }

        // The lit scene history (two slots: last frame's read here, this frame's written by addSSRHistoryPass), full mip chain.
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        RGTextureDesc historyDesc;
        historyDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        historyDesc.Usage = NRI::TextureUsage::Storage;
        historyDesc.MipLevels = 1;
        for (uint32_t size = std::max(m_frame.renderExtent.width, m_frame.renderExtent.height); size > 1; size >>= 1)
            ++historyDesc.MipLevels;
        const RGTextureHistory history = m_renderGraph.GetHistoryTexture("SSR Scene Color", historyDesc, 2);
        resources.SSRHistory[0] = history.Textures[0];
        resources.SSRHistory[1] = history.Textures[1];
        m_frame.ssrHistoryMips = historyDesc.MipLevels;
        if (history.WasReset)
            m_ssrHistoryValid = false;

        // Nothing to reflect yet (the first frame, a new size), or the RT reflections run: only the history is written this frame.
        if (!m_ssrHistoryValid || m_frame.rtReflectionsAdded)
            return;

        RGTextureDesc ssrDesc;
        ssrDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        resources.SSR = m_renderGraph.CreateTexture("SSR", ssrDesc);
        m_frame.ssrAdded = true;
        const RGTexture historyRead = resources.SSRHistory[m_ssrHistoryIndex];
        m_renderGraph.AddPass("SSR", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferMaterial);
                builder.Read(historyRead);
                builder.ColorTarget(resources.SSR, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, historyRead, mips = historyDesc.MipLevels](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_ssrPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::All);

                shaderio::PushConstantSSR push{};
                push.invViewProj = uniformData.invViewProj;
                push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                push.depthTextureIndex = context.Slot(res->Depth);
                push.normalTextureIndex = context.Slot(res->GBufferNormal);
                push.materialTextureIndex = context.Slot(res->GBufferMaterial);
                push.historyTextureIndex = context.Slot(historyRead);
                push.historyMipCount = mips;
                push.maxSteps = m_ssrMaxSteps;
                push.viewportSize = glm::vec2(static_cast<float>(m_frame.renderExtent.width), static_cast<float>(m_frame.renderExtent.height));
                push.maxDistance = WorldUnits::FromMeters(m_ssrMaxDistanceMeters);
                push.thickness = WorldUnits::FromMeters(m_ssrThicknessMeters);
                push.maxRoughness = m_ssrMaxRoughness;
                push.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                cmd.pushData(&push, sizeof(push));
                cmd.drawMeshTasks(1, 1, 1);
            });
    }

    void Renderer::addSSRHistoryPass()
    {
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.SSRHistory[0].IsValid() || m_frame.restirPTActive || m_frame.pathTracerActive || !resources.HDRScene.IsValid())
            return;

        // This frame's lit scene (opaque, sky, transparent, particles) and its 2x2-average mips into the other slot, for next frame.
        const uint32_t writeIndex = 1 - m_ssrHistoryIndex;
        const RGTexture target = resources.SSRHistory[writeIndex];
        const RGTexture source = resources.HDRScene;
        const NRI::Extent2D extent = m_frame.renderExtent;
        for (uint32_t mip = 0; mip < m_frame.ssrHistoryMips; ++mip)
        {
            m_renderGraph.AddPass(mip == 0 ? "SSR History" : "SSR History Mip", RGPassFlags::NeverCull,
                [&, mip](RGBuilder& builder)
                {
                    builder.Read(mip == 0 ? source : target);
                    builder.Write(target, RGTextureAccess::StorageWrite);
                },
                [this, mip, source, target, extent](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, mip == 0 ? *m_colorPyramidCopyPipeline : *m_colorPyramidDownsamplePipeline);
                    shaderio::PushConstantColorPyramid push{};
                    push.sourceIndex = uniformData.imageHeapIndexOffset + context.Slot(mip == 0 ? source : target);
                    push.outputIndex = context.StorageSlot(target, mip);
                    push.sourceMip = mip == 0 ? 0 : mip - 1;
                    push.width = std::max(extent.width >> mip, 1u);
                    push.height = std::max(extent.height >> mip, 1u);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                });
        }
        m_ssrHistoryIndex = writeIndex;
        m_ssrHistoryValid = true;
    }
}
