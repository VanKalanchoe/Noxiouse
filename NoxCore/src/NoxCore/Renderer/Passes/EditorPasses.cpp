// Final passes: ImGui / present to the swapchain, the entity-picking readback copy and render graph texture inspection.
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"

namespace Nox
{
    void Renderer::addPresentPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        // Editor: ImGui (its viewport window shows the scene image). Runtime: fullscreen present of the scene image.
        m_renderGraph.AddPass("ImGui / Present Pass", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Scene);
                // The image arrives from an Undefined transition (contents discarded) and ImGui / the present triangle cover
                // all of it: clear, never load.
                builder.SwapchainTarget(*m_swapChain, m_frame.imageIndex, NRI::LoadOP::clear, NRI::StoreOP::store);
                // ImGui's Vulkan backend may also submit texture uploads to the queue while rendering: safe from a worker,
                // only the main thread submits and it waits for recording.
                if (m_isEditor)
                    builder.RecordExclusive("ImGui");
                builder.SetRenderArea(m_swapChainExtent, RGViewport::None);
            },
            [this](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                if (m_isEditor)
                {
                    cmd.renderImGui();
                    return;
                }

                // Viewport / scissor (counts and values are both dynamic).
                float w = static_cast<float>(m_swapChainExtent.width);
                float h = static_cast<float>(m_swapChainExtent.height);
                cmd.setViewportWithCount({ 0.0f, 0.0f, w, h }, 0.0f, 1.0f);
                cmd.setScissorWithCount(m_swapChainExtent);

                // Vertex input empty since we use vertex fetch BDA but still needs to be called empty
                cmd.setVertexInput();

                // Input assembly.
                cmd.setPrimitiveTopology(NRI::PrimitiveTopology::TriangleList);
                cmd.setPrimitiveRestartEnable(false);

                // Rasterization (most of these come from VK_EXT_extended_dynamic_state_3).
                cmd.setRasterizerDiscardEnable(false);
                cmd.setPolygonMode(NRI::PolygonMode::Fill);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setFrontFace(NRI::FrontFace::CounterClockWise);
                cmd.setDepthBiasEnable(false);
                cmd.setDepthClampEnable(false);

                // Multisampling.
                uint32_t sampleCount = 1;
                cmd.setRasterizationSamples(sampleCount);
                const uint32_t sampleMask = 0xFFFFFFFF;
                cmd.setSampleMask(sampleCount, sampleMask);
                cmd.setAlphaToCoverageEnable(false);
                // alphaToOne is required by the spec when its device feature is enabled and a
                // shader object is bound, even if we don't actually use it.
                cmd.setAlphaToOneEnableEXT(false);

                // Depth / stencil.
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setDepthCompareOp(NRI::CompareOp::Less);
                cmd.setDepthBoundsTestEnable(false);
                cmd.setStencilTestEnable(false);

                // Color blend (for one color attachment); nothing varies between draws so it is set once.
                const NRI::ColorBlendEquation blendEquation
                {
                    .srcColorBlendFactor = NRI::BlendFactor::SrcAlpha,
                    .dstColorBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
                    .colorBlendOp = NRI::BlendOp::Add,
                    .srcAlphaBlendFactor = NRI::BlendFactor::SrcAlpha,
                    .dstAlphaBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
                    .alphaBlendOp = NRI::BlendOp::Add,
                };
                uint32_t colorWriteMask = NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A;
                cmd.setColorBlendEnable(0, false);
                cmd.setColorBlendEquation(0, blendEquation);
                cmd.setColorWriteMask(0, colorWriteMask);
                cmd.setLogicOpEnable(false);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_presentPipeline);

                PushConstantBlock references{};
                // Pass pointer to the global matrix via a buffer device address
                references.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                references.vertexReference = -1;
                references.instanceReference = -1;
                cmd.pushData(&references, sizeof(PushConstantBlock));

                cmd.draw(3, 1, 0, 0);
            });
    }

    void Renderer::addPickReadbackPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!m_pickRequest.active || m_pickRequest.x < 0 || m_pickRequest.y < 0 || !resources.PickerStaging.IsValid())
            return;

        const uint32_t sampleX = static_cast<uint32_t>(m_pickRequest.x);
        const uint32_t sampleY = static_cast<uint32_t>(m_pickRequest.y);
        if (sampleX >= m_frame.outputExtent.width || sampleY >= m_frame.outputExtent.height)
            return;

        // The request is answered by this slot's submission.
        PickRequest& readback = m_pickerReadbackRequests[frameIndex];
        readback = m_pickRequest;
        readback.width = std::min(m_pickRequest.width, m_frame.outputExtent.width - sampleX);
        readback.height = std::min(m_pickRequest.height, m_frame.outputExtent.height - sampleY);
        readback.frameNumber = m_sceneFrameCounter;
        m_pickRequest.active = false;

        // Copies the requested entity-ID area into this slot's staging buffer; read back once the slot's fence signaled
        // (Renderer::readPickResult).
        m_renderGraph.AddPass("Pick Readback", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.EntityHi, RGTextureAccess::CopySource);
                builder.Write(resources.PickerStaging, RGBufferAccess::CopyDestination);
            },
            [res = &resources, sampleX, sampleY, width = readback.width, height = readback.height](RGPassContext& context)
            {
                context.Texture(res->EntityHi).copyImageToBuffer(context.Cmd(), context.Buffer(res->PickerStaging), sampleX, sampleY, width, height);
            });
    }

    void Renderer::addTextureInspection()
    {
        RenderGraph& graph = m_renderGraph;
        const TextureInspection& settings = m_textureInspection;
        m_inspectionImage = nullptr;
        if (settings.Name.empty() || !m_textureInspectPipeline)
            return;

        const RGTexture source = graph.FindTexture(settings.Name, settings.Occurrence);
        // Cube maps are not 2D textures.
        if (!source.IsValid() || (m_environmentCubemap && graph.GetTexture(source) == m_environmentCubemap.get()))
            return;

        const RGTextureKey& key = graph.GetTextureKey(source);
        const uint32_t mip = std::min(settings.Mip, std::max(key.MipLevels, 1u) - 1);
        const uint32_t width = std::max(key.Width >> mip, 1u);
        const uint32_t height = std::max(key.Height >> mip, 1u);
        if (key.Width == 0 || key.Height == 0)
            return;

        // History textures: the display image outlives the frame (ImGui draws it next frame) and a size change releases
        // the old one only after in-flight frames are done with it.
        RGTextureDesc imageDesc;
        imageDesc.Size = RGSize::Absolute;
        imageDesc.Width = width;
        imageDesc.Height = height;
        imageDesc.Format = NRI::ImageFormat::RGBA8;
        imageDesc.Usage = NRI::TextureUsage::Storage;
        const RGTexture image = graph.GetHistoryTexture("Texture Inspection", imageDesc, 1).Textures[0];

        RGTextureDesc probeDesc = imageDesc;
        probeDesc.Width = 1;
        probeDesc.Height = 1;
        probeDesc.Format = NRI::ImageFormat::R32G32B32A32_SFLOAT;
        const RGTexture probe = graph.GetHistoryTexture("Texture Inspection Probe", probeDesc, 1).Textures[0];

        m_inspectionImage = graph.GetTexture(image);
        m_inspectedTextureKey = key;

        shaderio::PushConstantTextureInspect push{};
        push.mode = shaderio::TEXTURE_INSPECT_FLOAT;
        push.channelCount = 4;
        switch (key.Format)
        {
        case NRI::ImageFormat::R32SINT: push.mode = shaderio::TEXTURE_INSPECT_SIGNED_ID; break;
        case NRI::ImageFormat::R32G32_UINT: push.mode = shaderio::TEXTURE_INSPECT_UNSIGNED_PAIR; break;
        case NRI::ImageFormat::R16_SFLOAT:
        case NRI::ImageFormat::R32_SFLOAT: push.channelCount = 1; break;
        case NRI::ImageFormat::R16G16:
        case NRI::ImageFormat::R16G16_SFLOAT:
        case NRI::ImageFormat::R32G32_SFLOAT: push.channelCount = 2; break;
        default: break;
        }
        if (key.Usage == NRI::TextureUsage::DepthStencilAttachment || settings.DepthCurve)
            push.mode = shaderio::TEXTURE_INSPECT_DEPTH;
        push.mip = mip;
        push.channelMask = settings.ChannelMask;
        push.exposure = settings.Exposure;
        push.width = width;
        push.height = height;

        const bool probeTexel = settings.ProbeX >= 0 && settings.ProbeY >= 0 &&
                                static_cast<uint32_t>(settings.ProbeX) < width && static_cast<uint32_t>(settings.ProbeY) < height;
        push.probeX = probeTexel ? settings.ProbeX : -1;
        push.probeY = probeTexel ? settings.ProbeY : -1;
        m_frame.inspectionProbe = probeTexel;

        graph.InspectTexture(source, [this, source, image, probe, push, probeTexel](RGPassContext& context) mutable
        {
            NRI::CommandBuffer& cmd = context.Cmd();
            push.sourceTextureIndex = context.Slot(source);
            push.outputStorageIndex = context.StorageSlot(image);
            push.probeStorageIndex = context.StorageSlot(probe);

            cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_textureInspectPipeline);
            cmd.pushData(&push, sizeof(shaderio::PushConstantTextureInspect));
            cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);

            if (probeTexel)
            {
                cmd.executionBarrier();
                context.Texture(probe).copyImageToBuffer(cmd, *m_inspectionProbeBuffers[frameIndex], 0, 0, 1, 1);
            }
        });
    }
}
