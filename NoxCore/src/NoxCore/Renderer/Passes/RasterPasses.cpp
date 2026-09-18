// Raster passes: GPU scene update, TLAS build, instance culling, visibility buffer, G-buffer resolve, forward 3D (unlit, skybox,
// transparent, DDGI probes).
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

    void Renderer::addBLASBuildPass()
    {
        if (m_blasBuilds.empty())
            return;

        // The frame's share of queued builds, chosen here on the main thread: the scratch buffer grows and the GPU scene
        // learns the new BLAS (its TLAS records reference them from the next frame's instance list, after this build).
        uint64_t primitives = 0;
        size_t count = 0;
        uint64_t scratchSize = 0;
        std::vector<std::pair<NRI::AccelerationStructureBuildDesc, NRI::AccelerationStructure*>> builds;
        for (; count < m_blasBuilds.size() && primitives < BlasBuildPrimitivesPerFrame; ++count)
        {
            const BlasBuild& build = m_blasBuilds[count];
            MeshBLAS& blas = m_meshBLASes[build.blasId];
            const NRI::AccelerationStructureBuildDesc desc = blasBuildDesc(build);
            scratchSize = std::max(scratchSize, m_device->getAccelerationStructureBuildSizes(desc).buildScratchSize);
            builds.emplace_back(desc, blas.as.get());
            m_gpuScene.SetMeshBlas(build.meshSlot, blas.as->getDeviceAddress());
            primitives += build.indices.count / 3;
        }
        m_blasBuilds.erase(m_blasBuilds.begin(), m_blasBuilds.begin() + static_cast<std::ptrdiff_t>(count));

        if (!m_blasScratch || m_blasScratch->getSize() < scratchSize)
        {
            if (m_blasScratch)
            {
                std::scoped_lock lock(m_deferredReleasesMutex);
                m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(m_blasScratch) });
            }
            m_blasScratch = m_device->createBuffer(NRI::BufferDesc{ .size = scratchSize, .usage = NRI::BufferUsage::AccelerationStructureScratch });
        }

        // Reads the geometry streams, which the frame's submission waits on the upload timeline for.
        m_renderGraph.AddPass("BLAS Build", RGPassFlags::NeverCull,
            [&](RGBuilder&) {},
            [this, builds = std::move(builds)](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                for (const auto& [desc, blas] : builds)
                {
                    // One scratch buffer for all of them: each build waits for the one before.
                    cmd.buildAccelerationStructure(desc, m_blasScratch->getDeviceAddress(), *blas);
                    cmd.accelerationStructureBarrier(NRI::AccelerationStructureBarrierType::BuildToBuild);
                }
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

    namespace
    {
        // The pyramid a culling phase tests against; without one (first frame, history reset, occlusion off) the shader
        // skips the test.
        void fillHiZPushConstants(const RenderGraph& graph, const RGPassContext& context, RGTexture hiZ, shaderio::PushConstantInstanceCulling& push)
        {
            push.hiZTextureIndex = 0xFFFFFFFF;
            if (!hiZ.IsValid())
                return;

            push.hiZTextureIndex = context.Slot(hiZ);
            const RGTextureKey& key = graph.GetTextureKey(hiZ);
            push.hiZWidth = key.Width;
            push.hiZHeight = key.Height;
            push.hiZMipCount = key.MipLevels;
        }
    }

    void Renderer::addInstanceCullingPass()
    {
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const uint32_t entryCount = static_cast<uint32_t>(m_drawList.size());
        const bool pipelinesReady = std::all_of(m_instanceCullingPipelines.begin(), m_instanceCullingPipelines.end(), [](const auto& pipeline) { return pipeline != nullptr; });
        if (entryCount == 0 || !pipelinesReady)
            return;

        const uint32_t blockCount = (entryCount + shaderio::CULL_BLOCK_SIZE - 1) / shaderio::CULL_BLOCK_SIZE;
        ViewDrawResources& draws = resources.CameraDraws;
        draws.VisibleInstances = m_renderGraph.CreateBuffer("Camera Visible Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
        draws.Commands = m_renderGraph.CreateBuffer("Camera Draw Commands", { sizeof(shaderio::MeshTasksIndirectCommand) * entryCount, NRI::BufferUsage::Indirect });
        draws.Counts = m_renderGraph.CreateBuffer("Camera Draw Counts", { sizeof(shaderio::CullCounts), NRI::BufferUsage::Indirect });
        draws.LateInstances = m_renderGraph.CreateBuffer("Camera Late Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
        draws.LateCommands = m_renderGraph.CreateBuffer("Camera Late Commands", { sizeof(shaderio::MeshTasksIndirectCommand) * entryCount, NRI::BufferUsage::Indirect });
        const RGBuffer flags = m_renderGraph.CreateBuffer("Camera Cull Flags", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
        const RGBuffer blocks = m_renderGraph.CreateBuffer("Camera Cull Blocks", { sizeof(shaderio::CullBlock) * blockCount, NRI::BufferUsage::Storage });

        m_renderGraph.AddPass("Instance Culling", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                ReadGpuScene(builder, resources);
                // The pyramid of the previous frame (written later this frame by the Hi-Z build).
                ReadIfValid(builder, resources.CameraHiZ);
                builder.Write(flags);
                builder.Write(blocks);
                builder.Write(draws.VisibleInstances);
                builder.Write(draws.Commands);
                builder.Write(draws.Counts);
                builder.Write(draws.LateInstances);
            },
            [this, draws, flags, blocks, entryCount, blockCount, hiZ = resources.CameraHiZ](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                shaderio::PushConstantInstanceCulling push{};
                push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                push.viewReference = m_cullViewBuffers[frameIndex]->getDeviceAddress();
                push.drawListReference = m_drawListBuffers[frameIndex]->getDeviceAddress();
                push.flagsReference = context.Buffer(flags).getDeviceAddress();
                push.blocksReference = context.Buffer(blocks).getDeviceAddress();
                push.countsReference = context.Buffer(draws.Counts).getDeviceAddress();
                push.visibleInstancesReference = context.Buffer(draws.VisibleInstances).getDeviceAddress();
                push.commandsReference = context.Buffer(draws.Commands).getDeviceAddress();
                push.lateInstancesReference = context.Buffer(draws.LateInstances).getDeviceAddress();
                fillHiZPushConstants(m_renderGraph, context, hiZ, push);

                // Visibility per entry, visible entries per block, offsets and draw counts, then the compacted writes. Each
                // step reads what the previous one wrote.
                const std::array<NRI::BufferBarrierDesc, 3> stepBarriers = {
                    NRI::BufferBarrierDesc{ &context.Buffer(flags), { NRI::AccessBits::ShaderWrite, NRI::StageBits::Compute }, { NRI::AccessBits::ShaderRead, NRI::StageBits::Compute } },
                    NRI::BufferBarrierDesc{ &context.Buffer(blocks), { NRI::AccessBits::ShaderWrite, NRI::StageBits::Compute }, { NRI::AccessBits::ShaderRead | NRI::AccessBits::ShaderWrite, NRI::StageBits::Compute } },
                    NRI::BufferBarrierDesc{ &context.Buffer(draws.Counts), { NRI::AccessBits::ShaderWrite, NRI::StageBits::Compute }, { NRI::AccessBits::ShaderRead, NRI::StageBits::Compute } }
                };

                const std::array<uint32_t, 4> groupCounts = { (entryCount + 255) / 256, blockCount, 1, blockCount };
                for (size_t step = 0; step < groupCounts.size(); ++step)
                {
                    if (step > 0)
                        cmd.resourceBarriers({}, std::span<const NRI::BufferBarrierDesc>(stepBarriers).subspan(0, step == 1 ? 1 : (step == 2 ? 2 : 3)));

                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_instanceCullingPipelines[step]);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch(groupCounts[step], 1, 1);
                }
            });

        // Visible counts for the stats, read once this frame slot finished (Renderer::readCullStats).
        const RGBuffer stats = m_renderGraph.ImportBuffer("Cull Stats Staging", m_cullStatsBuffers[frameIndex].get());
        m_cullStatsPending[frameIndex] = true;
        m_renderGraph.AddPass("Cull Stats Readback", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(draws.Counts, RGBufferAccess::CopySource);
                builder.Write(stats, RGBufferAccess::CopyDestination);
            },
            [counts = draws.Counts, stats](RGPassContext& context)
            {
                context.Cmd().copyBuffer(context.Buffer(counts), context.Buffer(stats), NRI::BufferCopyRegion{ .size = sizeof(shaderio::CullCounts) });
            });
    }

    void Renderer::addVisibilityPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        // The visibility passes count what they draw after the LOD cut (Renderer::readClusterStats).
        const bool drawsGeometry = resources.CameraDraws.Commands.IsValid() && m_visibilityPipeline;
        if (drawsGeometry)
        {
            m_renderGraph.AddPass("Cluster Stats Clear", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Write(resources.ClusterStats, RGBufferAccess::CopyDestination);
                },
                [stats = resources.ClusterStats](RGPassContext& context)
                {
                    context.Cmd().fillBuffer(context.Buffer(stats), 0, sizeof(shaderio::ClusterStats), 0);
                });
        }

        m_renderGraph.AddPass("Visibility", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.Visibility, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                // Reverse-Z: clear to 0; stored for every later depth consumer.
                builder.DepthTarget(resources.Depth, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0 });
                builder.SetRenderArea(m_frame.renderExtent);
                ReadGpuScene(builder, resources);
                ReadViewDraws(builder, resources.CameraDraws);
                if (drawsGeometry)
                    builder.Write(resources.ClusterStats);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                // Opaque depth state on top of the command buffer baseline (Renderer::applyCommandBufferBaseline).
                cmd.setCullMode(NRI::CullMode::Back);
                cmd.setDepthTestEnable(true);
                cmd.setDepthWriteEnable(true);
                cmd.setDepthCompareOp(NRI::CompareOp::Greater);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                if (res->CameraDraws.Commands.IsValid() && m_visibilityPipeline)
                {
                    // All PBR opaque & mask geometry rasterizes to the visibility buffer and depth: the first four buckets.
                    MeshletDrawCursor cursor = beginMeshletDraws(context, res->CameraDraws);
                    cursor.taskFlags |= shaderio::MESHLET_COUNT_TRIANGLES;
                    drawMeshletBucket(cmd, cursor, RenderBucket::Opaque, *m_visibilityPipeline, NRI::CullMode::Back, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::OpaqueDoubleSided, *m_visibilityPipeline, NRI::CullMode::None, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::Mask, *m_visibilityPipeline, NRI::CullMode::Back, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::MaskDoubleSided, *m_visibilityPipeline, NRI::CullMode::None, true, false);
                }
            });
    }

    void Renderer::addHiZBuildPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.CameraHiZ.IsValid() || !m_hiZBuildPipeline)
            return;

        const RGTextureKey& key = m_renderGraph.GetTextureKey(resources.CameraHiZ);
        m_renderGraph.AddPass("Hi-Z Build", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Write(resources.CameraHiZ, RGTextureAccess::StorageWrite);
            },
            [this, hiZ = resources.CameraHiZ, depth = resources.Depth, key](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_hiZBuildPipeline);

                // Level 0 reduces the depth buffer, every next level the level before it.
                for (uint32_t mip = 0; mip < key.MipLevels; ++mip)
                {
                    if (mip > 0)
                    {
                        const NRI::TextureBarrierDesc barrier{ &context.Texture(hiZ),
                                                               { NRI::AccessBits::ShaderWrite, NRI::StageBits::Compute },
                                                               { NRI::AccessBits::ShaderRead, NRI::StageBits::Compute } };
                        cmd.resourceBarriers(std::span<const NRI::TextureBarrierDesc>(&barrier, 1), {});
                    }

                    shaderio::PushConstantHiZBuild push{};
                    push.sourceTextureIndex = mip == 0 ? context.Slot(depth) : context.Slot(hiZ);
                    push.outputStorageIndex = context.StorageSlot(hiZ, mip);
                    push.width = std::max(key.Width >> mip, 1u);
                    push.height = std::max(key.Height >> mip, 1u);
                    push.sourceMip = mip == 0 ? 0 : mip - 1;
                    push.sourceIsDepth = mip == 0 ? 1 : 0;

                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                }
            });
    }

    void Renderer::addInstanceCullingLatePass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const ViewDrawResources& draws = resources.CameraDraws;
        if (!draws.LateCommands.IsValid() || !m_instanceCullingPipelines[4])
            return;

        const uint32_t entryCount = static_cast<uint32_t>(m_drawList.size());
        m_renderGraph.AddPass("Instance Culling Late", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                ReadGpuScene(builder, resources);
                ReadIfValid(builder, resources.CameraHiZ);
                builder.Read(draws.Counts);
                builder.Read(draws.LateInstances);
                builder.Write(draws.LateCommands);
            },
            [this, draws, entryCount, hiZ = resources.CameraHiZ](RGPassContext& context)
            {
                shaderio::PushConstantInstanceCulling push{};
                push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                push.viewReference = m_cullViewBuffers[frameIndex]->getDeviceAddress();
                push.countsReference = context.Buffer(draws.Counts).getDeviceAddress();
                push.lateInstancesReference = context.Buffer(draws.LateInstances).getDeviceAddress();
                push.lateCommandsReference = context.Buffer(draws.LateCommands).getDeviceAddress();
                fillHiZPushConstants(m_renderGraph, context, hiZ, push);

                NRI::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_instanceCullingPipelines[4]);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch((entryCount + 255) / 256, 1, 1);
            });
    }

    void Renderer::addVisibilityLatePass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.CameraDraws.LateCommands.IsValid() || !m_visibilityPipeline)
            return;

        // What phase 2 found visible after all: the same targets, loaded (§5.6.4).
        m_renderGraph.AddPass("Visibility Late", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.Visibility, NRI::LoadOP::load, NRI::StoreOP::store);
                builder.DepthTarget(resources.Depth, NRI::LoadOP::load, NRI::StoreOP::store);
                builder.SetRenderArea(m_frame.renderExtent);
                ReadGpuScene(builder, resources);
                ReadViewDraws(builder, resources.CameraDraws, true);
                builder.Write(resources.ClusterStats);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();

                cmd.setCullMode(NRI::CullMode::Back);
                cmd.setDepthTestEnable(true);
                cmd.setDepthWriteEnable(true);
                cmd.setDepthCompareOp(NRI::CompareOp::Greater);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                MeshletDrawCursor cursor = beginMeshletDraws(context, res->CameraDraws, true);
                cursor.taskFlags |= shaderio::MESHLET_COUNT_TRIANGLES;
                drawMeshletBucket(cmd, cursor, RenderBucket::Opaque, *m_visibilityPipeline, NRI::CullMode::Back, true, false);
                drawMeshletBucket(cmd, cursor, RenderBucket::OpaqueDoubleSided, *m_visibilityPipeline, NRI::CullMode::None, true, false);
                drawMeshletBucket(cmd, cursor, RenderBucket::Mask, *m_visibilityPipeline, NRI::CullMode::Back, true, false);
                drawMeshletBucket(cmd, cursor, RenderBucket::MaskDoubleSided, *m_visibilityPipeline, NRI::CullMode::None, true, false);
            });
    }

    void Renderer::addGBufferPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const bool resolveMaterials = m_gbufferPipeline && !m_drawList.empty();

        // Texture streaming feedback starts empty every frame; the material resolve and the transparent pass fill it.
        if (resolveMaterials)
        {
            m_renderGraph.AddPass("Mip Feedback Clear", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Write(resources.MipFeedback, RGBufferAccess::CopyDestination);
                },
                [feedback = resources.MipFeedback](RGPassContext& context)
                {
                    context.Cmd().fillBuffer(context.Buffer(feedback), 0, sizeof(uint32_t) * shaderio::MipFeedbackSlots, shaderio::MipFeedbackNone);
                });
        }

        // Decoupled material resolve, or only the clears when the scene has no meshes (entity IDs must read -1 for
        // picking, and later passes still find defined G-buffer contents).
        m_renderGraph.AddPass(resolveMaterials ? "G-Buffer" : "G-Buffer Clear", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                if (resolveMaterials)
                {
                    builder.Read(resources.Visibility);
                    // Read and written: the shader skips the atomic when the slot already holds its mip.
                    builder.Read(resources.MipFeedback);
                    builder.Write(resources.MipFeedback);
                }
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

                gbufferPush.visibilityTextureIndex = context.Slot(res->Visibility);
                gbufferPush.viewportSize = glm::vec2(rw, rh);
                gbufferPush.debugMode = m_debugMode;
                cmd.pushData(&gbufferPush, sizeof(shaderio::PushConstantVisibilityDebug));

                cmd.drawMeshTasks(1, 1, 1);
            });
    }

    void Renderer::addClusterStatsReadbackPass()
    {
        // After both visibility passes, read once this frame slot finished (Renderer::readClusterStats).
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.CameraDraws.Commands.IsValid() || !m_visibilityPipeline)
            return;

        const RGBuffer staging = m_renderGraph.ImportBuffer("Cluster Stats Staging", m_clusterStatsReadback[frameIndex].get());
        m_clusterStatsPending[frameIndex] = true;
        m_renderGraph.AddPass("Cluster Stats Readback", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.ClusterStats, RGBufferAccess::CopySource);
                builder.Write(staging, RGBufferAccess::CopyDestination);
            },
            [stats = resources.ClusterStats, staging](RGPassContext& context)
            {
                context.Cmd().copyBuffer(context.Buffer(stats), context.Buffer(staging), NRI::BufferCopyRegion{ .size = sizeof(shaderio::ClusterStats) });
            });
    }

    void Renderer::addMipFeedbackReadbackPass()
    {
        // Read once this frame slot finished (Renderer::readMipFeedback); only frames that cleared it have feedback.
        if (!m_gbufferPipeline || m_drawList.empty())
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const RGBuffer staging = m_renderGraph.ImportBuffer("Mip Feedback Staging", m_mipFeedbackReadback[frameIndex].get());
        m_mipFeedbackPending[frameIndex] = true;
        m_renderGraph.AddPass("Mip Feedback Readback", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.MipFeedback, RGBufferAccess::CopySource);
                builder.Write(staging, RGBufferAccess::CopyDestination);
            },
            [feedback = resources.MipFeedback, staging](RGPassContext& context)
            {
                context.Cmd().copyBuffer(context.Buffer(feedback), context.Buffer(staging),
                                         NRI::BufferCopyRegion{ .size = sizeof(uint32_t) * shaderio::MipFeedbackSlots });
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
                ReadViewDraws(builder, resources.CameraDraws);
                ReadViewDraws(builder, resources.CameraDraws, true);
                // Transparent surfaces report the mips their textures need (texture streaming feedback): read and written.
                builder.Read(resources.MipFeedback);
                builder.Write(resources.MipFeedback);
            },
            [this, res = &resources, drawProbes](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const uint32_t totalDDGIProbes = m_frame.totalDDGIProbes;
                const bool hasDraws = res->CameraDraws.Commands.IsValid();
                MeshletDrawCursor cursor = hasDraws ? beginMeshletDraws(context, res->CameraDraws) : MeshletDrawCursor{};

                // A. UNLIT MESHES
                if (hasDraws && m_unlitPipeline && (getBucketEntryCount(RenderBucket::Unlit) > 0 || getBucketEntryCount(RenderBucket::UnlitDoubleSided) > 0))
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

                    MeshletDrawCursor lateCursor = beginMeshletDraws(context, res->CameraDraws, true);
                    lateCursor.boundPipeline = cursor.boundPipeline;
                    drawMeshletBucket(cmd, lateCursor, RenderBucket::Unlit, *m_unlitPipeline, NRI::CullMode::Back, true, false);
                    drawMeshletBucket(cmd, lateCursor, RenderBucket::UnlitDoubleSided, *m_unlitPipeline, NRI::CullMode::None, true, false);
                    cursor.boundPipeline = lateCursor.boundPipeline;
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
                bool hasAnyTransparent = hasDraws && (getBucketEntryCount(RenderBucket::Transparent) > 0 || getBucketEntryCount(RenderBucket::TransparentDoubleSided) > 0 ||
                                                      getBucketEntryCount(RenderBucket::TransparentUnlit) > 0 || getBucketEntryCount(RenderBucket::TransparentUnlitDoubleSided) > 0);
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

                    MeshletDrawCursor lateCursor = beginMeshletDraws(context, res->CameraDraws, true);
                    lateCursor.boundPipeline = cursor.boundPipeline;
                    drawMeshletBucket(cmd, lateCursor, RenderBucket::Transparent, *m_transparentLitPipeline, NRI::CullMode::Back, false, true);
                    drawMeshletBucket(cmd, lateCursor, RenderBucket::TransparentDoubleSided, *m_transparentLitPipeline, NRI::CullMode::None, false, true);
                    drawMeshletBucket(cmd, lateCursor, RenderBucket::TransparentUnlit, *m_unlitPipeline, NRI::CullMode::Back, false, true);
                    drawMeshletBucket(cmd, lateCursor, RenderBucket::TransparentUnlitDoubleSided, *m_unlitPipeline, NRI::CullMode::None, false, true);
                    cursor.boundPipeline = lateCursor.boundPipeline;

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
