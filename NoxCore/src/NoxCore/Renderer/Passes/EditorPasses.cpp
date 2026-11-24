// Final passes: NoxUI / present to the swapchain, the entity-picking readback copy and render graph texture inspection.
#include "NoxCore/Renderer/Renderer.h"

#include <array>
#include <NoxUI/Backends/noxui_impl_noxrhi.h>

#include "FrameGraphResources.h"

namespace Nox
{
    void Renderer::addPresentPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        // The editor's extra viewports show their own images (addExtraViewPasses draws them; importing again returns the same handle).
        std::vector<RGTexture> extraViewImages;
        for (uint32_t index = 0; index < MaxExtraViews; ++index)
        {
            if (m_extraViews[index].Shown)
                extraViewImages.push_back(m_renderGraph.ImportTexture("Extra View Image", m_extraViews[index].Shown->GetHandle()));
        }
        // Asset thumbnails taken from a view this frame: read once more here, after that nothing writes them.
        for (const Ref<Texture2D>& image : m_takenExtraViewImages)
            extraViewImages.push_back(m_renderGraph.ImportTexture("Asset Thumbnail", image->GetHandle()));

        const bool drawNoxUI = m_noxUIInitialized && !m_uiDrawList.IsEmpty();
        NoxUI_NoxRHIDrawData noxUIDrawData{};
        if (drawNoxUI)
            noxUIDrawData = NoxUI_ImplNoxRHI_PrepareDrawData(m_uiDrawList, frameIndex, 0);

        if (drawNoxUI)
        {
            m_renderGraph.AddPass("NoxUI Upload", RGPassFlags::NeverCull,
                [](RGBuilder& builder) { builder.RecordExclusive("NoxUI"); },
                [noxUIDrawData](RGPassContext& context)
                {
                    NoxUI_ImplNoxRHI_CopyData(context.Cmd(), noxUIDrawData);
                });
        }

        // Editor: NoxUI (its viewport window shows the scene image). Runtime: fullscreen present of the scene image.
        m_renderGraph.AddPass("Present Pass", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                ReadIfValid(builder, resources.Scene);
                for (const RGTexture image : extraViewImages)
                    ReadIfValid(builder, image);
                // The image arrives from an Undefined transition (contents discarded) and NoxUI / the present triangle cover
                // all of it: clear, never load.
                builder.SwapchainTarget(m_swapChain, m_frame.imageIndex, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE);
                if (drawNoxUI)
                    builder.RecordExclusive("NoxUI");
                builder.SetRenderArea(m_swapChainExtent, RGViewport::None);
            },
            [this, hasScene = resources.Scene.IsValid(), drawNoxUI, noxUIDrawData](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();

                // Viewport / scissor (counts and values are both dynamic).
                float w = static_cast<float>(m_swapChainExtent.width);
                float h = static_cast<float>(m_swapChainExtent.height);
                cmd.setViewportWithCount({ 0.0f, 0.0f, w, h }, 0.0f, 1.0f);
                cmd.setScissorWithCount(m_swapChainExtent);

                // Vertex input empty since we use vertex fetch BDA but still needs to be called empty
                cmd.setVertexInput();

                // Input assembly.
                cmd.setPrimitiveTopology(NRI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
                cmd.setPrimitiveRestartEnable(false);

                // Rasterization (most of these come from VK_EXT_extended_dynamic_state_3).
                cmd.setRasterizerDiscardEnable(false);
                cmd.setPolygonMode(NRI_POLYGON_MODE_FILL);
                cmd.setCullMode(NRI_CULL_MODE_NONE);
                cmd.setFrontFace(NRI_FRONT_FACE_COUNTER_CLOCKWISE);
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
                cmd.setAlphaToOneEnable(false);

                // Depth / stencil.
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setDepthCompareOp(NRI_COMPARE_LESS);
                cmd.setDepthBoundsTestEnable(false);
                cmd.setStencilTestEnable(false);

                // Color blend (for one color attachment); nothing varies between draws so it is set once.
                const nri::ColorBlendEquation blendEquation
                {
                    .srcColorBlendFactor = NRI_BLEND_FACTOR_SRC_ALPHA,
                    .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                    .colorBlendOp = NRI_BLEND_OP_ADD,
                    .srcAlphaBlendFactor = NRI_BLEND_FACTOR_SRC_ALPHA,
                    .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                    .alphaBlendOp = NRI_BLEND_OP_ADD,
                };
                uint32_t colorWriteMask = nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A;
                cmd.setColorBlendEnable(0, false);
                cmd.setColorBlendEquation(0, blendEquation);
                cmd.setColorWriteMask(0, colorWriteMask);
                cmd.setLogicOpEnable(false);

                if (!m_isEditor && hasScene && m_presentPipeline)
                {
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_presentPipeline);

                    PushConstantBlock references{};
                    // Pass pointer to the global matrix via a cross-API buffer reference
                    references.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                    references.vertexReference = -1;
                    references.instanceReference = -1;
                    cmd.pushData(&references, sizeof(PushConstantBlock));

                    cmd.draw(3, 1, 0, 0);

    
    
    
    
    
    

    
                }

                if (m_isEditor && m_sceneResource && m_sceneResource->GetDescriptorIndexSlot() == ~0u)
                {
                    m_resourceHeap.registerTexture(*m_sceneResource);
                    uniformData.imageHeapIndexOffset = m_resourceHeap.getImageHeapIndexOffset();
                }

                if (drawNoxUI)
                    NoxUI_ImplNoxRHI_RenderDrawData(cmd, noxUIDrawData);









            });
    }

    void Renderer::addTextureReadbackPasses()
    {
        // ReadbackTexture: each waiting texture copied into its staging buffer (read in readTextureReadbacks once this frame is done).
        static constexpr std::array<const char*, 4> kNames = { "Texture Readback 1", "Texture Readback 2", "Texture Readback 3", "Texture Readback 4" };
        size_t recorded = 0;
        for (TextureReadback& readback : m_textureReadbacks)
        {
            if (readback.FrameSlot != UINT32_MAX)
                continue;
            if (recorded == kNames.size())
                break;
            const RGTexture texture = m_renderGraph.ImportTexture(kNames[recorded], readback.Texture->GetHandle());
            NriBuffer staging = readback.Staging.handle();
            m_renderGraph.AddPass(kNames[recorded], RGPassFlags::NeverCull,
                [&](RGBuilder& builder)
                {
                    builder.Read(texture, RGTextureAccess::CopySource);
                },
                [texture, staging, width = readback.Width, height = readback.Height](RGPassContext& context)
                {
                    nriTextureCopyToBuffer(context.Texture(texture), context.Cmd().handle(), staging, 0, 0, width, height);
                });
            readback.FrameSlot = frameIndex;
            ++recorded;
        }
    }

    void Renderer::addEditorRaycastPass()
    {
        if (m_raycastQueue.empty() || frameIndex >= m_raycastBuffers.size() || !m_raycastsInFlight[frameIndex].empty())
            return;
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(m_raycastQueue.size(), shaderio::EDITOR_RAYCAST_MAX));
        std::vector<shaderio::EditorRay> rays(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            rays[index] = m_raycastQueue[index].Ray;
            rays[index].origin = ToRenderSpace(m_raycastQueue[index].Origin); // (large world coordinates: the origin of this frame)
            m_raycastsInFlight[frameIndex].push_back(std::move(m_raycastQueue[index].Callback));
        }
        m_raycastQueue.erase(m_raycastQueue.begin(), m_raycastQueue.begin() + count);
        const bool traced = m_editorRaycastPipeline && resources.TLAS.IsValid();
        if (!traced)
        {
            std::vector<RaycastCallback> callbacks = std::move(m_raycastsInFlight[frameIndex]);
            m_raycastsInFlight[frameIndex].clear();
            for (const RaycastCallback& cb : callbacks)
            {
                if (cb)
                    cb(false, -1.0f);
            }
            return;
        }
        const uint64_t rayBytes = sizeof(shaderio::EditorRay) * count;
        memcpy(m_raycastUploadBuffers[frameIndex].map(0, rayBytes), rays.data(), rayBytes);
        m_raycastUploadBuffers[frameIndex].unmap();

        m_renderGraph.AddPass("Editor Raycasts", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.TLAS, RGBufferAccess::AccelerationStructureRead);
                ReadGpuScene(builder, resources);
            },
            [this, count, rayBytes](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                nri::Buffer& uploadBuf = m_raycastUploadBuffers[frameIndex];
                nri::Buffer& gpuBuf = m_raycastGpuBuffers[frameIndex];
                nri::Buffer& readbackBuf = m_raycastBuffers[frameIndex];

                const nri::BufferCopyRegion region{ .srcOffset = 0, .dstOffset = 0, .size = rayBytes };
                cmd.copyBuffer(uploadBuf, gpuBuf, region);
                const nri::BufferBarrierDesc toCompute{
                    gpuBuf.handle(),
                    { nri::AccessBits::TransferWrite, nri::StageBits::Transfer },
                    { nri::AccessBits::ShaderRead | nri::AccessBits::ShaderWrite, nri::StageBits::Compute }
                };
                cmd.resourceBarriers({}, std::span<const nri::BufferBarrierDesc>(&toCompute, 1));

                shaderio::PushConstantEditorRaycast push{};
                push.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                push.raysReference = bufferWriteRef(gpuBuf);
                push.rayCount = count;
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_editorRaycastPipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch((count + 63) / 64, 1, 1);

                const nri::BufferBarrierDesc toReadback{
                    gpuBuf.handle(),
                    { nri::AccessBits::ShaderWrite, nri::StageBits::Compute },
                    { nri::AccessBits::TransferRead, nri::StageBits::Transfer }
                };
                cmd.resourceBarriers({}, std::span<const nri::BufferBarrierDesc>(&toReadback, 1));
                cmd.copyBuffer(gpuBuf, readbackBuf, region);
            });
    }

    void Renderer::addPickReadbackPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!m_pickRequest.active || m_pickRequest.x < 0 || m_pickRequest.y < 0 || !resources.PickerStaging.IsValid() || !resources.PickerDepthStaging.IsValid())
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
        // The depth block around the pixel, up to 9 x 9 (the measure tool snaps to a drawn pixel close to the cursor).
        constexpr int32_t kDepthRadius = 4;
        const int32_t patchX0 = std::max(0, static_cast<int32_t>(sampleX) - kDepthRadius);
        const int32_t patchY0 = std::max(0, static_cast<int32_t>(sampleY) - kDepthRadius);
        const int32_t patchX1 = std::min(static_cast<int32_t>(m_frame.outputExtent.width) - 1, static_cast<int32_t>(sampleX) + kDepthRadius);
        const int32_t patchY1 = std::min(static_cast<int32_t>(m_frame.outputExtent.height) - 1, static_cast<int32_t>(sampleY) + kDepthRadius);
        readback.patchX = patchX0;
        readback.patchY = patchY0;
        readback.patchWidth = static_cast<uint32_t>(patchX1 - patchX0 + 1);
        readback.patchHeight = static_cast<uint32_t>(patchY1 - patchY0 + 1);
        m_pickRequest.active = false;

        // Copies the requested entity-ID area into this slot's staging buffer; read back once the slot's fence signaled
        // (Renderer::readPickResult).
        m_renderGraph.AddPass("Pick Readback", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.EntityHi, RGTextureAccess::CopySource);
                builder.Read(resources.DepthHi, RGTextureAccess::CopySource);
                builder.Write(resources.PickerStaging, RGBufferAccess::CopyDestination);
                builder.Write(resources.PickerDepthStaging, RGBufferAccess::CopyDestination);
            },
            [res = &resources, sampleX, sampleY, width = readback.width, height = readback.height, patchX0, patchY0,
             patchWidth = readback.patchWidth, patchHeight = readback.patchHeight](RGPassContext& context)
            {
                nriTextureCopyToBuffer(context.Texture(res->EntityHi), context.Cmd().handle(), context.Buffer(res->PickerStaging), sampleX, sampleY, width, height);
                // The depths around the same first pixel (the measure tool turns one into a world point).
                nriTextureCopyToBuffer(context.Texture(res->DepthHi), context.Cmd().handle(), context.Buffer(res->PickerDepthStaging),
                                       static_cast<uint32_t>(patchX0), static_cast<uint32_t>(patchY0), patchWidth, patchHeight);
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
        if (!source.IsValid() || (m_environmentCubemap && graph.GetTexture(source) == m_environmentCubemap->GetHandle()))
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
        imageDesc.Format = NRI_FORMAT_RGBA8;
        imageDesc.Usage = NRI_TEXTURE_USAGE_STORAGE;
        const RGTexture image = graph.GetHistoryTexture("Texture Inspection", imageDesc, 1).Textures[0];

        RGTextureDesc probeDesc = imageDesc;
        probeDesc.Width = 1;
        probeDesc.Height = 1;
        probeDesc.Format = NRI_FORMAT_R32G32B32A32_SFLOAT;
        const RGTexture probe = graph.GetHistoryTexture("Texture Inspection Probe", probeDesc, 1).Textures[0];

        m_inspectionImage = graph.GetTexture(image);
        m_inspectedTextureKey = key;

        shaderio::PushConstantTextureInspect push{};
        push.mode = shaderio::TEXTURE_INSPECT_FLOAT;
        push.channelCount = 4;
        switch (key.Format)
        {
        case NRI_FORMAT_R32_SINT: push.mode = shaderio::TEXTURE_INSPECT_SIGNED_ID; break;
        case NRI_FORMAT_R32G32_UINT:
        case NRI_FORMAT_R32G32B32A32_UINT: push.mode = shaderio::TEXTURE_INSPECT_UNSIGNED_PAIR; break;
        case NRI_FORMAT_R8_UNORM:
        case NRI_FORMAT_R16_SFLOAT:
        case NRI_FORMAT_R32_SFLOAT: push.channelCount = 1; break;
        case NRI_FORMAT_R16G16:
        case NRI_FORMAT_R16G16_SFLOAT:
        case NRI_FORMAT_R32G32_SFLOAT: push.channelCount = 2; break;
        default: break;
        }
        if (key.Usage == NRI_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT || settings.DepthCurve)
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
            nri::CommandBuffer& cmd = context.Cmd();
            push.sourceTextureIndex = context.Slot(source);
            push.outputStorageIndex = context.StorageSlot(image);
            push.probeStorageIndex = context.StorageSlot(probe);

            cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_textureInspectPipeline);
            cmd.pushData(&push, sizeof(shaderio::PushConstantTextureInspect));
            cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);

            if (probeTexel)
            {
                cmd.executionBarrier();
                nriTextureCopyToBuffer(context.Texture(probe), cmd.handle(), m_inspectionProbeBuffers[frameIndex].handle(), 0, 0, 1, 1);
            }
        });
    }
}
