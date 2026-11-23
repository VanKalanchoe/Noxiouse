// Upscaling passes: DLSS / DLAA / Ray Reconstruction and the render-to-display resolution entity/depth blit.
#include "NoxCore/Renderer/Renderer.h"

#include <cmath>

#include "FrameGraphResources.h"
#include "NoxCore/Core/Log.h"

namespace Nox
{
    void Renderer::addDLSSPass()
    {
        if (!m_dlssEnabled || m_dlssMode == NRI_UPSCALER_MODE_OFF || !isDLSSSupported() || !m_dlssSR)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        FrameGraphState& frame = m_frame;
        frame.dlssAdded = true;
        // NGX keeps the history: resets (resize, mode change) arrive through the render graph's history key.
        frame.resetDLSS = m_isFirstFrame || m_renderGraph.WasHistoryReset(DLSSHistoryKey);

        // Ray Reconstruction divides the image by the albedo it was shaded with (diffuse and pre-integrated specular).
        // The path tracer writes these from its primary hit; raster/deferred paths derive them from the G-buffer.
        const bool rayReconstruction = m_dlssRayReconstructionEnabled && isDLSSRayReconstructionSupported() && m_dlssRR && m_rrGuidesPipeline;
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
                    builder.Read(resources.GBufferLobes);    // the clear coat's share of the specular albedo
                    builder.Read(resources.GBufferSpecular);
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
                    push.matrixReference = m_uniformBuffers[frameIndex].getDeviceAddress();
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

                    nri::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_rrGuidesPipeline);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                });
        }

        // NGX inverts world-to-screen (projection x view) and the previous frame's; a degenerate camera only shows as an NGX warning
        // every frame ("WorldToScreenMatrix not invertible"). Which input is bad, logged once each time it starts.
        {
            auto invertible = [](const glm::mat4& matrix)
            {
                const float determinant = glm::determinant(matrix);
                return std::isfinite(determinant) && std::abs(determinant) > 1e-30f;
            };
            const char* bad = !invertible(uniformData.nonJitteredProj * uniformData.view) ? "projection x view"
                            : !invertible(uniformData.prevProj * uniformData.prevView)    ? "previous projection x view"
                            : glm::length(m_cameraForward) < 1e-6f || glm::length(m_cameraUp) < 1e-6f || glm::length(m_cameraRight) < 1e-6f
                                ? "camera basis"
                                : nullptr;
            if (bad && !m_dlssCameraWarned)
            {
                NOX_CORE_WARN("DLSS: the camera's {} is not invertible (det view {:g}, det projection {:g}, previous {:g} / {:g}, render {}x{}, "
                              "near {:g}, fov {:g})", bad, glm::determinant(uniformData.view), glm::determinant(uniformData.nonJitteredProj),
                              glm::determinant(uniformData.prevView), glm::determinant(uniformData.prevProj), m_renderSize.width,
                              m_renderSize.height, m_cameraNear, m_cameraFOV);
            }
            m_dlssCameraWarned = bad != nullptr;
        }

        m_renderGraph.AddPass("DLSS", RGPassFlags::OwnCommandBuffer,
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
                if (rayReconstruction && frame.rrColorBeforeTransparency)
                    builder.Read(resources.RRColorBeforeTransparency);
                builder.Write(resources.DLSSOutput);
                builder.RecordExclusive("Streamline");
            },
            [this, res = &resources, rayReconstruction, pathTracerGuides](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                const FrameGraphState& frame = m_frame;

                // Path tracing feeds its noisy 1-SPP result - unless NRD denoises it this frame (mutually exclusive with
                // DLSS-RR, so DLSS acts as pure upscaling), then the denoised, decoded linear result.
                RGTexture input = res->HDRScene;
                if (frame.restirPTActive)
                    input = frame.ptNRDAdded ? res->PathTracerDenoised : res->ReSTIRPTOutput;
                else if (frame.pathTracerActive)
                    input = frame.ptNRDAdded ? res->PathTracerDenoised : res->PathTracerAccum[frame.pathTracerWriteIndex];
                const NriTexture inputColor = context.Texture(input);
                const NriTexture outputColor = context.Texture(res->DLSSOutput);

                nri::UpscalerEvaluateDesc dlssParams{};
                dlssParams.inputColor = inputColor;
                dlssParams.outputColor = outputColor;
                dlssParams.depth = context.Texture(pathTracerGuides ? res->RRPathDepth : res->Depth);
                dlssParams.motionVectors = context.Texture(pathTracerGuides ? res->RRPathMotionVectors : res->GBufferVelocity);
                if (rayReconstruction)
                {
                    dlssParams.albedo = context.Texture(res->RRDiffuseAlbedo);
                    dlssParams.specularAlbedo = context.Texture(res->RRSpecularAlbedo);
                    dlssParams.specularMotionVectors = context.Texture(res->RRSpecularMotionVectors);
                    if (frame.rrColorBeforeTransparency)
                        dlssParams.colorBeforeTransparency = context.Texture(res->RRColorBeforeTransparency);
                    if (pathTracerGuides)
                    {
                        dlssParams.normal = context.Texture(res->RRNormalRoughness);
                        dlssParams.normalRoughnessPacked = true;
                    }
                    else
                    {
                        dlssParams.normal = context.Texture(res->GBufferNormal);
                        dlssParams.roughness = context.Texture(res->GBufferMaterial);
                    }
                }

                std::memcpy(dlssParams.nonJitteredProj, glm::value_ptr(uniformData.nonJitteredProj), sizeof(float) * 16);
                std::memcpy(dlssParams.view, glm::value_ptr(uniformData.view), sizeof(float) * 16);
                std::memcpy(dlssParams.prevNonJitteredProj, glm::value_ptr(uniformData.prevProj), sizeof(float) * 16);
                std::memcpy(dlssParams.prevView, glm::value_ptr(uniformData.prevView), sizeof(float) * 16);

                dlssParams.jitterOffset[0] = m_currentJitter.x;
                dlssParams.jitterOffset[1] = m_currentJitter.y;
                dlssParams.cameraPos[0] = m_cameraPosition.x; dlssParams.cameraPos[1] = m_cameraPosition.y; dlssParams.cameraPos[2] = m_cameraPosition.z;
                dlssParams.cameraUp[0] = m_cameraUp.x; dlssParams.cameraUp[1] = m_cameraUp.y; dlssParams.cameraUp[2] = m_cameraUp.z;
                dlssParams.cameraRight[0] = m_cameraRight.x; dlssParams.cameraRight[1] = m_cameraRight.y; dlssParams.cameraRight[2] = m_cameraRight.z;
                dlssParams.cameraFwd[0] = m_cameraForward.x; dlssParams.cameraFwd[1] = m_cameraForward.y; dlssParams.cameraFwd[2] = m_cameraForward.z;
                dlssParams.cameraNear = m_cameraNear;
                dlssParams.cameraFovRad = glm::radians(m_cameraFOV);
                dlssParams.mode = m_dlssMode;
                dlssParams.reset = frame.resetDLSS;

                const nri::Upscaler& upscaler = rayReconstruction ? m_dlssRR : m_dlssSR;
                if (!upscaler || !upscaler.evaluate(cmd, dlssParams))
                    nriTextureBlitTo(inputColor, cmd.handle(), outputColor);
                cmd.bindDescriptorHeaps(&m_resourceHeap, &m_samplerHeap);
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
        desc.Format = NRI_FORMAT_R16G16B16A16_SFLOAT;
        desc.Usage = NRI_TEXTURE_USAGE_STORAGE;
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

                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_taaPipeline);
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
                nri::CommandBuffer& cmd = context.Cmd();
                // DLSS / NGX may have bound its own descriptors.
                cmd.bindDescriptorHeaps(&m_resourceHeap, &m_samplerHeap);
                nriTextureBlitTo(context.Texture(res->Entity), cmd.handle(), context.Texture(res->EntityHi));
                nriTextureBlitTo(context.Texture(res->Depth), cmd.handle(), context.Texture(res->DepthHi));
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
                builder.ColorTarget(resources.EntityHi, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { -1.0f, 0.0f, 0.0f, 0.0f }); // R32SINT
                builder.DepthTarget(resources.DepthHi, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0 });                    // reverse Z
                builder.SetRenderArea(m_frame.outputExtent);
                ReadGpuScene(builder, resources);
                ReadIfValid(builder, resources.SkinnedVertices);
                ReadViewDraws(builder, resources.CameraDraws);
                ReadViewDraws(builder, resources.CameraDraws, true);
            },
            [this, res = &resources](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                // Right after DLSS (as the blit it replaces did): NGX may have bound its own descriptors.
                cmd.bindDescriptorHeaps(&m_resourceHeap, &m_samplerHeap);
                cmd.setDepthTestEnable(true);
                cmd.setDepthCompareOp(NRI_COMPARE_GREATER_OR_EQUAL);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, nri::ColorComponent::All);

                const nri::Pipeline& pipeline = *stableEntityDepthPipeline(); // (with the material graphs' Opacity Mask while a material has a graph)
                struct BucketDraw
                {
                    RenderBucket Bucket;
                    nri::CullMode Cull;
                };
                constexpr BucketDraw solid[] = {
                    { RenderBucket::Opaque, NRI_CULL_MODE_BACK }, { RenderBucket::OpaqueDoubleSided, NRI_CULL_MODE_NONE },
                    { RenderBucket::Mask, NRI_CULL_MODE_BACK }, { RenderBucket::MaskDoubleSided, NRI_CULL_MODE_NONE },
                    { RenderBucket::Unlit, NRI_CULL_MODE_BACK }, { RenderBucket::UnlitDoubleSided, NRI_CULL_MODE_NONE } };
                constexpr BucketDraw transparent[] = {
                    { RenderBucket::Transparent, NRI_CULL_MODE_BACK }, { RenderBucket::TransparentDoubleSided, NRI_CULL_MODE_NONE },
                    { RenderBucket::TransparentUnlit, NRI_CULL_MODE_BACK }, { RenderBucket::TransparentUnlitDoubleSided, NRI_CULL_MODE_NONE } };

                const nri::Pipeline* bound = nullptr;
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
