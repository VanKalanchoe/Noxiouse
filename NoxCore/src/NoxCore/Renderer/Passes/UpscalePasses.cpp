// Upscaling passes: DLSS / DLAA / Ray Reconstruction and the render-to-display resolution entity/depth blit.
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"

namespace Nox
{
    void Renderer::addDLSSPass()
    {
        if (!m_dlssEnabled || m_dlssMode == NRI::UpscaleMode::Off || !m_device->isDLSSSupported())
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        FrameGraphState& frame = m_frame;
        frame.dlssAdded = true;
        // NGX keeps the history: resets (resize, mode change) arrive through the render graph's history key.
        frame.resetDLSS = m_isFirstFrame || m_renderGraph.WasHistoryReset(DLSSHistoryKey);

        // Ray Reconstruction divides the image by the albedo it was shaded with (diffuse and pre-integrated specular).
        // The path tracer writes these from its primary hit; raster/deferred paths derive them from the G-buffer.
        const bool rayReconstruction = m_dlssRayReconstructionEnabled && m_device->isDLSSRayReconstructionSupported() && m_rrGuidesPipeline;
        // The plain path tracer writes guides from its actual primary surface and the first specular/transmission segment.
        // Raster/deferred rendering still prepares guides from the G-buffer here.
        const bool pathTracerGuides = rayReconstruction && frame.pathTracerActive && !frame.restirPTActive;
        // RTXPT's DLSS-RR inputs: specular motion vectors, never the specular hit distance (its sample turns that off, "it's buggy").
        // The path tracer writes its own; the hybrid's come from the RT reflections' hit distance (RRGuides.slang).
        const bool reflectionDistance = rayReconstruction && frame.rtReflectionsAdded && !frame.restirPTActive && !frame.pathTracerActive;
        if (rayReconstruction && !pathTracerGuides)
        {
            m_renderGraph.AddPass("RR Guides", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.Depth);
                    builder.Read(resources.GBufferAlbedo);
                    builder.Read(resources.GBufferNormal);
                    builder.Read(resources.GBufferMaterial);
                    builder.Write(resources.RRDiffuseAlbedo, RGTextureAccess::StorageWrite);
                    builder.Write(resources.RRSpecularAlbedo, RGTextureAccess::StorageWrite);
                    builder.Read(resources.GBufferVelocity);
                    builder.Write(resources.RRSpecularMotionVectors, RGTextureAccess::StorageWrite);
                    if (reflectionDistance)
                        builder.Read(resources.RawReflection);
                },
                [this, res = &resources, reflectionDistance](RGPassContext& context)
                {
                    shaderio::PushConstantRRGuides push{};
                    push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                    push.depthTextureIndex = context.Slot(res->Depth);
                    push.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                    push.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                    push.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                    push.diffuseAlbedoStorageIndex = context.StorageSlot(res->RRDiffuseAlbedo);
                    push.specularAlbedoStorageIndex = context.StorageSlot(res->RRSpecularAlbedo);
                    push.rawReflectionIndex = reflectionDistance ? context.Slot(res->RawReflection) : 0xFFFFFFFF;
                    push.velocityTextureIndex = context.Slot(res->GBufferVelocity);
                    push.specularMotionStorageIndex = context.StorageSlot(res->RRSpecularMotionVectors);
                    push.width = m_frame.renderExtent.width;
                    push.height = m_frame.renderExtent.height;

                    NRI::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_rrGuidesPipeline);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                });
        }

        m_renderGraph.AddPass("DLSS", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                ReadLitScene(builder, resources, frame.restirPTActive, frame.pathTracerActive, frame.ptNRDAdded, frame.pathTracerWriteIndex);
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferVelocity);
                if (rayReconstruction)
                {
                    builder.Read(resources.RRDiffuseAlbedo);
                    builder.Read(resources.RRSpecularAlbedo);
                    builder.Read(resources.RRSpecularMotionVectors);
                    if (pathTracerGuides)
                    {
                        builder.Read(resources.RRNormalRoughness);
                        builder.Read(resources.RRPathDepth);
                        builder.Read(resources.RRPathMotionVectors);
                    }
                    else
                    {
                        builder.Read(resources.GBufferNormal);
                        builder.Read(resources.GBufferMaterial);
                    }
                }
                if (rayReconstruction && frame.rrTransparencyLayer)
                {
                    builder.Read(resources.RRTransparencyLayer);
                    builder.Read(resources.RRTransparencyOpacity);
                }
                builder.Write(resources.DLSSOutput);
                builder.RecordExclusive("Streamline");
            },
            [this, res = &resources, rayReconstruction, pathTracerGuides](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const FrameGraphState& frame = m_frame;

                // Path tracing feeds its noisy 1-SPP result - unless NRD denoises it this frame (mutually exclusive with
                // DLSS-RR, so DLSS acts as pure upscaling), then the denoised, decoded linear result.
                RGTexture input = res->HDRScene;
                if (frame.restirPTActive)
                    input = frame.ptNRDAdded ? res->PathTracerDenoised : res->ReSTIRPTOutput;
                else if (frame.pathTracerActive)
                    input = frame.ptNRDAdded ? res->PathTracerDenoised : res->PathTracerAccum[frame.pathTracerWriteIndex];
                NRI::Texture2D& inputColor = context.Texture(input);
                NRI::Texture2D& outputColor = context.Texture(res->DLSSOutput);

                NRI::DLSSParams dlssParams{};
                dlssParams.inputColor = &inputColor;
                dlssParams.outputColor = &outputColor;
                dlssParams.depth = &context.Texture(pathTracerGuides ? res->RRPathDepth : res->Depth);
                dlssParams.motionVectors = &context.Texture(pathTracerGuides ? res->RRPathMotionVectors : res->GBufferVelocity);
                if (rayReconstruction)
                {
                    dlssParams.albedo = &context.Texture(res->RRDiffuseAlbedo);
                    dlssParams.specularAlbedo = &context.Texture(res->RRSpecularAlbedo);
                    dlssParams.specularMotionVectors = &context.Texture(res->RRSpecularMotionVectors);
                    if (frame.rrTransparencyLayer)
                    {
                        dlssParams.transparencyLayer = &context.Texture(res->RRTransparencyLayer);
                        dlssParams.transparencyLayerOpacity = &context.Texture(res->RRTransparencyOpacity);
                    }
                    if (pathTracerGuides)
                    {
                        dlssParams.normal = &context.Texture(res->RRNormalRoughness);
                        dlssParams.normalRoughnessPacked = true;
                    }
                    else
                    {
                        dlssParams.normal = &context.Texture(res->GBufferNormal);
                        dlssParams.roughness = &context.Texture(res->GBufferMaterial);
                    }
                }
                dlssParams.rayReconstruction = rayReconstruction;
                dlssParams.commandBuffer = &cmd;

                dlssParams.nonJitteredProj = uniformData.nonJitteredProj;
                dlssParams.view = uniformData.view;
                dlssParams.prevNonJitteredProj = uniformData.prevProj;
                dlssParams.prevView = uniformData.prevView;

                dlssParams.jitterOffset = m_currentJitter;
                dlssParams.cameraPos = m_cameraPosition;
                dlssParams.cameraUp = m_cameraUp;
                dlssParams.cameraRight = m_cameraRight;
                dlssParams.cameraFwd = m_cameraForward;
                dlssParams.cameraNear = m_cameraNear;
                dlssParams.cameraFovRad = glm::radians(m_cameraFOV);
                dlssParams.mode = m_dlssMode;
                dlssParams.reset = frame.resetDLSS;

                // Post Process always reads the DLSS output; when Streamline fails (logged), it holds the unscaled input
                // instead of depending on a result another recording thread would have to publish.
                if (!m_device->evaluateDLSS(dlssParams))
                    inputColor.blitTo(cmd, outputColor);
                // Streamline / NGX bound descriptor sets of their own (classic descriptors), which unbinds the engine's heaps for the
                // passes recorded after this one in the same command buffer (Stable Entity/Depth, post, overlays).
                cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
            });
    }

    void Renderer::addTAAPass()
    {
        FrameGraphState& frame = m_frame;
        const bool run = isTAAActive() && !frame.dlssAdded && !frame.restirPTActive && !frame.pathTracerActive &&
                         frame.renderExtent.width == frame.outputExtent.width && frame.renderExtent.height == frame.outputExtent.height &&
                         frame.renderExtent.width > 0 && frame.renderExtent.height > 0;
        if (!run)
        {
            m_taaRanLastFrame = false;
            return;
        }

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        RGTextureDesc desc;
        desc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        desc.Usage = NRI::TextureUsage::Storage;
        const RGTextureHistory history = m_renderGraph.GetHistoryTexture("TAA History", desc, 2);
        resources.TAAHistory[0] = history.Textures[0];
        resources.TAAHistory[1] = history.Textures[1];
        const uint32_t readIndex = m_taaHistoryIndex;
        const uint32_t writeIndex = 1 - readIndex;
        m_taaHistoryIndex = writeIndex;
        const bool reset = m_isFirstFrame || history.WasReset || !m_taaRanLastFrame || m_renderGraph.WasHistoryReset(DLSSHistoryKey);
        m_taaRanLastFrame = true;
        frame.taaAdded = true;
        frame.taaWriteIndex = writeIndex;

        m_renderGraph.AddPass("TAA", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.HDRScene);
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferVelocity);
                builder.Read(resources.TAAHistory[readIndex]);
                builder.Write(resources.TAAHistory[writeIndex], RGTextureAccess::StorageWrite);
            },
            [this, res = &resources, readIndex, writeIndex, reset](RGPassContext& context)
            {
                shaderio::PushConstantTAA push{};
                push.colorIndex = uniformData.imageHeapIndexOffset + context.Slot(res->HDRScene);
                push.depthIndex = uniformData.imageHeapIndexOffset + context.Slot(res->Depth);
                push.velocityIndex = uniformData.imageHeapIndexOffset + context.Slot(res->GBufferVelocity);
                push.historyIndex = uniformData.imageHeapIndexOffset + context.Slot(res->TAAHistory[readIndex]);
                push.outputIndex = context.StorageSlot(res->TAAHistory[writeIndex]);
                push.width = m_frame.renderExtent.width;
                push.height = m_frame.renderExtent.height;
                push.reset = reset ? 1u : 0u;
                push.blend = 0.1f;

                NRI::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_taaPipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
            });
    }

    void Renderer::addEntityDepthBlitPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        // Editor with a jittered camera: drawn again without the jitter instead, so what reads them holds still.
        const bool stableWanted = m_isEditor && m_stableEditorDepth && m_currentJitter != glm::vec2(0.0f);
        if (stableWanted && m_stableEntityDepthPipeline && resources.CameraDraws.Commands.IsValid())
        {
            addStableEntityDepthPass();
            return;
        }

        // Render-resolution entity IDs/depth -> display resolution. The 2D overlay depth-tests world-space 2D content
        // against the 3D scene, and picking/outline need entity IDs at full display resolution; a nearest blit avoids
        // re-rendering the scene. Everything downstream reads the display copies, so this also runs at 1:1.
        m_renderGraph.AddPass("Entity/Depth Blit", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Entity, RGTextureAccess::CopySource);
                builder.Read(resources.Depth, RGTextureAccess::CopySource);
                builder.Write(resources.EntityHi, RGTextureAccess::CopyDestination);
                builder.Write(resources.DepthHi, RGTextureAccess::CopyDestination);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                // DLSS / NGX may have bound its own descriptors.
                cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
                context.Texture(res->Entity).blitTo(cmd, context.Texture(res->EntityHi));
                context.Texture(res->Depth).blitTo(cmd, context.Texture(res->DepthHi));
            });
    }

    void Renderer::addStableEntityDepthPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        // The camera's visible instances at display resolution with the non-jittered projection (EditorStableDepth.slang): what the
        // blit would give without the jitter. Depth-writing surfaces first, then transparent ones (entity IDs over them, depth tested,
        // like the forward pass writes them).
        m_renderGraph.AddPass("Stable Entity/Depth", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.EntityHi, NRI::LoadOP::clear, NRI::StoreOP::store, { -1.0f, 0.0f, 0.0f, 0.0f }); // R32SINT
                builder.DepthTarget(resources.DepthHi, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0 });                    // reverse Z
                builder.SetRenderArea(m_frame.outputExtent);
                ReadGpuScene(builder, resources);
                ReadIfValid(builder, resources.SkinnedVertices);
                ReadViewDraws(builder, resources.CameraDraws);
                ReadViewDraws(builder, resources.CameraDraws, true);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                // Right after DLSS (as the blit it replaces did): NGX may have bound its own descriptors.
                cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
                cmd.setDepthTestEnable(true);
                cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::All);

                NRI::Pipeline& pipeline = *m_stableEntityDepthPipeline;
                struct BucketDraw
                {
                    RenderBucket Bucket;
                    NRI::CullMode Cull;
                };
                constexpr BucketDraw solid[] = {
                    { RenderBucket::Opaque, NRI::CullMode::Back }, { RenderBucket::OpaqueDoubleSided, NRI::CullMode::None },
                    { RenderBucket::Mask, NRI::CullMode::Back }, { RenderBucket::MaskDoubleSided, NRI::CullMode::None },
                    { RenderBucket::Unlit, NRI::CullMode::Back }, { RenderBucket::UnlitDoubleSided, NRI::CullMode::None } };
                constexpr BucketDraw transparent[] = {
                    { RenderBucket::Transparent, NRI::CullMode::Back }, { RenderBucket::TransparentDoubleSided, NRI::CullMode::None },
                    { RenderBucket::TransparentUnlit, NRI::CullMode::Back }, { RenderBucket::TransparentUnlitDoubleSided, NRI::CullMode::None } };

                NRI::Pipeline* bound = nullptr;
                auto drawBuckets = [&](std::span<const BucketDraw> buckets, bool depthWrite)
                {
                    // Both culling phases: the instances found visible at once, then the ones phase 2 found behind the old depth.
                    for (int late = 0; late < 2; ++late)
                    {
                        MeshletDrawCursor cursor = beginMeshletDraws(context, res->CameraDraws, late == 1);
                        cursor.taskFlags &= ~shaderio::MESHLET_COUNT_TRIANGLES; // the stats count the camera's passes
                        cursor.boundPipeline = bound;
                        for (const BucketDraw& draw : buckets)
                            drawMeshletBucket(cmd, cursor, draw.Bucket, pipeline, draw.Cull, depthWrite, false);
                        bound = cursor.boundPipeline;
                    }
                };
                drawBuckets(solid, true);
                drawBuckets(transparent, false);
            });
    }
}
