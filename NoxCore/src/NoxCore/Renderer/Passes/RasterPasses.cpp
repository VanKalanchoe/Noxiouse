// Raster passes: GPU scene update, TLAS build, visibility buffer, G-buffer resolve, forward 3D (unlit, skybox, transparent, DDGI probes).
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"

namespace Nox
{
    void Renderer::addGpuSceneUpdatePass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!m_gpuScene.HasUploads(frameIndex))
            return;

        // This frame's scene changes, scattered from the frame slot's staging buffers into the persistent tables. Never
        // culled: the changes are staged once.
        m_renderGraph.AddPass("GPU Scene Update", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                for (RGBuffer table : { resources.SceneInstances, resources.SceneTransforms, resources.SceneMaterials, resources.SceneMeshes,
                                        resources.SceneRayTracingInstances })
                {
                    if (table.IsValid())
                        builder.Write(table, RGBufferAccess::CopyDestination);
                }
            },
            [this](RGPassContext& context)
            {
                m_gpuScene.RecordUploads(context.Cmd(), frameIndex);
            });
    }

    void Renderer::addTLASBuildPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.TLAS.IsValid() || !m_hasTLASBuild || !m_sceneTLAS || !m_tlasScratchBuffer || !m_tlasNeedBuild)
            return;

        // Consumed here: this frame's command buffers carry the build.
        m_tlasNeedBuild = false;

        m_renderGraph.AddPass("TLAS Build", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Write(resources.TLAS, RGBufferAccess::AccelerationStructureBuild);
            },
            [this](RGPassContext& context)
            {
                BuildSceneAccelerationStructure(context.Cmd());
            });
    }

    void Renderer::addVisibilityPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        m_renderGraph.AddPass("Visibility", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.Visibility, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                // Reverse-Z: clear to 0; stored for every later depth consumer.
                builder.DepthTarget(resources.Depth, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0 });
                builder.SetRenderArea(m_frame.renderExtent);
                ReadGpuScene(builder, resources);
            },
            [this](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                // Opaque depth state on top of the command buffer baseline (Renderer::applyCommandBufferBaseline).
                cmd.setCullMode(NRI::CullMode::Back);
                cmd.setDepthTestEnable(true);
                cmd.setDepthWriteEnable(true);
                cmd.setDepthCompareOp(NRI::CompareOp::Greater);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                if (!m_drawInstances.empty() && m_visibilityPipeline)
                {
                    // All PBR opaque & mask geometry rasterizes to the visibility buffer and depth: the first four buckets.
                    MeshletDrawCursor cursor = beginMeshletDraws();
                    drawMeshletBucket(cmd, cursor, RenderBucket::Opaque, *m_visibilityPipeline, NRI::CullMode::Back, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::OpaqueDoubleSided, *m_visibilityPipeline, NRI::CullMode::None, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::Mask, *m_visibilityPipeline, NRI::CullMode::Back, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::MaskDoubleSided, *m_visibilityPipeline, NRI::CullMode::None, true, false);
                }
            });
    }

    void Renderer::addGBufferPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const bool resolveMaterials = m_gbufferPipeline && !m_drawInstances.empty();

        // Decoupled material resolve, or only the clears when the scene has no meshes (entity IDs must read -1 for
        // picking, and later passes still find defined G-buffer contents).
        m_renderGraph.AddPass(resolveMaterials ? "G-Buffer" : "G-Buffer Clear", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                if (resolveMaterials)
                    builder.Read(resources.Visibility);
                    ReadGpuScene(builder, resources);
                builder.ColorTarget(resources.GBufferAlbedo, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });   // RGBA8
                builder.ColorTarget(resources.GBufferNormal, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });   // RGBA16F world normal
                builder.ColorTarget(resources.GBufferMaterial, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f }); // roughness, metallic, workflow
                builder.ColorTarget(resources.GBufferEmission, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f }); // RGBA16F
                builder.ColorTarget(resources.Entity, NRI::LoadOP::clear, NRI::StoreOP::store, { -1.0f, 0.0f, 0.0f, 0.0f });         // R32SINT
                builder.ColorTarget(resources.GBufferVelocity, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f }); // RG32F
                builder.ColorTarget(resources.GBufferSpecular, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f }); // RGBA8
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, resolveMaterials](RGPassContext& context)
            {
                if (!resolveMaterials)
                    return;

                NRI::CommandBuffer& cmd = context.Cmd();
                constexpr uint32_t attachmentCount = 7;
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_gbufferPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);

                for (uint32_t a = 0; a < attachmentCount; ++a)
                {
                    cmd.setColorBlendEnable(a, false);
                    cmd.setColorWriteMask(a, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
                }

                shaderio::PushConstantVisibilityDebug gbufferPush{};
                gbufferPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();

                bool hasBoneBuffers = frameIndex < m_boneBuffers.size() && m_boneBuffers[frameIndex] != nullptr;
                bool hasBones = !m_boneMatrices.empty();
                gbufferPush.boneMatrixReference = (hasBones && hasBoneBuffers) ? m_boneBuffers[frameIndex]->getDeviceAddress() : 0;

                bool hasPageTables = frameIndex < m_vertexPageTableBuffers.size() && m_vertexPageTableBuffers[frameIndex] != nullptr;
                gbufferPush.vertexPageTableReference = hasPageTables ? m_vertexPageTableBuffers[frameIndex]->getDeviceAddress() : 0;
                gbufferPush.meshletDrawsPageTableReference = (hasPageTables && frameIndex < m_meshletDrawPageTableBuffers.size() && m_meshletDrawPageTableBuffers[frameIndex])
                                                                 ? m_meshletDrawPageTableBuffers[frameIndex]->getDeviceAddress()
                                                                 : 0;
                gbufferPush.meshletVerticesPageTableReference = (hasPageTables && frameIndex < m_meshletVertPageTableBuffers.size() && m_meshletVertPageTableBuffers[frameIndex])
                                                                    ? m_meshletVertPageTableBuffers[frameIndex]->getDeviceAddress()
                                                                    : 0;
                gbufferPush.meshletTrianglesPageTableReference = (hasPageTables && frameIndex < m_meshletTriPageTableBuffers.size() && m_meshletTriPageTableBuffers[frameIndex])
                                                                     ? m_meshletTriPageTableBuffers[frameIndex]->getDeviceAddress()
                                                                     : 0;
                gbufferPush.visibilityTextureIndex = context.Slot(res->Visibility);
                gbufferPush.viewportSize = glm::vec2(rw, rh);
                gbufferPush.debugMode = m_debugMode;
                cmd.pushData(&gbufferPush, sizeof(shaderio::PushConstantVisibilityDebug));

                cmd.drawMeshTasks(1, 1, 1);
            });
    }

    void Renderer::addForward3DPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        // Debug mode 17 draws the DDGI irradiance atlas written this frame on the probe spheres.
        const bool drawProbes = m_ddgiDebugSpheresPipeline && m_debugMode == 17 && m_frame.ddgiAdded;

        // Unlit & skybox & transparent geometry rendered in HDR on top of the lit scene.
        m_renderGraph.AddPass("Forward 3D", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                // Composites over deferred lighting; path-traced frames never lit the HDR scene, so it starts cleared.
                if (m_frame.deferredLightingAdded)
                    builder.ColorTarget(resources.HDRScene, NRI::LoadOP::load, NRI::StoreOP::store);
                else
                    builder.ColorTarget(resources.HDRScene, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                builder.ColorTarget(resources.Entity, NRI::LoadOP::load, NRI::StoreOP::store);
                builder.DepthTarget(resources.Depth, NRI::LoadOP::load, NRI::StoreOP::store);
                // Transparent lit shading samples the DDGI atlases (through the uniforms); the probe spheres show irradiance.
                if (m_frame.ddgiAdded)
                {
                    builder.Read(resources.DDGIIrradiance[m_frame.ddgiWriteIndex]);
                    builder.Read(resources.DDGIDistance[m_frame.ddgiWriteIndex]);
                }
                builder.SetRenderArea(m_frame.renderExtent);
                ReadGpuScene(builder, resources);
            },
            [this, res = &resources, drawProbes](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const uint32_t totalDDGIProbes = m_frame.totalDDGIProbes;
                MeshletDrawCursor cursor = beginMeshletDraws();

                // A. UNLIT MESHES
                if (m_unlitPipeline && (getBucketDrawCount(RenderBucket::Unlit) > 0 || getBucketDrawCount(RenderBucket::UnlitDoubleSided) > 0))
                {
                    cursor.boundPipeline = nullptr;
                    cmd.setDepthTestEnable(true);
                    cmd.setDepthWriteEnable(true);
                    cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
                    cmd.setColorBlendEnable(1, false);
                    cmd.setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                    drawMeshletBucket(cmd, cursor, RenderBucket::Unlit, *m_unlitPipeline, NRI::CullMode::Back, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::UnlitDoubleSided, *m_unlitPipeline, NRI::CullMode::None, true, false);
                }

                // B. SKYBOX (Tested against depth == 0.0)
                if (m_skyboxPipeline && m_environmentCubemap && m_debugMode == 0)
                {
                    cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_skyboxPipeline);
                    cmd.setCullMode(NRI::CullMode::None);
                    cmd.setDepthTestEnable(true);
                    cmd.setDepthWriteEnable(false);
                    cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
                    cmd.setColorBlendEnable(1, false);
                    cmd.setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                    shaderio::PushConstantSkybox skyboxPush{};
                    skyboxPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                    skyboxPush.cubemapIndex = m_environmentCubemap->GetDescriptorIndexSlot();
                    cmd.pushData(&skyboxPush, sizeof(shaderio::PushConstantSkybox));
                    cmd.drawMeshTasks(1, 1, 1);
                }

                // C. TRANSPARENT FORWARD PASS (Back-to-front sorted, Alpha Blending)
                bool hasAnyTransparent = getBucketDrawCount(RenderBucket::Transparent) > 0 || getBucketDrawCount(RenderBucket::TransparentDoubleSided) > 0 ||
                                          getBucketDrawCount(RenderBucket::TransparentUnlit) > 0 || getBucketDrawCount(RenderBucket::TransparentUnlitDoubleSided) > 0;
                if (m_unlitPipeline && m_transparentLitPipeline && hasAnyTransparent)
                {
                    cursor.boundPipeline = nullptr;
                    cmd.setDepthTestEnable(true);
                    cmd.setDepthWriteEnable(false);
                    cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                    cmd.setColorBlendEnable(0, true);
                    cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
                    cmd.setColorBlendEnable(1, false);
                    cmd.setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                    // Non-unlit Blend objects get real shading (same BRDF/IBL as the deferred opaque path, via
                    // PBRLighting.slang) instead of a flat baseColor pass-through, so they don't look self-lit next to
                    // shaded Opaque/Mask geometry. True KHR_materials_unlit objects still use the flat shader.
                    //
                    // CullMode matches each object's own doubleSided flag (mirroring the opaque/mask queues): a closed,
                    // single-sided translucent shape needs its own backface culled, or both hemispheres blend on top of
                    // each other and wash the result out.
                    drawMeshletBucket(cmd, cursor, RenderBucket::Transparent, *m_transparentLitPipeline, NRI::CullMode::Back, false, true);
                    drawMeshletBucket(cmd, cursor, RenderBucket::TransparentDoubleSided, *m_transparentLitPipeline, NRI::CullMode::None, false, true);
                    drawMeshletBucket(cmd, cursor, RenderBucket::TransparentUnlit, *m_unlitPipeline, NRI::CullMode::Back, false, true);
                    drawMeshletBucket(cmd, cursor, RenderBucket::TransparentUnlitDoubleSided, *m_unlitPipeline, NRI::CullMode::None, false, true);

                    cmd.setColorBlendEnable(0, false);
                    cmd.setDepthWriteEnable(true);
                }

                // D. DDGI PROBE SPHERES (Debug Mode 17: Visualizes 3D Probe Grid with Irradiance)
                if (drawProbes)
                {
                    cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_ddgiDebugSpheresPipeline);
                    cmd.setCullMode(NRI::CullMode::None);
                    cmd.setDepthTestEnable(!m_ddgiDebugXRay);
                    cmd.setDepthWriteEnable(false);
                    cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);
                    cmd.setColorBlendEnable(1, false);
                    cmd.setColorWriteMask(1, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                    shaderio::PushConstantDDGIDebug debugPush{};
                    debugPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                    debugPush.probeCountTotal = totalDDGIProbes;
                    debugPush.sphereRadius = m_ddgiDebugSphereRadius;
                    debugPush.irradianceAtlasIndex = context.Slot(res->DDGIIrradiance[m_frame.ddgiWriteIndex]);
                    cmd.pushData(&debugPush, sizeof(shaderio::PushConstantDDGIDebug));

                    cmd.drawMeshTasks(totalDDGIProbes, 1, 1);
                }
            });
    }
}
