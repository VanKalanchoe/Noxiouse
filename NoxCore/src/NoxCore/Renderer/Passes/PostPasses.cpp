// Display-resolution passes: tonemapping/post-process, 2D overlays, selection outline.
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"

namespace Nox
{
    void Renderer::addPostProcessPass()
    {
        if (!m_postProcessPipeline || !m_autoExposureBuildPipeline || !m_autoExposureReducePipeline)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const FrameGraphState& frame = m_frame;

        RGTexture hdrSource = resources.HDRScene;
        if (frame.dlssAdded)
            hdrSource = resources.DLSSOutput;
        else if (frame.restirPTActive)
            hdrSource = frame.ptNRDAdded ? resources.PathTracerDenoised : resources.ReSTIRPTOutput;
        else if (frame.pathTracerActive)
            hdrSource = frame.ptNRDAdded ? resources.PathTracerDenoised : resources.PathTracerAccum[frame.pathTracerWriteIndex];

        if (m_autoExposure)
        for (uint32_t mip = 0; mip < 9; ++mip)
        {
            m_renderGraph.AddPass(mip == 0 ? "Auto Exposure" : "Auto Exposure Reduce", RGPassFlags::None,
                [&, mip, hdrSource](RGBuilder& builder)
                {
                    builder.Read(mip == 0 ? hdrSource : resources.AutoExposure);
                    builder.Write(resources.AutoExposure, RGTextureAccess::StorageWrite);
                },
                [this, res = &resources, mip, hdrSource](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute,
                                     mip == 0 ? *m_autoExposureBuildPipeline : *m_autoExposureReducePipeline);

                    shaderio::PushConstantAutoExposure push{};
                    push.sourceIndex = uniformData.imageHeapIndexOffset + context.Slot(mip == 0 ? hdrSource : res->AutoExposure);
                    push.outputIndex = context.StorageSlot(res->AutoExposure, mip);
                    push.sourceMip = mip == 0 ? 0 : mip - 1;
                    push.dimension = 256u >> mip;
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((push.dimension + 7) / 8, (push.dimension + 7) / 8, 1);
                });
        }

        // HDR scene (or DLSS output / path tracer result) -> LDR scene.
        m_renderGraph.AddPass("Post Process", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                if (frame.dlssAdded)
                    builder.Read(resources.DLSSOutput);
                else
                    ReadLitScene(builder, resources, frame.restirPTActive, frame.pathTracerActive, frame.ptNRDAdded, frame.pathTracerWriteIndex);
                if (m_autoExposure)
                    builder.Read(resources.AutoExposure);
                builder.ColorTarget(resources.Scene, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                builder.SetRenderArea(frame.outputExtent);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const FrameGraphState& frame = m_frame;

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_postProcessPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantPostProcess postPush{};
                postPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();

                uint32_t activeHdrSlot = 0;
                if (frame.dlssAdded)
                    activeHdrSlot = context.Slot(res->DLSSOutput);
                else if (frame.restirPTActive)
                    activeHdrSlot = context.Slot(frame.ptNRDAdded ? res->PathTracerDenoised : res->ReSTIRPTOutput);
                else if (frame.pathTracerActive)
                    activeHdrSlot = context.Slot(frame.ptNRDAdded ? res->PathTracerDenoised : res->PathTracerAccum[frame.pathTracerWriteIndex]);
                else
                    activeHdrSlot = context.Slot(res->HDRScene);

                postPush.hdrTextureIndex = activeHdrSlot;
                postPush.autoExposureTextureIndex = m_autoExposure ? context.Slot(res->AutoExposure) : 0xFFFFFFFF;
                postPush.debugMode = m_debugMode;
                postPush.tonemapMode = m_tonemapMode;
                postPush.autoExposureEnabled = m_autoExposure ? 1u : 0u;
                postPush.exposureCompensation = m_exposureCompensation;
                postPush.autoExposureMinEV = m_autoExposureMinEV;
                postPush.autoExposureMaxEV = m_autoExposureMaxEV;
                cmd.pushData(&postPush, sizeof(shaderio::PushConstantPostProcess));

                cmd.drawMeshTasks(1, 1, 1);
            });
    }

    void Renderer::addOverlay2DPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        // Quads, circles, text, gizmos on the LDR scene, depth-tested against the display-resolution depth.
        m_renderGraph.AddPass("2D Overlay", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.Scene, NRI::LoadOP::load, NRI::StoreOP::store);
                builder.ColorTarget(resources.EntityHi, NRI::LoadOP::load, NRI::StoreOP::store);
                builder.DepthTarget(resources.DepthHi, NRI::LoadOP::load, NRI::StoreOP::store);
                builder.SetRenderArea(m_frame.outputExtent);
            },
            [this](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                const NRI::ColorBlendEquation blendEquation{
                    .srcColorBlendFactor = NRI::BlendFactor::SrcAlpha,
                    .dstColorBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
                    .colorBlendOp = NRI::BlendOp::Add,
                    .srcAlphaBlendFactor = NRI::BlendFactor::Zero,
                    .dstAlphaBlendFactor = NRI::BlendFactor::One,
                    .alphaBlendOp = NRI::BlendOp::Add,
                };

                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(true);
                cmd.setDepthWriteEnable(false);
                cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);

                cmd.setColorBlendEnable(0, true);
                cmd.setColorBlendEquation(0, blendEquation);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                cmd.setColorBlendEnable(1, false);
                cmd.setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                m_renderer2D->Flush(cmd, *m_uniformBuffers[frameIndex], frameIndex);
            });
    }

    void Renderer::addOutlinePass()
    {
        if (m_SelectedEntityIDs.empty() || !m_outlinePipeline)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        // Selection outline from the display-resolution entity IDs.
        m_renderGraph.AddPass("Outline", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.EntityHi);
                builder.ColorTarget(resources.Scene, NRI::LoadOP::load, NRI::StoreOP::store);
                builder.SetRenderArea(m_frame.outputExtent);
            },
            [this](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                // Rasterization
                cmd.setRasterizerDiscardEnable(false);
                cmd.setPolygonMode(NRI::PolygonMode::Fill);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setFrontFace(NRI::FrontFace::CounterClockWise);
                cmd.setDepthBiasEnable(false);
                cmd.setDepthClampEnable(false);

                // Fullscreen post-process: one sample.
                cmd.setRasterizationSamples(1);
                cmd.setSampleMask(1, 0xFFFFFFFF);
                cmd.setAlphaToCoverageEnable(false);
                cmd.setAlphaToOneEnableEXT(false);

                // No depth test/write: the outline comes entirely from the entity IDs, so reverse-Z is irrelevant here.
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setDepthBoundsTestEnable(false);
                cmd.setStencilTestEnable(false);

                // Alpha blending for the orange outline.
                const NRI::ColorBlendEquation blendEquation
                {
                    .srcColorBlendFactor = NRI::BlendFactor::SrcAlpha,
                    .dstColorBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
                    .colorBlendOp = NRI::BlendOp::Add,

                    .srcAlphaBlendFactor = NRI::BlendFactor::SrcAlpha,
                    .dstAlphaBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
                    .alphaBlendOp = NRI::BlendOp::Add,
                };

                const uint32_t colorWriteMask =
                    NRI::ColorComponent::R |
                    NRI::ColorComponent::G |
                    NRI::ColorComponent::B |
                    NRI::ColorComponent::A;

                cmd.setColorBlendEnable(0, true);
                cmd.setColorBlendEquation(0, blendEquation);
                cmd.setColorWriteMask(0, colorWriteMask);
                cmd.setLogicOpEnable(false);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_outlinePipeline);

                shaderio::PushConstantOutline references{};
                references.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                references.selectedEntityIDsReference = m_selectedEntityIDBuffers[frameIndex]->getDeviceAddress();
                references.selectedEntityCount = static_cast<uint32_t>(m_SelectedEntityIDs.size());
                cmd.pushData(&references, sizeof(shaderio::PushConstantOutline));

                cmd.drawMeshTasks(1, 1, 1);
            });
    }
}
