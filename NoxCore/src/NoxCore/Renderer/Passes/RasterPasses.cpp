// Raster passes: GPU scene update, TLAS build, instance culling, visibility buffer, G-buffer resolve, forward 3D (unlit, skybox,
// transparent, DDGI probes).
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"
#include "NoxCore/Core/Log.h"

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

    void Renderer::addComputeSkinningPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!m_computeSkinningPipeline || m_frame.skinningWorkCount == 0 ||
            !resources.SkinningSourceVertices.IsValid() || !resources.SkinningBoneMatrices.IsValid() ||
            !resources.SkinningWorkItems.IsValid() || !resources.SkinnedVertices.IsValid())
            return;

        m_frame.computeSkinningAdded = true;
        m_renderGraph.AddPass("Compute Skinning", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.SkinningSourceVertices);
                builder.Read(resources.SkinningBoneMatrices);
                builder.Read(resources.SkinningWorkItems);
                builder.Write(resources.SkinnedVertices);
            },
            [this, res = &resources, morphWeights = frameIndex < m_morphWeightBuffers.size() && m_morphWeightBuffers[frameIndex]
                                                        ? bufferReadRef(m_morphWeightBuffers[frameIndex]) : 0](RGPassContext& context)
            {
                shaderio::PushConstantSkinning push{};
                push.sourceVerticesReference = bufferReadRef(context.Buffer(res->SkinningSourceVertices));
                push.sourceSkinReference = bufferReadRef(m_skinStream.GetBuffer());
                push.morphRangesReference = morphWeights != 0 ? bufferReadRef(m_morphRangeStream.GetBuffer()) : 0;
                push.morphDeltasReference = bufferReadRef(m_morphDeltaStream.GetBuffer());
                push.morphWeightsReference = morphWeights;
                push.skinnedVerticesReference = bufferWriteRef(context.Buffer(res->SkinnedVertices));
                push.boneMatricesReference = bufferReadRef(context.Buffer(res->SkinningBoneMatrices));
                push.workItemsReference = bufferReadRef(context.Buffer(res->SkinningWorkItems));

                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_computeSkinningPipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch(m_frame.skinningWorkCount, 1, 1);
            });
    }

    void Renderer::addRayTracingOffsetPass()
    {
        // World Position Offset in the ray tracing scene (material graph step 4c part 4): the moved vertices of the instances a graph
        // offsets, after compute skinning (a skinned one moves from its skinned vertices) and before their BLAS is refit from them.
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!m_rayTracingOffsetGraphPipeline || m_frame.rayTracingOffsetWorkCount == 0 || !resources.RayTracingOffsetWorkItems.IsValid() ||
            !resources.RayTracingOffsetVertices.IsValid())
            return;
        m_renderGraph.AddPass("Ray Tracing Offset", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.RayTracingOffsetWorkItems);
                if (resources.SkinnedVertices.IsValid())
                    builder.Read(resources.SkinnedVertices);
                builder.Write(resources.RayTracingOffsetVertices);
            },
            [this, res = &resources, uniforms = bufferReadRef(m_uniformBuffers[frameIndex])](RGPassContext& context)
            {
                shaderio::PushConstantRayTracingOffset push{};
                push.uniformsReference = uniforms;
                push.workItemsReference = bufferReadRef(context.Buffer(res->RayTracingOffsetWorkItems));
                push.skinnedVerticesReference = res->SkinnedVertices.IsValid() ? bufferReadRef(context.Buffer(res->SkinnedVertices)) : 0;
                push.outputReference = bufferWriteRef(context.Buffer(res->RayTracingOffsetVertices));
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_rayTracingOffsetGraphPipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch(m_frame.rayTracingOffsetWorkCount, 1, 1);
            });
    }

    void Renderer::addAnimatedBLASPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (m_animatedBLASUpdates.empty() || !m_animatedBLASScratch ||
            (!resources.SkinnedVertices.IsValid() && !resources.RayTracingOffsetVertices.IsValid()))
            return;

        const bool buildsTLAS = m_tlasNeedBuild && m_hasTLASBuild && m_sceneTLAS && m_tlasScratchBuffer;
        m_frame.animatedBLASAdded = true;
        m_renderGraph.AddPass("Animated BLAS Update", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                // Compute skinning (and the Ray Tracing Offset pass) write these buffers immediately before the BLAS update. The
                // build-input access gives the graph the compute-write -> acceleration-structure-read dependency with the correct stages.
                if (resources.SkinnedVertices.IsValid())
                    builder.Read(resources.SkinnedVertices, RGBufferAccess::AccelerationStructureBuildInput);
                if (resources.RayTracingOffsetVertices.IsValid())
                    builder.Read(resources.RayTracingOffsetVertices, RGBufferAccess::AccelerationStructureBuildInput);
            },
            [this, buildsTLAS](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                if (m_rtIndexStream.GetBuffer())
                {
                    const nri::BufferBarrierDesc indexReady{
                        .buffer = m_rtIndexStream.GetBuffer().handle(),
                        .before = { nri::AccessBits::None, nri::StageBits::None },
                        .after = { nri::AccessBits::ShaderRead | nri::AccessBits::AccelerationStructureRead,
                                   nri::StageBits::AccelerationStructureBuild | nri::StageBits::RayTracing | nri::StageBits::Compute | nri::StageBits::Fragment }
                    };
                    cmd.resourceBarriers({}, std::span(&indexReady, 1));
                }
                const uint64_t scratchAddress = m_animatedBLASScratch.getDeviceAddress();
                for (size_t index = 0; index < m_animatedBLASUpdates.size(); ++index)
                {
                    const AnimatedBLASUpdate& update = m_animatedBLASUpdates[index];
                    AnimatedBLAS& animated = m_animatedBLASes[update.instanceSlot];
                    const NriAccelerationStructureBuildDesc buildDesc = update.desc.toNri();
                    if (update.rebuild)
                        nriCmdBuildAccelerationStructure(cmd.handle(), &buildDesc, scratchAddress, animated.Resource.as.handle());
                    else
                        nriCmdUpdateAccelerationStructure(cmd.handle(), &buildDesc, scratchAddress, animated.Resource.as.handle(), animated.Resource.as.handle());

                    // Every animated BLAS shares one scratch buffer, so the next update must wait for this one.
                    if (index + 1 < m_animatedBLASUpdates.size())
                        nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_BUILD);
                }

                // A changed BLAS address is consumed by this frame's TLAS build. In steady state the TLAS address stays
                // unchanged and ray queries consume the updated BLAS directly.
                nriCmdAccelerationStructureBarrier(cmd.handle(), buildsTLAS ? NRI_AS_BARRIER_BUILD_TO_BUILD
                                                                            : NRI_AS_BARRIER_BUILD_TO_SHADER_READ);
            });

        for (const AnimatedBLASUpdate& update : m_animatedBLASUpdates)
            m_animatedBLASes[update.instanceSlot].built = true;
    }

    void Renderer::addBLASBuildPass()
    {
        if (m_blasBuilds.empty() && m_blasCompactions.empty())
            return;

        // Compactions measured in an earlier frame: the copy into the compacted structure, recorded before this frame's
        // builds; the GPU scene points at the compacted BLAS (the old one stays until frames tracing it are done -- one
        // frame more, as this frame's instance list may already reference it).
        std::vector<std::pair<NriAccelerationStructure, NriAccelerationStructure>> copies;
        for (BlasCompaction& compaction : m_blasCompactions)
        {
            MeshBLAS& blas = m_meshBLASes[compaction.blasId];
            if (blas.serial != compaction.compacted.serial || !blas.as)
                continue;
            copies.emplace_back(blas.as.handle(), compaction.compacted.as.handle());
            m_gpuScene.SetMeshBlas(compaction.meshSlot, compaction.compacted.as.getDeviceAddress());
            {
                std::scoped_lock lock(m_deferredReleasesMutex);
                m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT + 1, std::move(blas) });
            }
            blas = std::move(compaction.compacted);
        }
        m_blasCompactions.clear();

        // The frame's share of queued builds, chosen here on the main thread: the scratch buffer grows and the GPU scene
        // learns the new BLAS (its TLAS records reference them from the next frame's instance list, after this build).
        // Each build's compacted size is queried into this frame slot's pool (Renderer::readBlasCompactedSizes).
        uint64_t primitives = 0;
        size_t count = 0;
        uint64_t scratchSize = 0;
        std::vector<std::pair<ASBuildDesc, NriAccelerationStructure>> builds;
        std::vector<BlasCompactionQuery>& queries = m_blasCompactionQueries[frameIndex];
        queries.clear();
        std::vector<shaderio::ClusterIndexJob> clusterJobs;
        struct SelectedBuild
        {
            BlasBuild Build;
            NriAccelerationStructure Target = nullptr;
            uint64_t Serial = 0;
        };
        std::vector<SelectedBuild> selected;
        for (; count < m_blasBuilds.size() && primitives < BlasBuildPrimitivesPerFrame && count < BlasCompactionQueriesPerFrame; ++count)
        {
            BlasBuild build = m_blasBuilds[count];
            if (build.streamingId != UINT32_MAX)
            {
                if (!m_geometryStreamer.PrepareBlasBuild(build.streamingId, clusterJobs, build.indices, build.maxVertex))
                    continue;
                build.absolute = true;
                const ASBuildDesc buildDescOwner = blasBuildDesc(build);
                const NriAccelerationStructureBuildDesc buildDescC = buildDescOwner.toNri();
                const nri::AccelerationStructureBuildSizes sizes = nriGetAccelerationStructureBuildSizes(m_device.handle(), &buildDescC);
                MeshBLAS replacement;
                replacement.storageBuffer = m_device.createBuffer(nri::BufferDesc{ .size = sizes.accelerationStructureSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE });
                const NriAccelerationStructureDesc asDesc{
                    .type = NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL,
                    .storageBuffer = replacement.storageBuffer.handle(),
                    .bufferOffset = 0,
                    .size = sizes.accelerationStructureSize
                };
                NriAccelerationStructure rawAs = nullptr;
                nri::checkResult(nriCreateAccelerationStructure(m_device.handle(), &asDesc, &rawAs), "nriCreateAccelerationStructure");
                replacement.as = nri::AccelerationStructure(rawAs);
                replacement.serial = ++m_blasSerial;
                if (m_meshBLASes[build.blasId].as)
                {
                    std::scoped_lock lock(m_deferredReleasesMutex);
                    m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT + 1, std::move(m_meshBLASes[build.blasId]) });
                }
                m_meshBLASes[build.blasId] = std::move(replacement);
                if (build.blasId < m_blasSources.size())
                    m_blasSources[build.blasId].indices = build.indices;
                // The hit shaders read the new indices from the frame the TLAS references the new BLAS (both the next).
                m_gpuScene.UpdateMeshGeometry(build.meshSlot, build.indices.offset, build.indices.count / 3, build.maxVertex + 1);
            }
            selected.push_back({ build, m_meshBLASes[build.blasId].as.handle(), m_meshBLASes[build.blasId].serial });
            primitives += build.indices.count / 3;
        }
        m_blasBuilds.erase(m_blasBuilds.begin(), m_blasBuilds.begin() + static_cast<std::ptrdiff_t>(count));
        for (const SelectedBuild& selection : selected)
        {
            const BlasBuild& build = selection.Build;
            // Superseded in this frame by a streamed build of the same mesh: its structure is already on its way out.
            if (!selection.Target || selection.Target != m_meshBLASes[build.blasId].as.handle())
                continue;
            const ASBuildDesc desc = blasBuildDesc(build);
            const NriAccelerationStructureBuildDesc descC = desc.toNri();
            scratchSize = std::max(scratchSize, nriGetAccelerationStructureBuildSizes(m_device.handle(), &descC).buildScratchSize);
            builds.emplace_back(desc, selection.Target);
            queries.push_back({ build.blasId, build.meshSlot, selection.Serial });
            m_gpuScene.SetMeshBlas(build.meshSlot, nriGetAccelerationStructureDeviceAddress(selection.Target));
        }

        if (scratchSize > 0 && (!m_blasScratch || m_blasScratch.getSize() < scratchSize))
        {
            if (m_blasScratch)
            {
                std::scoped_lock lock(m_deferredReleasesMutex);
                m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(m_blasScratch) });
            }
            m_blasScratch = m_device.createBuffer(nri::BufferDesc{ .size = scratchSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_SCRATCH });
        }

        // The streamed builds' cluster index jobs, in this frame slot's buffer (its previous submission is done).
        if (!clusterJobs.empty())
        {
            const uint64_t jobBytes = sizeof(shaderio::ClusterIndexJob) * clusterJobs.size();
            nri::Buffer& jobBuffer = m_clusterIndexJobBuffers[frameIndex];
            if (!jobBuffer || jobBuffer.getSize() < jobBytes)
                jobBuffer = m_device.createBuffer(nri::BufferDesc{ .size = std::max<uint64_t>(jobBytes + jobBytes / 2, 64 * 1024), .usage = NRI_BUFFER_USAGE_UNIFORM });
            memcpy(jobBuffer.map(0, jobBytes), clusterJobs.data(), jobBytes);
            jobBuffer.unmap();
        }

        // Reads the geometry streams, which the frame's submission waits on the upload timeline for.
        m_renderGraph.AddPass("BLAS Build", RGPassFlags::NeverCull,
            [&](RGBuilder&) {},
            [this, copies = std::move(copies), builds = std::move(builds), queryPool = &m_blasCompactionQueryPools[frameIndex],
             jobCount = static_cast<uint32_t>(clusterJobs.size())](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                if (jobCount > 0 && m_clusterIndicesPipeline)
                {
                    const nri::BufferBarrierDesc beforeWrite{
                        .buffer = m_rtIndexStream.GetBuffer().handle(),
                        .before = { nri::AccessBits::ShaderRead | nri::AccessBits::AccelerationStructureRead,
                                    nri::StageBits::AccelerationStructureBuild | nri::StageBits::RayTracing | nri::StageBits::Compute | nri::StageBits::Fragment },
                        .after = { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }
                    };
                    cmd.resourceBarriers({}, std::span(&beforeWrite, 1));
                    // The streamed meshes' leaf cut triangles into the RT index stream, before the builds (and the hit shaders) read them.
                    shaderio::PushConstantClusterIndices push{};
                    push.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                    push.jobsReference = bufferReadRef(m_clusterIndexJobBuffers[frameIndex]);
                    push.indicesReference = bufferWriteRef(m_rtIndexStream.GetBuffer());
                    push.jobCount = jobCount;
                    push.groupCount = std::min(jobCount, 4096u);
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_clusterIndicesPipeline);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch(push.groupCount, 1, 1);
                    const nri::BufferBarrierDesc written{
                        .buffer = m_rtIndexStream.GetBuffer().handle(),
                        .before = { nri::AccessBits::ShaderWrite, nri::StageBits::Compute },
                        .after = { nri::AccessBits::ShaderRead | nri::AccessBits::AccelerationStructureRead,
                                   nri::StageBits::AccelerationStructureBuild | nri::StageBits::RayTracing | nri::StageBits::Compute | nri::StageBits::Fragment }
                    };
                    cmd.resourceBarriers({}, std::span(&written, 1));
                }
                for (const auto& [source, compacted] : copies)
                    nriCmdCopyAccelerationStructure(cmd.handle(), source, compacted, true);
                if (!copies.empty())
                    nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_BUILD);

                if (builds.empty())
                    return;
                cmd.resetQueries(*queryPool, 0, static_cast<uint32_t>(builds.size()));
                std::vector<NriAccelerationStructure> built;
                built.reserve(builds.size());
                for (const auto& [desc, blas] : builds)
                {
                    const NriAccelerationStructureBuildDesc descC = desc.toNri();
                    nriCmdBuildAccelerationStructure(cmd.handle(), &descC, m_blasScratch.getDeviceAddress(), blas);
                    nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_BUILD);
                    built.push_back(blas);
                }
                nriCmdWriteCompactedSizes(cmd.handle(), built.data(), static_cast<uint32_t>(built.size()), queryPool->handle(), 0);
            });
    }

    void Renderer::addGeometryPagesUpdatePass()
    {
        std::vector<nri::BufferCopyRegion>& drawRegions = m_geometryDrawPatchRegions[frameIndex];
        std::vector<nri::BufferCopyRegion>& pageRegions = m_geometryPagePatchRegions[frameIndex];
        std::vector<nri::BufferCopyRegion>& virtualPageRegions = m_virtualPagePatchRegions[frameIndex];
        std::vector<nri::BufferCopyRegion>& virtualTextureRegions = m_virtualTexturePatchRegions[frameIndex];
        if ((drawRegions.empty() && pageRegions.empty() && virtualPageRegions.empty() && virtualTextureRegions.empty()) || !m_geometryPatchStaging[frameIndex])
            return;

        m_renderGraph.AddPass("Streaming Update", RGPassFlags::NeverCull,
            [&](RGBuilder&) {},
            [this, drawRegions = std::move(drawRegions), pageRegions = std::move(pageRegions), virtualPageRegions = std::move(virtualPageRegions),
             virtualTextureRegions = std::move(virtualTextureRegions)](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                constexpr uint32_t readers = nri::StageBits::Task | nri::StageBits::Mesh | nri::StageBits::Compute | nri::StageBits::Fragment |
                                             nri::StageBits::RayTracing | nri::StageBits::AccelerationStructureBuild;
                nri::Buffer& staging = m_geometryPatchStaging[frameIndex];
                struct Target
                {
                    nri::Buffer* buffer;
                    const std::vector<nri::BufferCopyRegion>* regions;
                    uint32_t access; // how the frame's shaders use it
                };
                const Target targets[] = {
                    { &m_meshletDrawStream.GetBuffer(), &drawRegions, nri::AccessBits::ShaderRead },
                    { &m_geometryPageStream.GetBuffer(), &pageRegions, nri::AccessBits::ShaderRead | nri::AccessBits::ShaderWrite },
                    { m_virtualPageTable ? &m_virtualPageTable : nullptr, &virtualPageRegions, nri::AccessBits::ShaderRead },
                    { m_virtualTextureTable ? &m_virtualTextureTable : nullptr, &virtualTextureRegions, nri::AccessBits::ShaderRead },
                };
                std::vector<nri::BufferBarrierDesc> before, after;
                for (const Target& target : targets)
                {
                    if (!target.buffer || target.regions->empty())
                        continue;
                    before.push_back({ target.buffer->handle(), { target.access, readers }, { nri::AccessBits::TransferWrite, nri::StageBits::Transfer } });
                    after.push_back({ target.buffer->handle(), { nri::AccessBits::TransferWrite, nri::StageBits::Transfer }, { target.access, readers } });
                }
                cmd.resourceBarriers({}, before);
                for (const Target& target : targets)
                {
                    if (target.buffer && !target.regions->empty())
                        cmd.copyBuffer(staging, *target.buffer, std::span<const nri::BufferCopyRegion>(*target.regions));
                }
                cmd.resourceBarriers({}, after);
            });
    }

    void Renderer::addGeometryFeedbackPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!m_geometryFeedbackPipeline || m_drawList.empty() || m_geometryStreamer.GetStats().Meshes == 0 || !m_geometryStreamer.IsEnabled())
            return;

        // Which pages of the streamed meshes the LOD wants (GeometryStreamingFeedback.slang), read back once this frame slot is done
        // (Renderer::readGeometryFeedback).
        const RGBuffer feedback = m_renderGraph.ImportBuffer("Geometry Feedback", m_geometryFeedbackBuffer);
        const RGBuffer staging = m_renderGraph.ImportBuffer("Geometry Feedback Staging", m_geometryFeedbackReadback[frameIndex]);
        m_geometryFeedbackPending[frameIndex] = true;
        const uint32_t drawCount = static_cast<uint32_t>(m_drawList.size());
        m_renderGraph.AddPass("Geometry Streaming Feedback", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                ReadGpuScene(builder, resources);
                builder.Write(feedback);
            },
            [this, feedback, drawCount](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                const NriBuffer buffer = context.Buffer(feedback);
                // After the previous frame's pass and readback copy, the request count back to 0.
                const nri::BufferBarrierDesc reused{ buffer, { nri::AccessBits::ShaderRead | nri::AccessBits::ShaderWrite | nri::AccessBits::TransferRead,
                                                                nri::StageBits::Compute | nri::StageBits::Transfer },
                                                     { nri::AccessBits::TransferWrite, nri::StageBits::Transfer } };
                cmd.resourceBarriers({}, std::span(&reused, 1));
                nriCmdFillBuffer(cmd.handle(), buffer, 0, 16, 0);
                const nri::BufferBarrierDesc cleared{ buffer, { nri::AccessBits::TransferWrite, nri::StageBits::Transfer },
                                                      { nri::AccessBits::ShaderRead | nri::AccessBits::ShaderWrite, nri::StageBits::Compute } };
                cmd.resourceBarriers({}, std::span(&cleared, 1));

                shaderio::PushConstantGeometryFeedback push{};
                push.drawListReference = bufferReadRef(m_drawListBuffers[frameIndex]);
                push.feedbackReference = bufferWriteRef(buffer);
                push.drawCount = drawCount;
                push.groupCount = std::min(drawCount, 4096u);
                push.frame = static_cast<uint32_t>(m_sceneFrameCounter);
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_geometryFeedbackPipeline);
                // The main view, then every editor view drawn this frame (other viewports, asset previews): each wants the detail
                // its own camera needs. A page is reported once a frame whichever view asks first.
                push.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch(push.groupCount, 1, 1);
                for (const ExtraView& view : m_extraViews)
                {
                    if (!view.Recorded || frameIndex >= view.UniformBuffers.size() || !view.UniformBuffers[frameIndex])
                        continue;
                    push.matrixReference = bufferReadRef(view.UniformBuffers[frameIndex]);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch(push.groupCount, 1, 1);
                }
            });
        m_renderGraph.AddPass("Geometry Feedback Readback", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(feedback, RGBufferAccess::CopySource);
                builder.Write(staging, RGBufferAccess::CopyDestination);
            },
            [feedback, staging](RGPassContext& context)
            {
                { const nri::BufferCopyRegion _r = nri::BufferCopyRegion{ .size = sizeof(shaderio::GeometryStreamingFeedback) }; nriCmdCopyBuffer(context.Cmd().handle(), context.Buffer(feedback), context.Buffer(staging), &_r); };
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

    void Renderer::addCullingDispatchPass(const char* name, const ViewDrawResources& draws, RGBuffer flags, RGBuffer blocks, uint32_t entryCount,
                                          RGTexture hiZ, nri::Buffer* uniformBuffer, nri::Buffer* cullViewBuffer)
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const uint32_t blockCount = (entryCount + shaderio::CULL_BLOCK_SIZE - 1) / shaderio::CULL_BLOCK_SIZE;
        m_renderGraph.AddPass(name, RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                ReadGpuScene(builder, resources);
                ReadIfValid(builder, hiZ);
                builder.Write(flags);
                builder.Write(blocks);
                builder.Write(draws.VisibleInstances);
                builder.Write(draws.Commands);
                builder.Write(draws.Counts);
                builder.Write(draws.LateInstances);
            },
            [this, draws, flags, blocks, entryCount, blockCount, hiZ, uniformBuffer, cullViewBuffer](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();

                shaderio::PushConstantInstanceCulling push{};
                push.matrixReference = bufferReadRef(*uniformBuffer);
                push.viewReference = bufferReadRef(*cullViewBuffer);
                push.drawListReference = bufferReadRef(m_drawListBuffers[frameIndex]);
                push.flagsReference = context.BufferWriteRef(flags);
                push.blocksReference = context.BufferWriteRef(blocks);
                push.countsReference = context.BufferWriteRef(draws.Counts);
                push.visibleInstancesReference = context.BufferWriteRef(draws.VisibleInstances);
                push.commandsReference = context.BufferWriteRef(draws.Commands);
                push.lateInstancesReference = context.BufferWriteRef(draws.LateInstances);
                fillHiZPushConstants(m_renderGraph, context, hiZ, push);

                // Visibility per entry, visible entries per block, offsets and draw counts, then the compacted writes. Each
                // step reads what the previous one wrote.
                const std::array<nri::BufferBarrierDesc, 3> stepBarriers = {
                    nri::BufferBarrierDesc{ context.Buffer(flags), { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead, nri::StageBits::Compute } },
                    nri::BufferBarrierDesc{ context.Buffer(blocks), { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead | nri::AccessBits::ShaderWrite, nri::StageBits::Compute } },
                    nri::BufferBarrierDesc{ context.Buffer(draws.Counts), { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead, nri::StageBits::Compute } }
                };

                const std::array<uint32_t, 4> groupCounts = { (entryCount + 255) / 256, blockCount, 1, blockCount };
                for (size_t step = 0; step < groupCounts.size(); ++step)
                {
                    if (step > 0)
                        cmd.resourceBarriers({}, std::span<const nri::BufferBarrierDesc>(stepBarriers).subspan(0, step == 1 ? 1 : (step == 2 ? 2 : 3)));

                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_instanceCullingPipelines[step]);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch(groupCounts[step], 1, 1);
                }
            });
    }

    void Renderer::addInstanceCullingPass()
    {
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const uint32_t entryCount = static_cast<uint32_t>(m_drawList.size());
        const bool pipelinesReady = std::all_of(m_instanceCullingPipelines.begin(), m_instanceCullingPipelines.end(), [](const auto& pipeline) { return static_cast<bool>(pipeline); });
        if (entryCount == 0 || !pipelinesReady)
            return;

        const uint32_t blockCount = (entryCount + shaderio::CULL_BLOCK_SIZE - 1) / shaderio::CULL_BLOCK_SIZE;
        ViewDrawResources& draws = resources.CameraDraws;
        draws.VisibleInstances = m_renderGraph.CreateBuffer("Camera Visible Instances", { sizeof(uint32_t) * entryCount, NRI_BUFFER_USAGE_STORAGE });
        draws.Commands = m_renderGraph.CreateBuffer("Camera Draw Commands", { sizeof(shaderio::MeshTasksIndirectCommand) * entryCount, NRI_BUFFER_USAGE_INDIRECT });
        draws.Counts = m_renderGraph.CreateBuffer("Camera Draw Counts", { sizeof(shaderio::CullCounts), NRI_BUFFER_USAGE_INDIRECT });
        draws.LateInstances = m_renderGraph.CreateBuffer("Camera Late Instances", { sizeof(uint32_t) * entryCount, NRI_BUFFER_USAGE_STORAGE });
        draws.LateCommands = m_renderGraph.CreateBuffer("Camera Late Commands", { sizeof(shaderio::MeshTasksIndirectCommand) * entryCount, NRI_BUFFER_USAGE_INDIRECT });
        const RGBuffer flags = m_renderGraph.CreateBuffer("Camera Cull Flags", { sizeof(uint32_t) * entryCount, NRI_BUFFER_USAGE_STORAGE });
        const RGBuffer blocks = m_renderGraph.CreateBuffer("Camera Cull Blocks", { sizeof(shaderio::CullBlock) * blockCount, NRI_BUFFER_USAGE_STORAGE });

        // The pyramid of the previous frame (written later this frame by the Hi-Z build).
        addCullingDispatchPass("Instance Culling", draws, flags, blocks, entryCount, resources.CameraHiZ, &m_uniformBuffers[frameIndex],
                               &m_cullViewBuffers[frameIndex]);

        // Visible counts for the stats, read once this frame slot finished (Renderer::readCullStats).
        const RGBuffer stats = m_renderGraph.ImportBuffer("Cull Stats Staging", m_cullStatsBuffers[frameIndex]);
        m_cullStatsPending[frameIndex] = true;
        m_renderGraph.AddPass("Cull Stats Readback", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(draws.Counts, RGBufferAccess::CopySource);
                builder.Write(stats, RGBufferAccess::CopyDestination);
            },
            [counts = draws.Counts, stats](RGPassContext& context)
            {
                { const nri::BufferCopyRegion _r = nri::BufferCopyRegion{ .size = sizeof(shaderio::CullCounts) }; nriCmdCopyBuffer(context.Cmd().handle(), context.Buffer(counts), context.Buffer(stats), &_r); };
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
                    nriCmdFillBuffer(context.Cmd().handle(), context.Buffer(stats), 0, sizeof(shaderio::ClusterStats), 0);
                });
        }

        m_renderGraph.AddPass("Visibility", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.Visibility, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f });
                // Reverse-Z: clear to 0; stored for every later depth consumer.
                builder.DepthTarget(resources.Depth, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0 });
                builder.SetRenderArea(m_frame.renderExtent);
                ReadGpuScene(builder, resources);
                ReadIfValid(builder, resources.SkinnedVertices);
                ReadViewDraws(builder, resources.CameraDraws);
                if (drawsGeometry)
                    builder.Write(resources.ClusterStats);
            },
            [this, res = &resources](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();

                // Opaque depth state on top of the command buffer baseline (Renderer::applyCommandBufferBaseline).
                cmd.setCullMode(NRI_CULL_MODE_BACK);
                cmd.setDepthTestEnable(true);
                cmd.setDepthWriteEnable(true);
                // Near-or-equal (Unreal's CF_DepthNearOrEqual, Godot's GREATER_OR_EQUAL with reverse-Z): at an exact tie
                // between coplanar triangles the later one in draw order wins, the painter's order layered meshes (decal
                // strips over their wall in one index buffer) are authored for. The task shader keeps that order fixed.
                cmd.setDepthCompareOp(NRI_COMPARE_GREATER_OR_EQUAL);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);

                if (res->CameraDraws.Commands.IsValid() && m_visibilityPipeline)
                {
                    // All PBR opaque & mask geometry rasterizes to the visibility buffer and depth: the first four buckets.
                    MeshletDrawCursor cursor = beginMeshletDraws(context, res->CameraDraws);
                    cursor.taskFlags |= shaderio::MESHLET_COUNT_TRIANGLES;
                    drawMeshletBucket(cmd, cursor, RenderBucket::Opaque, m_visibilityPipeline, NRI_CULL_MODE_BACK, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::OpaqueDoubleSided, m_visibilityPipeline, NRI_CULL_MODE_NONE, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::Mask, *visibilityMaskPipeline(), NRI_CULL_MODE_BACK, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::MaskDoubleSided, *visibilityMaskPipeline(), NRI_CULL_MODE_NONE, true, false);
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
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_hiZBuildPipeline);

                // Level 0 reduces the depth buffer, every next level the level before it.
                for (uint32_t mip = 0; mip < key.MipLevels; ++mip)
                {
                    if (mip > 0)
                        MipChainSourceBarrier(cmd, context.Texture(hiZ), mip - 1, true);

                    shaderio::PushConstantHiZBuild push{};
                    push.sourceTextureIndex = mip == 0 ? context.Slot(depth) : context.Slot(hiZ);
                    push.outputStorageIndex = context.StorageSlot(hiZ, mip);
                    push.width = std::max(key.Width >> mip, 1u);
                    push.height = std::max(key.Height >> mip, 1u);
                    push.sourceMip = mip == 0 ? 0 : mip - 1;
                    push.sourceIsDepth = mip == 0 ? 1 : 0;

                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                    if (mip > 0)
                        MipChainSourceBarrier(cmd, context.Texture(hiZ), mip - 1, false);
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
                push.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                push.viewReference = bufferReadRef(m_cullViewBuffers[frameIndex]);
                push.countsReference = context.BufferWriteRef(draws.Counts);
                push.lateInstancesReference = context.BufferReadRef(draws.LateInstances);
                push.lateCommandsReference = context.BufferWriteRef(draws.LateCommands);
                fillHiZPushConstants(m_renderGraph, context, hiZ, push);

                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_instanceCullingPipelines[4]);
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
                builder.ColorTarget(resources.Visibility, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                builder.DepthTarget(resources.Depth, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                builder.SetRenderArea(m_frame.renderExtent);
                ReadGpuScene(builder, resources);
                ReadIfValid(builder, resources.SkinnedVertices);
                ReadViewDraws(builder, resources.CameraDraws, true);
                builder.Write(resources.ClusterStats);
            },
            [this, res = &resources](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();

                cmd.setCullMode(NRI_CULL_MODE_BACK);
                cmd.setDepthTestEnable(true);
                cmd.setDepthWriteEnable(true);
                cmd.setDepthCompareOp(NRI_COMPARE_GREATER_OR_EQUAL); // as the Visibility pass
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);

                MeshletDrawCursor cursor = beginMeshletDraws(context, res->CameraDraws, true);
                cursor.taskFlags |= shaderio::MESHLET_COUNT_TRIANGLES;
                drawMeshletBucket(cmd, cursor, RenderBucket::Opaque, m_visibilityPipeline, NRI_CULL_MODE_BACK, true, false);
                drawMeshletBucket(cmd, cursor, RenderBucket::OpaqueDoubleSided, m_visibilityPipeline, NRI_CULL_MODE_NONE, true, false);
                drawMeshletBucket(cmd, cursor, RenderBucket::Mask, *visibilityMaskPipeline(), NRI_CULL_MODE_BACK, true, false);
                drawMeshletBucket(cmd, cursor, RenderBucket::MaskDoubleSided, *visibilityMaskPipeline(), NRI_CULL_MODE_NONE, true, false);
            });
    }

    void Renderer::addGBufferPass()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const bool nrdAvailable = isNRDInitialized();
        const bool dlssNeedsVelocity = m_dlssEnabled && m_dlssMode != NRI_UPSCALER_MODE_OFF && isDLSSSupported();
        const bool nrdShadowsNeedVelocity = uniformData.enableRTShadows != 0 && m_shadowMaskPipeline &&
                                            m_nrdShadowsEnabled && nrdAvailable;
        const bool nrdReflectionsNeedVelocity = !m_frame.runPathTracer && uniformData.enableRTReflections != 0 &&
                                                m_reflectionPipeline &&
                                                m_nrdReflectionDenoiser != Nox::NRDReflectionDenoiser::Off && nrdAvailable;
        const bool restirGINeedsVelocity = (!m_frame.runPathTracer || m_frame.pathTracerUsesRTXDI) && isReSTIRGIRequested();
        // Screen-probe GI reprojects its history with them.
        const bool screenProbeGINeedsVelocity = !m_frame.runPathTracer && m_rayTracingEnabled && m_diffuseGIMode == 3;
        const bool restirDINeedsVelocity = (!m_frame.runPathTracer || m_frame.pathTracerUsesRTXDI) && m_directLightingMode == 1;
        const bool restirPTNeedsVelocity = m_frame.runPathTracer && m_restirPTEnabled && m_restirPTTemporalEnabled;
        const bool pathTracerNRDNeedsVelocity = m_frame.runPathTracer &&
                                                m_nrdPTDenoiser != Nox::NRDDiffuseDenoiser::Off && nrdAvailable;
        const bool taaNeedsVelocity = isTAAActive() && !m_frame.runPathTracer;
        const bool inspectVelocity = m_textureInspection.Name == "GBuffer Velocity";
        const bool writeVelocity = dlssNeedsVelocity || taaNeedsVelocity || nrdShadowsNeedVelocity || nrdReflectionsNeedVelocity ||
                                   restirGINeedsVelocity || screenProbeGINeedsVelocity || restirDINeedsVelocity || restirPTNeedsVelocity ||
                                   pathTracerNRDNeedsVelocity || m_debugMode == 15 || inspectVelocity || m_postEffects.MotionBlur;
        m_frame.gbufferVelocityWritten = writeVelocity;

        nri::Pipeline* gbufferPipeline = writeVelocity ? (m_gbufferPipeline ? &m_gbufferPipeline : nullptr) : (m_gbufferPipelineNoVelocity ? &m_gbufferPipelineNoVelocity : nullptr);
        // While a material has a material graph: the resolve compiled with the project's graphs (CustomLitPasses.cpp; one shader for
        // every material, the graph a pixel's material names called by its slot). Without one, the plain resolve: no graph code costs.
        if (m_gpuScene.GetGraphMaterialCount() > 0 && m_gbufferGraphPipeline && m_gbufferGraphPipelineNoVelocity)
            gbufferPipeline = writeVelocity ? &m_gbufferGraphPipeline : &m_gbufferGraphPipelineNoVelocity;
        const bool resolveMaterials = gbufferPipeline && !m_drawList.empty();

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
                    nriCmdFillBuffer(context.Cmd().handle(), context.Buffer(feedback), 0, sizeof(uint32_t) * shaderio::MipFeedbackSlots, shaderio::MipFeedbackNone);
                    // No virtual texture tile requested yet (VirtualTextureFeedback::count).
                    nriCmdFillBuffer(context.Cmd().handle(), context.Buffer(feedback), sizeof(uint32_t) * shaderio::MipFeedbackSlots, sizeof(uint32_t), 0);
                });
        }

        // The render graph starts fresh each frame, so it has no record of last frame's compute-skinning writes to
        // this ring slot; make them visible to the G-Buffer's fragment shader read with its own pass (RGPassFlags::
        // None, no dynamic rendering scope) instead of a manual barrier inside G-Buffer's Execute -- vkCmdPipelineBarrier2
        // is illegal between vkCmdBeginRendering/vkCmdEndRendering without VK_KHR_dynamic_rendering_local_read/
        // VK_EXT_shader_tile_image, which this device doesn't enable (validation: "can not be called inside a
        // dynamic rendering instance", first hit once a skinned mesh -- e.g. Fox -- made skinnedHistoryValid true).
        if (resolveMaterials && m_frame.skinnedHistoryValid)
        {
            m_renderGraph.AddPass("Skinned History Barrier", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    ReadIfValid(builder, resources.PreviousSkinnedVertices);
                },
                [res = &resources](RGPassContext& context)
                {
                    if (!res->PreviousSkinnedVertices.IsValid())
                        return;
                    const nri::BufferBarrierDesc historyBarrier{
                        context.Buffer(res->PreviousSkinnedVertices),
                        { nri::AccessBits::ShaderWrite, nri::StageBits::Compute },
                        { nri::AccessBits::ShaderRead, nri::StageBits::Fragment }
                    };
                    context.Cmd().resourceBarriers({}, std::span<const nri::BufferBarrierDesc>(&historyBarrier, 1));
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
                    // The Visibility pass already wrote this pixel's exact, hardware-rasterized depth: used to reconstruct this
                    // pixel's world position directly, instead of re-deriving it from a world-space ray/triangle-plane
                    // intersection whose division becomes numerically unstable for triangles seen at a grazing angle to the
                    // view ray -- common all over a curved surface (a sphere), rare on a flat one (a cube), which is why the
                    // resolved normal looked speckled/noisy only on curved geometry (docs/Backlog_2026.md, ortho sphere entry;
                    // this turned out to affect perspective too, since the instability has nothing to do with the projection).
                    builder.Read(resources.Depth);
                    // Read and written: the shader skips the atomic when the slot already holds its mip.
                    builder.Read(resources.MipFeedback);
                    builder.Write(resources.MipFeedback);
                }
                ReadGpuScene(builder, resources);
                ReadIfValid(builder, resources.SkinnedVertices);
                if (m_frame.skinnedHistoryValid)
                    ReadIfValid(builder, resources.PreviousSkinnedVertices);
                builder.ColorTarget(resources.GBufferAlbedo, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f });   // RGBA8
                builder.ColorTarget(resources.GBufferNormal, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f });   // RGBA16F world normal
                builder.ColorTarget(resources.GBufferMaterial, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f }); // roughness, metallic, workflow
                builder.ColorTarget(resources.GBufferEmission, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f }); // RGBA16F
                builder.ColorTarget(resources.Entity, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { -1.0f, 0.0f, 0.0f, 0.0f });         // R32SINT
                builder.ColorTarget(resources.GBufferLobes, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f });    // RGBA32U lobes
                builder.ColorTarget(resources.GBufferSpecular, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f }); // RGBA8 F0, weight
                if (writeVelocity)
                    builder.ColorTarget(resources.GBufferVelocity, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f }); // RG32F
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, resolveMaterials, writeVelocity, gbufferPipeline](RGPassContext& context)
            {
                if (!resolveMaterials)
                    return;

                nri::CommandBuffer& cmd = context.Cmd();
                const uint32_t attachmentCount = writeVelocity ? 8u : 7u;
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, *gbufferPipeline);
                cmd.setCullMode(NRI_CULL_MODE_NONE);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);

                for (uint32_t a = 0; a < attachmentCount; ++a)
                {
                    cmd.setColorBlendEnable(a, false);
                    cmd.setColorWriteMask(a, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);
                }

                shaderio::PushConstantVisibilityDebug gbufferPush{};
                gbufferPush.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);

                bool hasBoneBuffers = frameIndex < m_boneBuffers.size() && static_cast<bool>(m_boneBuffers[frameIndex]);
                bool hasBones = !m_boneMatrices.empty();
                gbufferPush.boneMatrixReference = (hasBones && hasBoneBuffers) ? bufferReadRef(m_boneBuffers[frameIndex]) : 0;

                if (m_frame.computeSkinningAdded && res->SkinnedVertices.IsValid())
                    gbufferPush.skinnedVertexReference = bufferReadRef(context.Buffer(res->SkinnedVertices));
                if (m_frame.skinnedHistoryValid && res->PreviousSkinnedVertices.IsValid())
                {
                    gbufferPush.previousSkinnedVertexReference = bufferReadRef(context.Buffer(res->PreviousSkinnedVertices));
                    gbufferPush.skinnedHistoryValid = 1;
                }

                gbufferPush.visibilityTextureIndex = context.Slot(res->Visibility);
                gbufferPush.depthTextureIndex = context.Slot(res->Depth);
                gbufferPush.viewportSize = glm::vec2(rw, rh);
                gbufferPush.debugMode = m_debugMode;
                cmd.pushData(&gbufferPush, sizeof(shaderio::PushConstantVisibilityDebug));

                nri::drawMeshTasks(cmd, 1, 1, 1);
            });
    }

    void Renderer::addClusterStatsReadbackPass()
    {
        // After both visibility passes, read once this frame slot finished (Renderer::readClusterStats).
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.CameraDraws.Commands.IsValid() || !m_visibilityPipeline)
            return;

        const RGBuffer staging = m_renderGraph.ImportBuffer("Cluster Stats Staging", m_clusterStatsReadback[frameIndex]);
        m_clusterStatsPending[frameIndex] = true;
        m_renderGraph.AddPass("Cluster Stats Readback", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.ClusterStats, RGBufferAccess::CopySource);
                builder.Write(staging, RGBufferAccess::CopyDestination);
            },
            [stats = resources.ClusterStats, staging](RGPassContext& context)
            {
                { const nri::BufferCopyRegion _r = nri::BufferCopyRegion{ .size = sizeof(shaderio::ClusterStats) }; nriCmdCopyBuffer(context.Cmd().handle(), context.Buffer(stats), context.Buffer(staging), &_r); };
            });
    }

    void Renderer::addMipFeedbackReadbackPass()
    {
        // Read once this frame slot finished (Renderer::readMipFeedback); only frames that cleared it have feedback.
        if (!m_gbufferPipeline || m_drawList.empty())
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const RGBuffer staging = m_renderGraph.ImportBuffer("Mip Feedback Staging", m_mipFeedbackReadback[frameIndex]);
        m_mipFeedbackPending[frameIndex] = true;
        m_renderGraph.AddPass("Mip Feedback Readback", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.MipFeedback, RGBufferAccess::CopySource);
                builder.Write(staging, RGBufferAccess::CopyDestination);
            },
            [feedback = resources.MipFeedback, staging](RGPassContext& context)
            {
                { const nri::BufferCopyRegion _r = nri::BufferCopyRegion{ .size = MipFeedbackBytes }; nriCmdCopyBuffer(context.Cmd().handle(), context.Buffer(feedback), context.Buffer(staging), &_r); };
            });
    }

    shaderio::PushConstantSkybox Renderer::makeSkyboxSunPush() const
    {
        // The sun's disc over a procedural sky (an HDR map has its own sun in the image).
        shaderio::PushConstantSkybox skyboxSun{};
        skyboxSun.skyTransmittanceIndex = ~0u;
        skyboxSun.horizonBandIndex = ~0u;
        if (m_proceduralSkyActive && m_skyTransmittanceLut)
        {
            const SkyAtmosphereSettings& sky = m_proceduralSky;
            const float sunLength = glm::length(sky.SunDirection);
            const float radius = glm::clamp(sky.SunAngularRadius, 0.0005f, 0.2f);
            // Radiance = illuminance / the disc's solid angle (2 pi (1 - cos r)), capped: the real ratio (~15000x for the
            // sun's 0.27 degrees) only saturates the tonemapper harder, and a few pixels that bright skew auto exposure.
            const float solidAngle = 2.0f * glm::pi<float>() * (1.0f - std::cos(radius));
            skyboxSun.skyTransmittanceIndex = m_skyTransmittanceLut->GetDescriptorIndexSlot();
            skyboxSun.sunDirection = sunLength > 1e-6f ? sky.SunDirection / sunLength : glm::vec3(0.0f, 1.0f, 0.0f);
            skyboxSun.sunDiscRadiance = glm::max(sky.SunIlluminance, glm::vec3(0.0f)) * std::min(1.0f / solidAngle, 64.0f);
            skyboxSun.sunCosAngularRadius = std::cos(radius);
            skyboxSun.skyAltitudeKm = m_skyRenderedAltitudeKm; // the height the sky cubemap was rendered from

            // The moon's disc. Its light on the scene is ~1000x the real moon's (so moonlit scenes read); its disc is not, or it would
            // glare like a second sun: about the real ratio of a full moon's brightness to the ground it lights, and comparable
            // to the blue sky when it shows by day.
            const float moonLength = glm::length(sky.MoonDirection);
            const float moonRadius = glm::clamp(sky.MoonAngularRadius, 0.0005f, 0.2f);
            const float moonSolidAngle = 2.0f * glm::pi<float>() * (1.0f - std::cos(moonRadius));
            skyboxSun.moonDirection = moonLength > 1e-6f ? sky.MoonDirection / moonLength : glm::vec3(0.0f, -1.0f, 0.0f);
            skyboxSun.moonCosAngularRadius = std::cos(moonRadius);
            skyboxSun.moonDiscRadiance = glm::max(sky.MoonDiscIlluminance, glm::vec3(0.0f)) * (0.002f / moonSolidAngle);
            skyboxSun.starIntensity = glm::max(sky.StarIntensity, 0.0f);
            if (m_skyHorizonBand && sky.Clouds && sky.CloudCoverage > 0.0f && sky.CloudDensity > 0.0f)
                skyboxSun.horizonBandIndex = m_skyHorizonBand->GetDescriptorIndexSlot();
        }
        return skyboxSun;
    }

    bool Renderer::hasTransmissiveDraws() const
    {
        // Lit transparent draws whose material transmits (glass): they refract the scene behind them.
        for (RenderBucket bucket : { RenderBucket::Transparent, RenderBucket::TransparentDoubleSided })
        {
            const size_t start = m_drawBucketStarts[static_cast<size_t>(bucket)];
            const size_t end = start + getBucketEntryCount(bucket);
            for (size_t entry = start; entry < end && entry < m_drawList.size(); ++entry)
            {
                const uint32_t material = m_gpuScene.GetInstances().Get(m_drawList[entry]).materialIndex;
                if (m_gpuScene.GetMaterials().Get(material).transmissionFactor >= shaderio::SeeThroughTransmission)
                    return true;
            }
        }
        return false;
    }

    bool Renderer::hasTransparentDraws() const
    {
        return getBucketEntryCount(RenderBucket::Transparent) > 0 || getBucketEntryCount(RenderBucket::TransparentDoubleSided) > 0 ||
               getBucketEntryCount(RenderBucket::TransparentUnlit) > 0 || getBucketEntryCount(RenderBucket::TransparentUnlitDoubleSided) > 0;
    }

    void Renderer::drawForwardTransparent(nri::CommandBuffer& cmd, const RGPassContext& context, const ViewDrawResources& draws, MeshletDrawCursor& cursor, bool oit,
                                          TransparentBlend blend) const
    {
        const bool litOnly = blend != TransparentBlend::Over;
        // (Order-independent transparency: a Custom Lit surface is lit as Default Lit there -- the OIT variant has no Custom Lit build.)
        const nri::Pipeline* litPipeline = oit ? &m_transparentLitOITPipeline : forwardLitPipeline();
        const nri::Pipeline* unlitPipeline = oit ? &m_unlitOITPipeline : &m_unlitPipeline;
        if (!unlitPipeline || !litPipeline || !hasTransparentDraws())
            return;

        cursor.boundPipeline = nullptr;
        cmd.setDepthTestEnable(true);
        cmd.setDepthWriteEnable(false);
        cmd.setDepthCompareOp(NRI_COMPARE_GREATER_OR_EQUAL);
        cmd.setColorBlendEnable(0, true);
        cmd.setColorWriteMask(0, nri::ColorComponent::All);
        if (oit)
        {
            // Accumulation: summed. Revealage: multiplied by (1 - alpha). Entity IDs: the last one drawn, the nearest by the sort.
            cmd.setColorBlendEquation(0, {
                .srcColorBlendFactor = NRI_BLEND_FACTOR_ONE, .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE, .colorBlendOp = NRI_BLEND_OP_ADD,
                .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE, .alphaBlendOp = NRI_BLEND_OP_ADD });
            cmd.setColorBlendEnable(1, true);
            cmd.setColorBlendEquation(1, {
                .srcColorBlendFactor = NRI_BLEND_FACTOR_ZERO, .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, .colorBlendOp = NRI_BLEND_OP_ADD,
                .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = NRI_BLEND_OP_ADD });
            cmd.setColorWriteMask(1, nri::ColorComponent::All);
            cmd.setColorBlendEnable(2, false);
            cmd.setColorWriteMask(2, nri::ColorComponent::All);
        }
        else
        {
            nri::ColorBlendEquation blendEquation
            {
                .srcColorBlendFactor = NRI_BLEND_FACTOR_ONE,
                .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .colorBlendOp = NRI_BLEND_OP_ADD,
                .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .alphaBlendOp = NRI_BLEND_OP_ADD,
            };
            if (blend == TransparentBlend::GlassMultiply) // scene *= what the pane lets through (MESHLET_GLASS_MULTIPLY)
                blendEquation = { NRI_BLEND_FACTOR_ZERO, NRI_BLEND_FACTOR_SRC_COLOR, NRI_BLEND_OP_ADD,
                                  NRI_BLEND_FACTOR_ZERO, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD };
            else if (blend == TransparentBlend::GlassAdd) // scene += the pane's own light (MESHLET_GLASS_ADD)
                blendEquation = { NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD,
                                  NRI_BLEND_FACTOR_ZERO, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD };
            cmd.setColorBlendEquation(0, blendEquation);
            cmd.setColorBlendEnable(1, false);
            cmd.setColorWriteMask(1, nri::ColorComponent::All);
        }

        // Non-unlit Blend objects get real shading (same BRDF/IBL as the deferred opaque path, via
        // PBRLighting.slang) instead of a flat baseColor pass-through, so they don't look self-lit next to
        // shaded Opaque/Mask geometry. True KHR_materials_unlit objects still use the flat shader.
        //
        // CullMode matches each object's own doubleSided flag (mirroring the opaque/mask queues): a closed,
        // single-sided translucent shape needs its own backface culled, or both hemispheres blend on top of
        // each other and wash the result out.
        drawMeshletBucket(cmd, cursor, RenderBucket::Transparent, *litPipeline, NRI_CULL_MODE_BACK, false, true);
        drawMeshletBucket(cmd, cursor, RenderBucket::TransparentDoubleSided, *litPipeline, NRI_CULL_MODE_NONE, false, true);
        if (!litOnly) // unlit surfaces are never see-through (no transmission)
        {
            drawMeshletBucket(cmd, cursor, RenderBucket::TransparentUnlit, *unlitPipeline, NRI_CULL_MODE_BACK, false, true);
            drawMeshletBucket(cmd, cursor, RenderBucket::TransparentUnlitDoubleSided, *unlitPipeline, NRI_CULL_MODE_NONE, false, true);
        }

        MeshletDrawCursor lateCursor = beginMeshletDraws(context, draws, true);
        lateCursor.boundPipeline = cursor.boundPipeline;
        lateCursor.references.ssaoTextureIndex = cursor.references.ssaoTextureIndex;
        lateCursor.references.refractionTextureIndex = cursor.references.refractionTextureIndex;
        // The pass's flags too (refraction rays, glass only or all but glass, RR's transparency layer): the instances that came into
        // view this frame were drawn without them.
        lateCursor.taskFlags = cursor.taskFlags;
        drawMeshletBucket(cmd, lateCursor, RenderBucket::Transparent, *litPipeline, NRI_CULL_MODE_BACK, false, true);
        drawMeshletBucket(cmd, lateCursor, RenderBucket::TransparentDoubleSided, *litPipeline, NRI_CULL_MODE_NONE, false, true);
        if (!litOnly)
        {
            drawMeshletBucket(cmd, lateCursor, RenderBucket::TransparentUnlit, *unlitPipeline, NRI_CULL_MODE_BACK, false, true);
            drawMeshletBucket(cmd, lateCursor, RenderBucket::TransparentUnlitDoubleSided, *unlitPipeline, NRI_CULL_MODE_NONE, false, true);
        }
        cursor.boundPipeline = lateCursor.boundPipeline;

        cmd.setColorBlendEnable(0, false);
        if (oit)
            cmd.setColorBlendEnable(1, false);
        cmd.setDepthWriteEnable(true);
    }

    void Renderer::ensureOITTargets()
    {
        if (m_frame.oitTargetsCreated)
            return;
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        RGTextureDesc desc;
        desc.Format = NRI_FORMAT_R16G16B16A16_SFLOAT;
        resources.OITAccum = m_renderGraph.CreateTexture("OIT Accumulation", desc);
        // Revealage per color channel (what tinted glass lets through of each) and mono (a: the total coverage the resolve uses).
        resources.OITReveal = m_renderGraph.CreateTexture("OIT Revealage", desc);
        m_frame.oitTargetsCreated = true;
    }

    void Renderer::addOITCompositePass()
    {
        if (!m_frame.oitWritten || !m_oitCompositePipeline)
            return;
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_renderGraph.AddPass("OIT Composite", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.HDRScene, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                builder.Read(resources.OITAccum);
                builder.Read(resources.OITReveal);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_oitCompositePipeline);
                cmd.setCullMode(NRI_CULL_MODE_NONE);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, true);
                cmd.setColorWriteMask(0, nri::ColorComponent::All);
                shaderio::PushConstantOITComposite push{};
                push.accumIndex = uniformData.imageHeapIndexOffset + context.Slot(res->OITAccum);
                push.revealIndex = uniformData.imageHeapIndexOffset + context.Slot(res->OITReveal);
                // 1. The scene times what the layers let through of it, per channel (tinted glass tints what is behind); alpha stays.
                cmd.setColorBlendEquation(0, {
                    .srcColorBlendFactor = NRI_BLEND_FACTOR_ZERO, .dstColorBlendFactor = NRI_BLEND_FACTOR_SRC_COLOR, .colorBlendOp = NRI_BLEND_OP_ADD,
                    .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE, .alphaBlendOp = NRI_BLEND_OP_ADD });
                push.mode = shaderio::OIT_COMPOSITE_MULTIPLY;
                cmd.pushData(&push, sizeof(push));
                cmd.draw(3, 1, 0, 0);
                // 2. The layers' own light, normalized by their total coverage, added.
                cmd.setColorBlendEquation(0, {
                    .srcColorBlendFactor = NRI_BLEND_FACTOR_ONE, .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE, .colorBlendOp = NRI_BLEND_OP_ADD,
                    .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE, .alphaBlendOp = NRI_BLEND_OP_ADD });
                push.mode = shaderio::OIT_COMPOSITE_ADD;
                cmd.pushData(&push, sizeof(push));
                cmd.draw(3, 1, 0, 0);
                cmd.setColorBlendEnable(0, false);
            });
    }

    void Renderer::addParticlePass()
    {
        const bool hasCpuParticles = std::any_of(m_particleBatches.begin(), m_particleBatches.end(),
            [](const ParticleBatch& b) { return b.GpuIndirectBuffer == nullptr; });
        if (m_particleBatches.empty() || !m_particlePipeline ||
            (hasCpuParticles && (m_particleBuffers.size() <= frameIndex || !m_particleBuffers[frameIndex])) ||
            m_frame.renderExtent.width == 0 || m_frame.renderExtent.height == 0)
        {
            addOITCompositePass(); // the transparent surfaces' alone
            return;
        }
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        // Emitters far to near (each alpha-blended one is sorted inside already, SubmitParticles). With OIT the alpha-blended ones go
        // into its targets with the transparent surfaces, in any order; the additive ones are order-independent already and add
        // after the resolve.
        std::vector<ParticleBatch> batches = m_particleBatches;
        std::stable_sort(batches.begin(), batches.end(), [](const ParticleBatch& a, const ParticleBatch& b) { return a.Distance > b.Distance; });
        std::vector<ParticleBatch> motionBatches = batches;
        std::vector<ParticleBatch> oitBatches;
        if (isOITActive() && m_particleOITPipeline && m_oitCompositePipeline)
        {
            for (const ParticleBatch& batch : batches)
            {
                if (!batch.Settings.Additive)
                    oitBatches.push_back(batch);
            }
            std::erase_if(batches, [](const ParticleBatch& batch) { return !batch.Settings.Additive; });
        }
        bool lit = false;
        for (const ParticleBatch& batch : m_particleBatches)
            lit |= batch.Settings.Lit;
        const bool shadows = lit && getRayTracedShadowsActive();
        const uint64_t particles = (m_particleBuffers.size() > frameIndex && m_particleBuffers[frameIndex])
            ? bufferReadRef(m_particleBuffers[frameIndex]) : 0ull;
        const uint64_t uniforms = bufferReadRef(m_uniformBuffers[frameIndex]);

        auto declareParticles = [&](RGBuilder& builder)
        {
            builder.Read(resources.Depth); // each fragment tests and fades against it
            if (resources.AerialPerspective.IsValid())
                builder.Read(resources.AerialPerspective); // the fog in front of them (through the uniforms)
            if (lit)
            {
                if (m_frame.ddgiAdded)
                {
                    ReadDDGI(builder, resources, m_frame.ddgiWriteIndex);
                }
                // The sun's shadow maps (ShadowMaps.slang, through the uniforms).
                ReadShadowMaps(builder, resources);
                if (shadows && resources.TLAS.IsValid())
                {
                    builder.Read(resources.TLAS, RGBufferAccess::AccelerationStructureRead);
                    ReadGpuScene(builder, resources);
                }
            }
            builder.SetRenderArea(m_frame.renderExtent);
        };
        auto drawBatch = [particles, uniforms, shadows](nri::CommandBuffer& cmd, const ParticleBatch& batch, uint32_t depthSlot)
        {
            shaderio::PushConstantParticles push{};
            push.matrixReference = uniforms;
            push.particlesReference = batch.GpuIndirectBuffer ? batch.GpuRenderParticlesRef : particles;
            push.countersReference = batch.GpuIndirectBuffer ? batch.GpuCountersRef : 0ull;
            push.firstParticle = batch.First;
            push.particleCount = batch.Count;
            push.textureIndex = batch.Settings.TextureSlot;
            push.flipbookColumns = std::max(batch.Settings.FlipbookColumns, 1u);
            push.flipbookRows = std::max(batch.Settings.FlipbookRows, 1u);
            push.flags = (batch.Settings.Lit ? shaderio::PARTICLE_LIT : 0u) | (batch.Settings.Soft ? shaderio::PARTICLE_SOFT : 0u) |
                         (batch.Settings.Additive ? shaderio::PARTICLE_ADDITIVE : 0u) | (batch.Settings.Lit && shadows ? shaderio::PARTICLE_SHADOW : 0u);
            push.depthTextureIndex = depthSlot;
            push.softDistance = batch.Settings.SoftDistance;
            cmd.pushData(&push, sizeof(push));
            if (batch.GpuIndirectBuffer)
                nri::drawMeshTasksIndirect(cmd, *batch.GpuIndirectBuffer, 0, 1, 12u);
            else
                nri::drawMeshTasks(cmd, (batch.Count + 31) / 32, 1, 1);
        };

        if (!oitBatches.empty())
        {
            addAVBOITPasses(false); // (built already when transparent meshes were drawn this frame)
            ensureOITTargets();
            const bool first = !m_frame.oitWritten;
            m_frame.oitWritten = true;
            m_renderGraph.AddPass("Particles OIT", RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.ColorTarget(resources.OITAccum, first ? NRI_LOAD_OP_CLEAR : NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f });
                    builder.ColorTarget(resources.OITReveal, first ? NRI_LOAD_OP_CLEAR : NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE, { 1.0f, 1.0f, 1.0f, 1.0f });
                    declareParticles(builder);
                },
                [this, res = &resources, oitBatches = std::move(oitBatches), drawBatch](RGPassContext& context)
                {
                    nri::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_particleOITPipeline);
                    cmd.setCullMode(NRI_CULL_MODE_NONE);
                    cmd.setDepthTestEnable(false);
                    cmd.setDepthWriteEnable(false);
                    cmd.setColorBlendEnable(0, true);
                    cmd.setColorWriteMask(0, nri::ColorComponent::All);
                    cmd.setColorBlendEquation(0, {
                        .srcColorBlendFactor = NRI_BLEND_FACTOR_ONE, .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE, .colorBlendOp = NRI_BLEND_OP_ADD,
                        .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE, .alphaBlendOp = NRI_BLEND_OP_ADD });
                    cmd.setColorBlendEnable(1, true);
                    cmd.setColorWriteMask(1, nri::ColorComponent::All);
                    cmd.setColorBlendEquation(1, {
                        .srcColorBlendFactor = NRI_BLEND_FACTOR_ZERO, .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, .colorBlendOp = NRI_BLEND_OP_ADD,
                        .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = NRI_BLEND_OP_ADD });
                    const uint32_t depthSlot = context.Slot(res->Depth);
                    for (const ParticleBatch& batch : oitBatches)
                        drawBatch(cmd, batch, depthSlot);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorBlendEnable(1, false);
                });
        }

        addOITCompositePass();

        if (!batches.empty())
        {
            m_renderGraph.AddPass("Particles", RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.ColorTarget(resources.HDRScene, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                    declareParticles(builder);
                },
                [this, res = &resources, batches = std::move(batches), drawBatch](RGPassContext& context)
                {
                    nri::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_particlePipeline);
                    cmd.setCullMode(NRI_CULL_MODE_NONE);
                    cmd.setDepthTestEnable(false);
                    cmd.setDepthWriteEnable(false);
                    cmd.setColorBlendEnable(0, true);
                    cmd.setColorWriteMask(0, nri::ColorComponent::All);
                    const uint32_t depthSlot = context.Slot(res->Depth);

                    // Premultiplied colors: additive adds them and keeps what is behind; alpha covers it by their alpha.
                    const nri::ColorBlendEquation additive
                    {
                        .srcColorBlendFactor = NRI_BLEND_FACTOR_ONE,
                        .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE,
                        .colorBlendOp = NRI_BLEND_OP_ADD,
                        .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ZERO,
                        .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE,
                        .alphaBlendOp = NRI_BLEND_OP_ADD,
                    };
                    const nri::ColorBlendEquation covering
                    {
                        .srcColorBlendFactor = NRI_BLEND_FACTOR_ONE,
                        .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                        .colorBlendOp = NRI_BLEND_OP_ADD,
                        .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ONE,
                        .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                        .alphaBlendOp = NRI_BLEND_OP_ADD,
                    };
                    for (const ParticleBatch& batch : batches)
                    {
                        cmd.setColorBlendEquation(0, batch.Settings.Additive ? additive : covering);
                        drawBatch(cmd, batch, depthSlot);
                    }
                    cmd.setColorBlendEnable(0, false);
                });
        }

        // Their motion for DLSS / TAA, where they cover the pixel (Particles.slang fragMainMotion), unlit.
        const bool temporal = isTAAActive() || (m_dlssEnabled && m_dlssMode != NRI_UPSCALER_MODE_OFF && isDLSSSupported());
        if (temporal && m_particleMotionPipeline && resources.GBufferVelocity.IsValid() && m_frame.deferredLightingAdded)
        {
            m_renderGraph.AddPass("Particle Motion", RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.ColorTarget(resources.GBufferVelocity, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                    builder.Read(resources.Depth);
                    builder.SetRenderArea(m_frame.renderExtent);
                },
                [this, res = &resources, motionBatches = std::move(motionBatches), drawBatch](RGPassContext& context)
                {
                    nri::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_particleMotionPipeline);
                    cmd.setCullMode(NRI_CULL_MODE_NONE);
                    cmd.setDepthTestEnable(false);
                    cmd.setDepthWriteEnable(false);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G);
                    const uint32_t depthSlot = context.Slot(res->Depth);
                    for (ParticleBatch batch : motionBatches)
                    {
                        batch.Settings.Lit = false; // the motion needs no light
                        drawBatch(cmd, batch, depthSlot);
                    }
                });
        }
    }

    void Renderer::addForward3DPass()
    {
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        // Debug mode 17 draws the DDGI irradiance atlas written this frame on the probe spheres.
        const bool drawProbes = m_ddgiDebugSpheresPipeline && m_debugMode == 17 && m_frame.ddgiAdded;
        const bool hasDraws = resources.CameraDraws.Commands.IsValid();

        const shaderio::PushConstantSkybox skyboxSun = makeSkyboxSunPush();

        // The path tracers shade transparent surfaces in their own image: drawing them here (into the raster image nobody shows)
        // cost ~13 ms with glass and OIT on.
        const bool pathTraced = m_frame.pathTracerActive || m_frame.restirPTActive;

        // Glass in view (transmissive materials) refracts the lit scene behind it (Hybrid_Renderer_Plan_2026-10.md §4 H3b): the
        // transparent surfaces then draw in a pass of their own, after the scene so far is copied with blur mips. Without glass,
        // one pass as always.
        const bool refraction = !pathTraced && m_refractionEnabled && hasDraws && m_colorPyramidCopyPipeline && m_colorPyramidDownsamplePipeline &&
                                m_frame.renderExtent.width > 0 && m_frame.renderExtent.height > 0 && hasTransmissiveDraws();
        // Order-independent transparency: the transparent surfaces into its targets in a pass of their own (resolved after the
        // particles, addParticlePass).
        const bool oit = !pathTraced && isOITActive() && hasDraws && m_transparentLitOITPipeline && m_unlitOITPipeline && m_oitCompositePipeline &&
                         hasTransparentDraws();
        // Under DLSS Ray Reconstruction with glass in view: the glass goes into RR's input color as without RR (so RR accumulates it
        // like the rest of the image), and RR is given the image as it was before any transparent surface (NVIDIA's DLSS-RR
        // programming guide, 4.1.11 "Color Before Transparency", Streamline kBufferTypeColorBeforeTransparency): which part of a pixel
        // is the glass, which is the scene behind it its guides describe. Drawn into the guide's transparency layer instead (4.1.10),
        // RR did not accumulate it: the small bright reflections in curved glass flickered with the camera's jitter.
        const bool rrBeforeTransparency = refraction && !oit && uniformData.rayReconstructionActive != 0 && m_rrGuidesPipeline;
        m_frame.rrColorBeforeTransparency = rrBeforeTransparency;

        // Shared setup of the passes that draw over the lit scene.
        auto declareForward = [this](RGBuilder& builder, bool lit) { declareForwardInputs(builder, lit); };

        // When transparents draw inside Forward 3D (no glass refraction, no OIT), copy the opaque depth first so Scene Depth /
        // Depth Fade (step 4h) can sample it while Depth is bound as the depth target.
        if (!pathTraced && hasDraws && !refraction && !oit && hasTransparentDraws())
            addTransparentSceneDepthCopy();

        // Unlit & skybox (& transparent, without glass) geometry rendered in HDR on top of the lit scene.
        m_renderGraph.AddPass("Forward 3D", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                // Composites over deferred lighting; path-traced frames never lit the HDR scene, so it starts cleared.
                if (m_frame.deferredLightingAdded)
                    builder.ColorTarget(resources.HDRScene, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                else
                    builder.ColorTarget(resources.HDRScene, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 1.0f });
                declareForward(builder, true);
                if (resources.TransparentSceneDepth.IsValid())
                    builder.Read(resources.TransparentSceneDepth);
            },
            [this, res = &resources, drawProbes, skyboxSun, hasDraws, refraction, oit, pathTraced](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                const uint32_t totalDDGIProbes = m_frame.totalDDGIProbes;
                MeshletDrawCursor cursor = hasDraws ? beginMeshletDraws(context, res->CameraDraws) : MeshletDrawCursor{};

                // A. UNLIT MESHES
                if (hasDraws && m_unlitPipeline && (getBucketEntryCount(RenderBucket::Unlit) > 0 || getBucketEntryCount(RenderBucket::UnlitDoubleSided) > 0))
                {
                    cursor.boundPipeline = nullptr;
                    cmd.setDepthTestEnable(true);
                    cmd.setDepthWriteEnable(true);
                    cmd.setDepthCompareOp(NRI_COMPARE_GREATER_OR_EQUAL);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);
                    cmd.setColorBlendEnable(1, false);
                    cmd.setColorWriteMask(1, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);

                    drawMeshletBucket(cmd, cursor, RenderBucket::Unlit, m_unlitPipeline, NRI_CULL_MODE_BACK, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::UnlitDoubleSided, m_unlitPipeline, NRI_CULL_MODE_NONE, true, false);

                    MeshletDrawCursor lateCursor = beginMeshletDraws(context, res->CameraDraws, true);
                    lateCursor.boundPipeline = cursor.boundPipeline;
                    drawMeshletBucket(cmd, lateCursor, RenderBucket::Unlit, m_unlitPipeline, NRI_CULL_MODE_BACK, true, false);
                    drawMeshletBucket(cmd, lateCursor, RenderBucket::UnlitDoubleSided, m_unlitPipeline, NRI_CULL_MODE_NONE, true, false);
                    cursor.boundPipeline = lateCursor.boundPipeline;
                }

                // The rect lights' faces (LightShapes.slang): emissive panels, tested against the scene's depth. Not written to the
                // entity target: a click there still picks what is behind.
                if (m_lightShapesPipeline && m_rectLightCount > 0 && m_debugMode == 0)
                {
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_lightShapesPipeline);
                    cursor.boundPipeline = &m_lightShapesPipeline;
                    cmd.setCullMode(NRI_CULL_MODE_NONE);
                    cmd.setDepthTestEnable(true);
                    cmd.setDepthWriteEnable(false);
                    cmd.setDepthCompareOp(NRI_COMPARE_GREATER_OR_EQUAL);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);
                    cmd.setColorBlendEnable(1, false);
                    cmd.setColorWriteMask(1, 0u);
                    shaderio::PushConstantLightShapes lightShapesPush{};
                    lightShapesPush.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                    cmd.pushData(&lightShapesPush, sizeof(lightShapesPush));
                    nri::drawMeshTasks(cmd, 1, 1, 1);
                    cmd.setColorWriteMask(1, nri::ColorComponent::All);
                }

                // B. SKYBOX (Tested against depth == 0.0)
                if (m_skyboxPipeline && m_environmentCubemap && m_debugMode == 0)
                {
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_skyboxPipeline);
                    cursor.boundPipeline = &m_skyboxPipeline;
                    cmd.setCullMode(NRI_CULL_MODE_NONE);
                    cmd.setDepthTestEnable(true);
                    cmd.setDepthWriteEnable(false);
                    cmd.setDepthCompareOp(NRI_COMPARE_GREATER_OR_EQUAL);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);
                    cmd.setColorBlendEnable(1, false);
                    cmd.setColorWriteMask(1, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);

                    shaderio::PushConstantSkybox skyboxPush = skyboxSun;
                    skyboxPush.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                    skyboxPush.cubemapIndex = m_environmentCubemap->GetDescriptorIndexSlot();
                    cmd.pushData(&skyboxPush, sizeof(shaderio::PushConstantSkybox));
                    nri::drawMeshTasks(cmd, 1, 1, 1);
                }

                // C. TRANSPARENT FORWARD PASS (Back-to-front sorted, Alpha Blending), here unless glass needs the scene first.
                if (hasDraws && !refraction && !oit && !pathTraced)
                {
                    // Whichever of SSAO/RTAO ran this frame (never both -- see Renderer::m_ssaoEnabled), for the
                    // specular occlusion approximation in Material_TransparentLit_Mesh.slang; every other consumer
                    // of this cursor's push constant (unlit, skybox, opaque) never reads the field.
                    cursor.references.ssaoTextureIndex = m_frame.ssaoAdded ? context.Slot(res->SSAO) : 0xFFFFFFFF;
                    drawForwardTransparent(cmd, context, res->CameraDraws, cursor);
                }

                // E. WIREFRAME OVERLAY (editor): every bucket's triangles as lines, multiplied into the image, depth tested against
                // the scene (its own surfaces pass: the same vertices). Not into the entity IDs: picking keeps the surfaces.
                if (hasDraws && m_wireframeOverlay && m_wireframePipeline)
                {
                    MeshletDrawCursor wireCursor = beginMeshletDraws(context, res->CameraDraws);
                    wireCursor.references.ssaoTextureIndex = 0xFFFFFFFF;
                    cmd.setPolygonMode(NRI_POLYGON_MODE_LINE);
                    cmd.setLineWidth(1.0f);
                    cmd.setDepthTestEnable(true);
                    cmd.setDepthWriteEnable(false);
                    cmd.setDepthCompareOp(NRI_COMPARE_GREATER_OR_EQUAL);
                    cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B);
                    cmd.setColorWriteMask(1, 0u);
                    const nri::ColorBlendEquation multiply{ NRI_BLEND_FACTOR_ZERO, NRI_BLEND_FACTOR_SRC_COLOR, NRI_BLEND_OP_ADD,
                                                            NRI_BLEND_FACTOR_ZERO, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD };
                    cmd.setColorBlendEquation(0, multiply);
                    constexpr RenderBucket kWireBuckets[] = { RenderBucket::Opaque, RenderBucket::OpaqueDoubleSided, RenderBucket::Mask, RenderBucket::MaskDoubleSided,
                                                              RenderBucket::Unlit, RenderBucket::UnlitDoubleSided, RenderBucket::Transparent,
                                                              RenderBucket::TransparentDoubleSided, RenderBucket::TransparentUnlit,
                                                              RenderBucket::TransparentUnlitDoubleSided };
                    for (RenderBucket bucket : kWireBuckets)
                        drawMeshletBucket(cmd, wireCursor, bucket, m_wireframePipeline, NRI_CULL_MODE_NONE, false, true);
                    MeshletDrawCursor lateCursor = beginMeshletDraws(context, res->CameraDraws, true); // the second culling phase's draws
                    lateCursor.boundPipeline = wireCursor.boundPipeline;
                    for (RenderBucket bucket : kWireBuckets)
                        drawMeshletBucket(cmd, lateCursor, bucket, m_wireframePipeline, NRI_CULL_MODE_NONE, false, true);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setPolygonMode(NRI_POLYGON_MODE_FILL);
                    cmd.setColorWriteMask(0, nri::ColorComponent::All);
                    cmd.setColorWriteMask(1, nri::ColorComponent::All);
                    cmd.setDepthWriteEnable(true);
                }

                // D. DDGI PROBE SPHERES (Debug Mode 17: Visualizes 3D Probe Grid with Irradiance)
                if (drawProbes)
                {
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_ddgiDebugSpheresPipeline);
                    cmd.setCullMode(NRI_CULL_MODE_NONE);
                    cmd.setDepthTestEnable(!m_ddgiDebugXRay);
                    cmd.setDepthWriteEnable(false);
                    cmd.setDepthCompareOp(NRI_COMPARE_GREATER_OR_EQUAL);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);
                    cmd.setColorBlendEnable(1, false);
                    cmd.setColorWriteMask(1, nri::ColorComponent::R | nri::ColorComponent::G | nri::ColorComponent::B | nri::ColorComponent::A);

                    shaderio::PushConstantDDGIDebug debugPush{};
                    debugPush.matrixReference = bufferReadRef(m_uniformBuffers[frameIndex]);
                    debugPush.probeCountTotal = totalDDGIProbes;
                    debugPush.sphereRadius = WorldUnits::FromMeters(m_ddgiDebugSphereRadius);
                    debugPush.irradianceAtlasIndex = context.Slot(res->DDGIIrradiance[m_frame.ddgiWriteIndex]);
                    cmd.pushData(&debugPush, sizeof(shaderio::PushConstantDDGIDebug));

                    nri::drawMeshTasks(cmd, totalDDGIProbes, 1, 1);
                }
            });

        if (!refraction && !oit)
            return;

        if (oit)
        {
            addTransparentSceneDepthCopy();
            if (refraction)
            {
                addRefractionSourcePasses();
                addTransparentReflectionInputs();
            }
            // AVBOIT: the transmittance volume of every transparent surface first (meshes and alpha-blended particles).
            addAVBOITPasses(true);
            ensureOITTargets();
            m_frame.oitWritten = true;
            m_renderGraph.AddPass("Transparent OIT", RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.ColorTarget(resources.OITAccum, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f });
                    builder.ColorTarget(resources.OITReveal, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 1.0f, 1.0f, 1.0f, 1.0f });
                    declareForward(builder, true);
                    if (refraction)
                        builder.Read(resources.RefractionSource);
                    if (resources.TransparentSceneDepth.IsValid())
                        builder.Read(resources.TransparentSceneDepth);
                    if (resources.TransparentReflectionDepth.IsValid())
                    {
                        builder.Read(resources.TransparentReflectionDepth);
                        builder.Read(resources.GBufferNormal);
                        builder.Read(resources.TransparentReflectionHiZ);
                    }
                },
                [this, res = &resources, refraction](RGPassContext& context)
                {
                    nri::CommandBuffer& cmd = context.Cmd();
                    MeshletDrawCursor cursor = beginMeshletDraws(context, res->CameraDraws);
                    cursor.references.ssaoTextureIndex = m_frame.ssaoAdded ? context.Slot(res->SSAO) : 0xFFFFFFFF;
                    if (refraction)
                    {
                        cursor.references.refractionTextureIndex = context.Slot(res->RefractionSource);
                        if (m_rtRefractionEnabled && m_rayTracingEnabled)
                            cursor.taskFlags |= shaderio::MESHLET_RT_REFRACTION;
                    }
                    drawForwardTransparent(cmd, context, res->CameraDraws, cursor, true);
                });
            return;
        }

        // DLSS-RR's Color Before Transparency guide: the lit scene before any transparent surface, in the input color's format.
        if (rrBeforeTransparency)
        {
            m_renderGraph.AddPass("RR Color Before Transparency", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.HDRScene, RGTextureAccess::CopySource);
                    builder.Write(resources.RRColorBeforeTransparency, RGTextureAccess::CopyDestination);
                },
                [res = &resources](RGPassContext& context)
                {
                    nriTextureBlitTo(context.Texture(res->HDRScene), context.Cmd().handle(), context.Texture(res->RRColorBeforeTransparency));
                });
        }

        addTransparentSceneDepthCopy();
        addRefractionSourcePasses();
        addTransparentReflectionInputs();

        // Transparent surfaces over the scene; glass refracts the copy.
        m_renderGraph.AddPass("Forward Transparent", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.HDRScene, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                declareForward(builder, true);
                builder.Read(resources.RefractionSource);
                if (resources.TransparentSceneDepth.IsValid())
                    builder.Read(resources.TransparentSceneDepth);
                if (resources.TransparentReflectionDepth.IsValid())
                {
                    builder.Read(resources.TransparentReflectionDepth);
                    builder.Read(resources.GBufferNormal);
                    builder.Read(resources.TransparentReflectionHiZ);
                }
            },
            [this, res = &resources](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                MeshletDrawCursor cursor = beginMeshletDraws(context, res->CameraDraws);
                cursor.references.ssaoTextureIndex = m_frame.ssaoAdded ? context.Slot(res->SSAO) : 0xFFFFFFFF;
                cursor.references.refractionTextureIndex = context.Slot(res->RefractionSource);
                if (m_rtRefractionEnabled && m_rayTracingEnabled)
                    cursor.taskFlags |= shaderio::MESHLET_RT_REFRACTION;
                // Everything but the glass, back to front; then the glass in its two passes (MESHLET_GLASS_MULTIPLY / _ADD), in any
                // order: the panes behind panes all tint what is behind them, and every pane keeps its reflection. Drawn over the copy
                // like the rest, each pane replaced the pixel with the copy (no glass in it) tinted once: a lantern seen through, in at
                // the front and out at the back, came out with one tint or none and only the last pane drawn.
                const uint32_t passFlags = cursor.taskFlags;
                cursor.taskFlags = passFlags | shaderio::MESHLET_SKIP_SEE_THROUGH;
                drawForwardTransparent(cmd, context, res->CameraDraws, cursor);
                cursor.taskFlags = passFlags | shaderio::MESHLET_SEE_THROUGH_ONLY | shaderio::MESHLET_GLASS_MULTIPLY;
                drawForwardTransparent(cmd, context, res->CameraDraws, cursor, false, TransparentBlend::GlassMultiply);
                cursor.taskFlags = passFlags | shaderio::MESHLET_SEE_THROUGH_ONLY | shaderio::MESHLET_GLASS_ADD;
                drawForwardTransparent(cmd, context, res->CameraDraws, cursor, false, TransparentBlend::GlassAdd);
            });
    }

    void Renderer::declareForwardInputs(RGBuilder& builder, bool lit)
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        builder.ColorTarget(resources.Entity, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
        builder.DepthTarget(resources.Depth, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
        // The fog in front of forward-shaded surfaces (HeightFog.slang sceneFogTerms, through the uniforms).
        if (resources.AerialPerspective.IsValid())
            builder.Read(resources.AerialPerspective);
        if (lit)
        {
            // Transparent lit shading samples the DDGI atlases (through the uniforms); the probe spheres show irradiance.
            if (m_frame.ddgiAdded)
            {
                ReadDDGI(builder, resources, m_frame.ddgiWriteIndex);
            }
            // ...and the screen-probe GI of the surface right behind (decals) or of a traced hit on screen (SceneGI.slang).
            if (m_frame.screenProbeGIAdded)
            {
                builder.Read(resources.ScreenProbeGI[m_frame.screenProbeGIWriteIndex]);
                builder.Read(resources.ScreenProbeGIDistance);
            }
            // Transparent lit shading's specular occlusion approximation (Material_TransparentLit_Mesh.slang).
            if (m_frame.ssaoAdded)
                builder.Read(resources.SSAO);
            // ...and the SSGI of the surface right behind (decals; through the uniforms).
            if (m_frame.ssgiAdded && resources.SSGI.IsValid() && resources.SSGIDepth.IsValid())
            {
                builder.Read(resources.SSGI);
                builder.Read(resources.SSGIDepth);
            }
            // Transparent lit shading traces its own shadow rays (ray query) against the scene, and looks up the sun's shadow maps.
            if (resources.TLAS.IsValid())
                builder.Read(resources.TLAS, RGBufferAccess::AccelerationStructureRead);
            ReadShadowMaps(builder, resources);
        }
        builder.SetRenderArea(m_frame.renderExtent);
        ReadGpuScene(builder, resources);
        ReadViewDraws(builder, resources.CameraDraws);
        ReadViewDraws(builder, resources.CameraDraws, true);
        // Transparent surfaces report the mips their textures need (texture streaming feedback): read and written.
        builder.Read(resources.MipFeedback);
        builder.Write(resources.MipFeedback);
    }

    glm::vec2 Renderer::jitterScreenOffsetUv() const
    {
        // From the two projections themselves, so the sign is whatever the jitter's convention is; uv y is down (flipped viewport).
        const glm::vec4 point(0.0f, 0.0f, -1.0f, 1.0f); // in front of the camera (view space looks down -Z)
        const glm::vec4 jittered = uniformData.proj * point;
        const glm::vec4 still = uniformData.nonJitteredProj * point;
        if (std::abs(jittered.w) <= 1e-6f || std::abs(still.w) <= 1e-6f)
            return glm::vec2(0.0f);
        const glm::vec2 ndcOffset = glm::vec2(jittered) / jittered.w - glm::vec2(still) / still.w;
        return ndcOffset * glm::vec2(0.5f, -0.5f);
    }

    void Renderer::addTransparentSceneDepthCopy()
    {
        if (m_frame.renderExtent.width == 0 || m_frame.renderExtent.height == 0)
            return;
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (resources.TransparentSceneDepth.IsValid() || !resources.Depth.IsValid())
            return;
        // The depth the transparent passes test against is their depth target: they cannot sample it while it is bound. A copy (as
        // the previous-frame snapshot's), made once the opaque surfaces are all in it, serves both Scene Depth (step 4h) and SSR.
        RGTextureDesc depthDesc;
        depthDesc.Usage = NRI_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT;
        resources.TransparentSceneDepth = m_renderGraph.CreateTexture("Transparent Scene Depth", depthDesc);
        m_renderGraph.AddPass("Transparent Scene Depth", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth, RGTextureAccess::CopySource);
                builder.Write(resources.TransparentSceneDepth, RGTextureAccess::CopyDestination);
            },
            [this, res = &resources](RGPassContext& context)
            {
                nriCmdCopyTexture(context.Cmd().handle(), context.Texture(res->Depth), context.Texture(res->TransparentSceneDepth), m_frame.renderExtent.width,
                                  m_frame.renderExtent.height);
            });
    }

    void Renderer::addTransparentReflectionInputs()
    {
        // SSR stands in for the RT reflections (as for the opaque surfaces, addSSRPass): with them running, glass takes its traced
        // reflection or the probes'.
        if (!m_ssrEnabled || m_frame.rtReflectionsAdded || m_frame.renderExtent.width == 0 || m_frame.renderExtent.height == 0)
            return;
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.RefractionSource.IsValid() || !resources.GBufferNormal.IsValid() || !m_hiZBuildPipeline)
            return;
        addTransparentSceneDepthCopy();
        resources.TransparentReflectionDepth = resources.TransparentSceneDepth;
        uint32_t hizLevels = 0;
        resources.TransparentReflectionHiZ = addSSRHiZPass(resources.TransparentReflectionDepth, "Transparent Reflection Hi-Z", hizLevels);
        uniformData.transparentSSRHiZLevels = hizLevels;
        uniformData.ssrMaxSteps = m_ssrMaxSteps;
        uniformData.ssrMaxDistance = WorldUnits::FromMeters(m_ssrMaxDistanceMeters);
        uniformData.ssrThickness = WorldUnits::FromMeters(m_ssrThicknessMeters);
        uniformData.ssrMaxRoughness = m_ssrMaxRoughness;
        // (the slots: resolveFrameUniforms, once the graph is compiled)
    }

    void Renderer::addRefractionSourcePasses()
    {
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        addRefractionSourcePasses(resources.HDRScene, m_frame.renderExtent);
    }

    void Renderer::addRefractionSourcePasses(RGTexture source, nri::Extent2D extent)
    {
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        // The scene behind the glass: the HDR image so far (opaque, unlit, sky) with a mip chain of 2x2 averages (ColorPyramid.slang).
        RGTextureDesc pyramidDesc;
        pyramidDesc.Format = NRI_FORMAT_R16G16B16A16_SFLOAT;
        pyramidDesc.Usage = NRI_TEXTURE_USAGE_STORAGE;
        pyramidDesc.MipLevels = 1;
        if (extent.width != m_frame.renderExtent.width || extent.height != m_frame.renderExtent.height)
        {
            if (extent.width == m_frame.outputExtent.width && extent.height == m_frame.outputExtent.height)
                pyramidDesc.Size = RGSize::OutputResolution;
            else
            {
                pyramidDesc.Size = RGSize::Absolute;
                pyramidDesc.Width = extent.width;
                pyramidDesc.Height = extent.height;
            }
        }
        for (uint32_t size = std::max(extent.width, extent.height); size > 1; size >>= 1)
            ++pyramidDesc.MipLevels;
        resources.RefractionSource = m_renderGraph.CreateTexture("Refraction Source", pyramidDesc);
        const uint32_t mipCount = pyramidDesc.MipLevels;
        for (uint32_t mip = 0; mip < mipCount; ++mip)
        {
            m_renderGraph.AddPass(mip == 0 ? "Refraction Source" : "Refraction Source Mip", RGPassFlags::None,
                [&, mip](RGBuilder& builder)
                {
                    builder.Read(mip == 0 ? source : resources.RefractionSource);
                    builder.Write(resources.RefractionSource, RGTextureAccess::StorageWrite);
                },
                [this, res = &resources, mip, source, extent](RGPassContext& context)
                {
                    nri::CommandBuffer& cmd = context.Cmd();
                    // After DLSS (a source past it): NGX may have bound its own descriptors.
                    cmd.bindDescriptorHeaps(&m_resourceHeap, &m_samplerHeap);
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, mip == 0 ? m_colorPyramidCopyPipeline : m_colorPyramidDownsamplePipeline);

                    shaderio::PushConstantColorPyramid push{};
                    push.sourceIndex = uniformData.imageHeapIndexOffset + context.Slot(mip == 0 ? source : res->RefractionSource);
                    push.outputIndex = context.StorageSlot(res->RefractionSource, mip);
                    push.sourceMip = mip == 0 ? 0 : mip - 1;
                    push.width = std::max(extent.width >> mip, 1u);
                    push.height = std::max(extent.height >> mip, 1u);
                    cmd.pushData(&push, sizeof(push));
                    if (mip > 0)
                        MipChainSourceBarrier(cmd, context.Texture(res->RefractionSource), mip - 1, true);
                    cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                    if (mip > 0)
                        MipChainSourceBarrier(cmd, context.Texture(res->RefractionSource), mip - 1, false);
                });
        }
    }
}
