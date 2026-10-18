// The editor's extra views (docs/Future.md item 5): the viewports besides the active one. Each is a camera of its own drawn
// raster only after the main view -- frustum culling, one forward pass (opaque and masked surfaces lit by
// Material_TransparentLit_Mesh, with its ray-query shadows of the strongest lights, unlit, the sky, transparent surfaces),
// tonemapping into the view's texture, then the 2D overlay's world-space batch with the view's own grid. No G-buffer, RT
// reflections / GI passes, upscaling or history of its own.
#include "NoxCore/Renderer/Renderer.h"

#include <algorithm>
#include <optional>

#include "FrameGraphResources.h"

namespace Nox
{
    namespace
    {
        // Graph names must outlive the graph: a literal per view.
        constexpr const char* kCullNames[Renderer::MaxExtraViews] = { "Viewport 1 Culling", "Viewport 2 Culling", "Viewport 3 Culling", "Viewport 4 Culling", "Thumbnail Culling", "Material Preview Culling", "Animation Preview Culling" };
        constexpr const char* kForwardNames[Renderer::MaxExtraViews] = { "Viewport 1 Forward", "Viewport 2 Forward", "Viewport 3 Forward", "Viewport 4 Forward", "Thumbnail Forward", "Material Preview Forward", "Animation Preview Forward" };
        constexpr const char* kPostNames[Renderer::MaxExtraViews] = { "Viewport 1 Post Process", "Viewport 2 Post Process", "Viewport 3 Post Process", "Viewport 4 Post Process", "Thumbnail Post Process", "Material Preview Post Process", "Animation Preview Post Process" };
        constexpr const char* kOverlayNames[Renderer::MaxExtraViews] = { "Viewport 1 2D Overlay", "Viewport 2 2D Overlay", "Viewport 3 2D Overlay", "Viewport 4 2D Overlay", "Thumbnail 2D Overlay", "Material Preview 2D Overlay", "Animation Preview 2D Overlay" };
        constexpr const char* kOutputNames[Renderer::MaxExtraViews] = { "Viewport 1 Image", "Viewport 2 Image", "Viewport 3 Image", "Viewport 4 Image", "Thumbnail Image", "Material Preview Image", "Animation Preview Image" };
        constexpr const char* kHdrNames[Renderer::MaxExtraViews] = { "Viewport 1 HDR", "Viewport 2 HDR", "Viewport 3 HDR", "Viewport 4 HDR", "Thumbnail HDR", "Material Preview HDR", "Animation Preview HDR" };
        constexpr const char* kEntityNames[Renderer::MaxExtraViews] = { "Viewport 1 Entity IDs", "Viewport 2 Entity IDs", "Viewport 3 Entity IDs", "Viewport 4 Entity IDs", "Thumbnail Entity IDs", "Material Preview Entity IDs", "Animation Preview Entity IDs" };
        constexpr const char* kDepthNames[Renderer::MaxExtraViews] = { "Viewport 1 Depth", "Viewport 2 Depth", "Viewport 3 Depth", "Viewport 4 Depth", "Thumbnail Depth", "Material Preview Depth", "Animation Preview Depth" };
    }

    void Renderer::addExtraViewPasses()
    {
        for (ExtraView& view : m_extraViews)
            view.Recorded = false;
        if (!m_isEditor)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const uint32_t entryCount = static_cast<uint32_t>(m_drawList.size());
        const bool cullingReady = entryCount > 0 &&
            std::all_of(m_instanceCullingPipelines.begin(), m_instanceCullingPipelines.end(), [](const auto& pipeline) { return pipeline != nullptr; });
        const uint32_t blockCount = (entryCount + shaderio::CULL_BLOCK_SIZE - 1) / shaderio::CULL_BLOCK_SIZE;
        // The camera's exposure is shared (auto exposure measured the main view; addPostProcessPass wrote it earlier).
        const bool postReady = m_postProcessPipeline && m_autoExposureBuildPipeline && m_autoExposureReducePipeline;
        const bool cameraAutoExposure = postReady && m_autoExposure;
        const shaderio::PushConstantSkybox skyboxSun = makeSkyboxSunPush();
        const bool ddgi = m_frame.ddgiAdded;
        const uint32_t ddgiIndex = m_frame.ddgiWriteIndex;

        for (uint32_t index = 0; index < MaxExtraViews; ++index)
        {
            ExtraView& view = m_extraViews[index];
            if (!view.Submitted || !view.Output || view.Size.width == 0 || view.Size.height == 0 || frameIndex >= view.UniformBuffers.size() || !postReady)
                continue;
            view.Recorded = true;

            NRI::Buffer* uniformBuffer = view.UniformBuffers[frameIndex].get();
            NRI::Buffer* cullViewBuffer = view.CullViewBuffers[frameIndex].get();
            const glm::mat4 viewProjection = view.Projection * view.View;
            // An asset preview keeps its own brightness whatever the scene's exposure is doing.
            const bool autoExposure = cameraAutoExposure && !view.Preview;

            // What this camera sees of the draw list: the frustum test only (no depth pyramid of its own).
            ViewDrawResources draws;
            if (cullingReady)
            {
                shaderio::CullView cull{};
                cull.frustum = shaderio::Frustum{ viewProjection };
                cull.viewProj = viewProjection;
                cull.previousViewProj = viewProjection;
                std::copy(m_drawBucketStarts.begin(), m_drawBucketStarts.end(), cull.bucketStart);
                cull.entryCount = entryCount;
                cull.blockCount = blockCount;
                cull.flags = m_instanceCullingEnabled ? shaderio::CULL_INSTANCES : 0u;
                memcpy(cullViewBuffer->map(0, sizeof(cull)), &cull, sizeof(cull));
                cullViewBuffer->unmap();

                draws.VisibleInstances = m_renderGraph.CreateBuffer("Extra View Visible Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
                draws.Commands = m_renderGraph.CreateBuffer("Extra View Draw Commands", { sizeof(shaderio::MeshTasksIndirectCommand) * entryCount, NRI::BufferUsage::Indirect });
                draws.Counts = m_renderGraph.CreateBuffer("Extra View Draw Counts", { sizeof(shaderio::CullCounts), NRI::BufferUsage::Indirect });
                // Where the shader puts entries the occlusion test rejected: never written without that test.
                draws.LateInstances = m_renderGraph.CreateBuffer("Extra View Late Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
                const RGBuffer flags = m_renderGraph.CreateBuffer("Extra View Cull Flags", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
                const RGBuffer blocks = m_renderGraph.CreateBuffer("Extra View Cull Blocks", { sizeof(shaderio::CullBlock) * blockCount, NRI::BufferUsage::Storage });
                addCullingDispatchPass(kCullNames[index], draws, flags, blocks, entryCount, RGTexture{}, uniformBuffer, cullViewBuffer);
            }

            RGTextureDesc targetDesc;
            targetDesc.Size = RGSize::Absolute;
            targetDesc.Width = view.Size.width;
            targetDesc.Height = view.Size.height;
            targetDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
            const RGTexture hdr = m_renderGraph.CreateTexture(kHdrNames[index], targetDesc);
            targetDesc.Format = NRI::ImageFormat::R32SINT;
            const RGTexture entity = m_renderGraph.CreateTexture(kEntityNames[index], targetDesc);
            targetDesc.Format = NRI::ImageFormat::None;
            targetDesc.Usage = NRI::TextureUsage::DepthStencilAttachment;
            const RGTexture depth = m_renderGraph.CreateTexture(kDepthNames[index], targetDesc);
            const RGTexture output = m_renderGraph.ImportTexture(kOutputNames[index], view.Output.get());
            const NRI::Extent2D extent = view.Size;

            m_renderGraph.AddPass(kForwardNames[index], RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.ColorTarget(hdr, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                    builder.ColorTarget(entity, NRI::LoadOP::clear, NRI::StoreOP::store, { -1.0f, 0.0f, 0.0f, 0.0f }); // R32SINT
                    builder.DepthTarget(depth, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0 });                    // reverse Z
                    // Forward shading samples the DDGI atlases (through the uniforms) when DDGI ran this frame.
                    if (ddgi)
                    {
                        builder.Read(resources.DDGIIrradiance[ddgiIndex]);
                        builder.Read(resources.DDGIDistance[ddgiIndex]);
                    }
                    // The forward shading's shadow rays (ray query).
                    if (resources.TLAS.IsValid())
                        builder.Read(resources.TLAS, RGBufferAccess::AccelerationStructureRead);
                    builder.SetRenderArea(extent);
                    ReadGpuScene(builder, resources);
                    ReadViewDraws(builder, draws);
                },
                [this, draws, uniformBuffer, skyboxSun](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    const uint64_t uniforms = uniformBuffer->getDeviceAddress();
                    const bool hasDraws = draws.Commands.IsValid();
                    MeshletDrawCursor cursor = hasDraws ? beginMeshletDraws(context, draws) : MeshletDrawCursor{};
                    cursor.references.matrixReference = uniforms;
                    cursor.references.ssaoTextureIndex = 0xFFFFFFFF;
                    // ClusterStats count the camera's clusters only.
                    cursor.taskFlags &= ~shaderio::MESHLET_COUNT_TRIANGLES;
                    const uint32_t taskFlags = cursor.taskFlags;

                    cmd.setDepthTestEnable(true);
                    cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, NRI::ColorComponent::All);
                    cmd.setColorBlendEnable(1, false);
                    cmd.setColorWriteMask(1, NRI::ColorComponent::All);

                    // A. Opaque and masked surfaces, lit in the fragment shader (no G-buffer here).
                    if (hasDraws && m_transparentLitPipeline)
                    {
                        cursor.taskFlags = taskFlags | shaderio::MESHLET_SHADE_OPAQUE;
                        drawMeshletBucket(cmd, cursor, RenderBucket::Opaque, *m_transparentLitPipeline, NRI::CullMode::Back, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::OpaqueDoubleSided, *m_transparentLitPipeline, NRI::CullMode::None, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::Mask, *m_transparentLitPipeline, NRI::CullMode::Back, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::MaskDoubleSided, *m_transparentLitPipeline, NRI::CullMode::None, true, false);
                        cursor.taskFlags = taskFlags;
                    }

                    // B. Unlit surfaces.
                    if (hasDraws && m_unlitPipeline)
                    {
                        drawMeshletBucket(cmd, cursor, RenderBucket::Unlit, *m_unlitPipeline, NRI::CullMode::Back, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::UnlitDoubleSided, *m_unlitPipeline, NRI::CullMode::None, true, false);
                    }

                    // C. The sky where nothing was drawn (depth 0).
                    if (m_skyboxPipeline && m_environmentCubemap)
                    {
                        cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_skyboxPipeline);
                        cursor.boundPipeline = m_skyboxPipeline.get();
                        cmd.setCullMode(NRI::CullMode::None);
                        cmd.setDepthWriteEnable(false);
                        cmd.setColorBlendEnable(0, false);

                        shaderio::PushConstantSkybox skyboxPush = skyboxSun;
                        skyboxPush.matrixReference = uniforms;
                        skyboxPush.cubemapIndex = m_environmentCubemap->GetDescriptorIndexSlot();
                        cmd.pushData(&skyboxPush, sizeof(shaderio::PushConstantSkybox));
                        cmd.drawMeshTasks(1, 1, 1);
                    }

                    // D. Transparent surfaces, blended (sorted back to front for the main camera, which is close enough here).
                    if (hasDraws && m_unlitPipeline && m_transparentLitPipeline)
                    {
                        const NRI::ColorBlendEquation blendEquation
                        {
                            .srcColorBlendFactor = NRI::BlendFactor::One,
                            .dstColorBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
                            .colorBlendOp = NRI::BlendOp::Add,
                            .srcAlphaBlendFactor = NRI::BlendFactor::One,
                            .dstAlphaBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
                            .alphaBlendOp = NRI::BlendOp::Add,
                        };
                        cmd.setColorBlendEquation(0, blendEquation);
                        drawMeshletBucket(cmd, cursor, RenderBucket::Transparent, *m_transparentLitPipeline, NRI::CullMode::Back, false, true);
                        drawMeshletBucket(cmd, cursor, RenderBucket::TransparentDoubleSided, *m_transparentLitPipeline, NRI::CullMode::None, false, true);
                        drawMeshletBucket(cmd, cursor, RenderBucket::TransparentUnlit, *m_unlitPipeline, NRI::CullMode::Back, false, true);
                        drawMeshletBucket(cmd, cursor, RenderBucket::TransparentUnlitDoubleSided, *m_unlitPipeline, NRI::CullMode::None, false, true);
                        cmd.setColorBlendEnable(0, false);
                        cmd.setDepthWriteEnable(true);
                    }
                });

            // HDR -> the view's image, with the camera's exposure and tonemapper.
            m_renderGraph.AddPass(kPostNames[index], RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.Read(hdr);
                    if (autoExposure)
                        builder.Read(resources.AutoExposure);
                    builder.ColorTarget(output, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                    builder.SetRenderArea(extent);
                },
                [this, hdr, autoExposure, exposureTexture = resources.AutoExposure, uniformBuffer](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_postProcessPipeline);
                    cmd.setCullMode(NRI::CullMode::None);
                    cmd.setDepthTestEnable(false);
                    cmd.setDepthWriteEnable(false);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, NRI::ColorComponent::All);

                    shaderio::PushConstantPostProcess postPush{};
                    postPush.matrixReference = uniformBuffer->getDeviceAddress();
                    postPush.hdrTextureIndex = context.Slot(hdr);
                    postPush.autoExposureTextureIndex = autoExposure ? context.Slot(exposureTexture) : 0xFFFFFFFF;
                    postPush.debugMode = 0;
                    postPush.tonemapMode = m_tonemapMode;
                    postPush.autoExposureEnabled = autoExposure ? 1u : 0u;
                    postPush.exposureCompensation = m_exposureCompensation;
                    postPush.autoExposureMinEV = m_autoExposureMinEV;
                    postPush.autoExposureMaxEV = m_autoExposureMaxEV;
                    cmd.pushData(&postPush, sizeof(shaderio::PushConstantPostProcess));
                    cmd.drawMeshTasks(1, 1, 1);
                });

            // The 2D overlay's world-space content (lines, sprites, text) and this view's grid, depth tested against its depth.
            const std::optional<shaderio::PushConstantGrid> grid = view.HasGrid ? std::optional<shaderio::PushConstantGrid>(view.Grid) : std::nullopt;
            m_renderGraph.AddPass(kOverlayNames[index], RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.ColorTarget(output, NRI::LoadOP::load, NRI::StoreOP::store);
                    builder.ColorTarget(entity, NRI::LoadOP::load, NRI::StoreOP::store);
                    builder.DepthTarget(depth, NRI::LoadOP::load, NRI::StoreOP::store);
                    builder.SetRenderArea(extent);
                },
                [this, uniformBuffer, grid](RGPassContext& context)
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
                    cmd.setColorWriteMask(0, NRI::ColorComponent::All);
                    cmd.setColorBlendEnable(1, false);
                    cmd.setColorWriteMask(1, NRI::ColorComponent::All);

                    m_renderer2D->FlushWorld(cmd, *uniformBuffer, frameIndex, grid ? &*grid : nullptr);
                });
        }
    }
}
