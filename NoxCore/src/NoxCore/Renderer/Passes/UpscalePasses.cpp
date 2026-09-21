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
        // Match RTXPT's shipping/default DLSS-RR path: its sample deliberately disables SpecularHitDistance
        // ("it's buggy") and supplies SpecularMotionVectors for path tracing instead. A single stochastic path
        // cannot provide a dense, stable hit-distance field; tagging it makes static glass retain the reflected
        // history until motion happens to invalidate it. Hybrid reflections still have a dense hit-distance buffer.
        const bool hitDistance = rayReconstruction && frame.rtReflectionsAdded &&
            !frame.restirPTActive && !frame.pathTracerActive;
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
                    if (hitDistance)
                    {
                        builder.Read(resources.RawReflection);
                        builder.Write(resources.RRSpecularHitDistance, RGTextureAccess::StorageWrite);
                    }
                },
                [this, res = &resources, hitDistance](RGPassContext& context)
                {
                    shaderio::PushConstantRRGuides push{};
                    push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                    push.depthTextureIndex = context.Slot(res->Depth);
                    push.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                    push.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                    push.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                    push.diffuseAlbedoStorageIndex = context.StorageSlot(res->RRDiffuseAlbedo);
                    push.specularAlbedoStorageIndex = context.StorageSlot(res->RRSpecularAlbedo);
                    push.rawReflectionIndex = hitDistance ? context.Slot(res->RawReflection) : 0xFFFFFFFF;
                    push.hitDistanceStorageIndex = hitDistance ? context.StorageSlot(res->RRSpecularHitDistance) : 0xFFFFFFFF;
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
                    if (pathTracerGuides)
                    {
                        builder.Read(resources.RRNormalRoughness);
                        builder.Read(resources.RRPathDepth);
                        builder.Read(resources.RRPathMotionVectors);
                        builder.Read(resources.RRSpecularMotionVectors);
                    }
                    else
                    {
                        builder.Read(resources.GBufferNormal);
                        builder.Read(resources.GBufferMaterial);
                    }
                }
                if (hitDistance)
                    builder.Read(resources.RRSpecularHitDistance);
                builder.Write(resources.DLSSOutput);
                builder.RecordExclusive("Streamline");
            },
            [this, res = &resources, rayReconstruction, pathTracerGuides, hitDistance](RGPassContext& context)
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
                    if (pathTracerGuides)
                    {
                        dlssParams.normal = &context.Texture(res->RRNormalRoughness);
                        dlssParams.normalRoughnessPacked = true;
                        dlssParams.specularMotionVectors = &context.Texture(res->RRSpecularMotionVectors);
                    }
                    else
                    {
                        dlssParams.normal = &context.Texture(res->GBufferNormal);
                        dlssParams.roughness = &context.Texture(res->GBufferMaterial);
                    }
                }
                if (hitDistance)
                    dlssParams.specularHitDistance = &context.Texture(res->RRSpecularHitDistance);
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
            });
    }

    void Renderer::addEntityDepthBlitPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

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
}
