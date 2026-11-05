// The irradiance volume (RS5a, docs/Backlog_2026.md "Raster versions of the RT features"; Diffuse GI: Raster Probe Volume): the
// diffuse GI without rays, measured against the screen-probe GI and DDGI. A grid of probes over the Irradiance Volume entity's box,
// a few captured each frame in turn: per probe six small faces (IrradianceProbeFaceSize) drawn as the editor's extra views draw a
// camera (opaque surfaces lit in the fragment shader with the shadow maps or RT shadow rays the frame has, unlit ones, the sky), all
// of a frame's faces in one raster pass into an atlas (a viewport per face; one culling for all, each face culling its clusters),
// then projected onto L1 spherical harmonics (IrradianceProbeSH.slang). The deferred lighting looks the volume up per pixel
// (IrradianceVolumeCommon.slang). UE's volumetric lightmap, updated at runtime instead of baked.
#include "NoxCore/Renderer/Renderer.h"

#include <algorithm>

#include "FrameGraphResources.h"

namespace Nox
{
    namespace
    {
        constexpr glm::vec3 kVolumeDirections[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        constexpr glm::vec3 kVolumeUps[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 1, 0 } };
        constexpr uint32_t MaxProbesPerAxis = 32;

        glm::mat4 VolumeFaceProjection(float nearPlane)
        {
            // The cameras' infinite reverse-Z perspective, 90 degrees, square (the SH shader's texel directions assume it).
            return glm::mat4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, nearPlane, 0.0f);
        }
    }

    void Renderer::SubmitIrradianceVolume(uint64_t id, const glm::dmat4& worldTransform, const IrradianceVolumeComponent& volume)
    {
        if (m_irradianceVolumeSubmission.Valid)
            return; // one volume: the first
        m_irradianceVolumeSubmission.Valid = true;
        m_irradianceVolumeSubmission.Id = id;
        m_irradianceVolumeSubmission.World = worldTransform;
        m_irradianceVolumeSubmission.Extents = glm::max(volume.Extents, glm::vec3(1e-3f));
        m_irradianceVolumeSubmission.Spacing = std::max(volume.ProbeSpacing, WorldUnits::FromMeters(0.1f));
        m_irradianceVolumeSubmission.Intensity = std::max(volume.Intensity, 0.0f);
        m_irradianceVolumeSubmission.Revision = volume.Revision;
    }

    void Renderer::createIrradianceVolumePipelines(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.type = NRI::PipelineType::Compute;
        desc.forceCompile = forceCompile;
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Compute, .entryPoint = "compMain", .sourcePath = "assets/shaders/IrradianceProbeSH.slang" });
        m_irradianceSHPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::addIrradianceVolumePasses()
    {
        uniformData.irradianceVolumeReference = 0;
        m_irradianceCaptureFrame.Count = 0;
        const bool pathTracing = m_pathTracingEnabled || m_debugMode == 18 || m_debugMode == 19;
        const IrradianceVolumeSubmission& submission = m_irradianceVolumeSubmission;
        if (m_diffuseGIMode != 4 || pathTracing || !submission.Valid || !m_irradianceSHPipeline)
            return;

        // The grid: probes spacing apart over the box (world size: the extents times the entity's scale), at most 32 per axis.
        IrradianceVolumeState& state = m_irradianceVolume;
        const glm::mat4 world = ToRenderSpace(submission.World);
        const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
        if (state.Id != submission.Id || state.World != submission.World || state.Extents != submission.Extents ||
            state.Spacing != submission.Spacing || state.Revision != submission.Revision)
        {
            glm::uvec3 counts;
            for (int axis = 0; axis < 3; ++axis)
                counts[axis] = std::clamp(static_cast<uint32_t>(std::round(2.0f * submission.Extents[axis] * scale[axis] / submission.Spacing)) + 1u, 2u, MaxProbesPerAxis);
            const uint32_t probeCount = counts.x * counts.y * counts.z;
            if (!state.SH || state.ProbeCount != probeCount)
            {
                if (state.SH)
                {
                    std::scoped_lock lock(m_deferredReleasesMutex);
                    m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(state.SH) });
                }
                state.SH = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(glm::vec4) * 3 * probeCount, .usage = NRI::BufferUsage::Storage });
            }
            // Moved, resized or asked to: captured again from the start (black until each probe's turn comes).
            state.NeedsClear = true;
            state.Id = submission.Id;
            state.World = submission.World;
            state.Extents = submission.Extents;
            state.Spacing = submission.Spacing;
            state.Revision = submission.Revision;
            state.Counts = counts;
            state.ProbeCount = probeCount;
            state.Cursor = 0;
            state.Captured = 0;
        }

        if (!m_irradianceTableBuffers[frameIndex])
        {
            m_irradianceTableBuffers[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::GpuIrradianceVolume), .usage = NRI::BufferUsage::Storage });
            m_irradianceTableBuffersMapped[frameIndex] = m_irradianceTableBuffers[frameIndex]->map(0, sizeof(shaderio::GpuIrradianceVolume));
            m_irradianceProbeListBuffers[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(uint32_t) * shaderio::MaxIrradianceProbesPerFrame, .usage = NRI::BufferUsage::Storage });
            m_irradianceProbeListBuffersMapped[frameIndex] = m_irradianceProbeListBuffers[frameIndex]->map(0, sizeof(uint32_t) * shaderio::MaxIrradianceProbesPerFrame);
            m_irradianceCullViews[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::CullView), .usage = NRI::BufferUsage::Storage });
        }
        shaderio::GpuIrradianceVolume volume{};
        volume.localToWorld = world;
        volume.worldToLocal = glm::inverse(world);
        volume.extents = glm::vec4(submission.Extents, submission.Intensity);
        volume.countX = state.Counts.x;
        volume.countY = state.Counts.y;
        volume.countZ = state.Counts.z;
        volume.probeCount = state.ProbeCount;
        volume.probeSHReference = state.SH->getDeviceAddress();
        // A quarter of a cell along the normal (in local units).
        volume.normalBias = 0.25f * std::min({ 2.0f * submission.Extents.x / static_cast<float>(state.Counts.x - 1),
                                                2.0f * submission.Extents.y / static_cast<float>(state.Counts.y - 1),
                                                2.0f * submission.Extents.z / static_cast<float>(state.Counts.z - 1) });
        memcpy(m_irradianceTableBuffersMapped[frameIndex], &volume, sizeof(volume));
        uniformData.irradianceVolumeReference = m_irradianceTableBuffers[frameIndex]->getDeviceAddress();

        // A cleared volume (zero SH) before its first captures land.
        NRI::Buffer* shBuffer = state.SH.get();
        if (state.NeedsClear)
        {
            state.NeedsClear = false;
            const uint64_t bytes = sizeof(glm::vec4) * 3 * state.ProbeCount;
            m_renderGraph.AddPass("Irradiance Volume Clear", RGPassFlags::NeverCull,
                [](RGBuilder&) {},
                [shBuffer, bytes](RGPassContext& context)
                {
                    context.Cmd().fillBuffer(*shBuffer, 0, bytes, 0);
                    context.Cmd().executionBarrier();
                });
        }

        const uint32_t entryCount = static_cast<uint32_t>(m_drawList.size());
        const bool drawReady = entryCount > 0 && m_transparentLitPipeline && m_unlitPipeline &&
            std::all_of(m_instanceCullingPipelines.begin(), m_instanceCullingPipelines.end(), [](const auto& pipeline) { return pipeline != nullptr; });
        if (!drawReady)
            return;

        // This frame's probes, in turn over the grid.
        const uint32_t count = std::min(m_irradianceProbesPerFrame, state.ProbeCount);
        uint32_t* probeList = static_cast<uint32_t*>(m_irradianceProbeListBuffersMapped[frameIndex]);
        const glm::vec3 cell = 2.0f * submission.Extents / glm::vec3(state.Counts - 1u);
        for (uint32_t k = 0; k < count; ++k)
        {
            const uint32_t probe = (state.Cursor + k) % state.ProbeCount;
            probeList[k] = probe;
            const glm::uvec3 coord(probe % state.Counts.x, (probe / state.Counts.x) % state.Counts.y, probe / (state.Counts.x * state.Counts.y));
            const glm::vec3 local = glm::vec3(coord) * cell - submission.Extents;
            m_irradianceCaptureFrame.Centers[k] = glm::vec3(world * glm::vec4(local, 1.0f));
        }
        state.Cursor = (state.Cursor + count) % state.ProbeCount;
        state.Captured = std::min(state.Captured + count, state.ProbeCount);
        m_irradianceCaptureFrame.Count = count;
        for (uint32_t face = 0; face < 6 * count; ++face)
        {
            if (!m_irradianceFaceUniforms[frameIndex][face])
            {
                m_irradianceFaceUniforms[frameIndex][face] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::UniformBufferObject), .usage = NRI::BufferUsage::Uniform });
                m_irradianceFaceUniformsMapped[frameIndex][face] = m_irradianceFaceUniforms[frameIndex][face]->map(0, sizeof(shaderio::UniformBufferObject));
            }
        }

        // One draw list for every face: no instance culling (each face's task stage culls the clusters out of its view).
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const uint32_t blockCount = (entryCount + shaderio::CULL_BLOCK_SIZE - 1) / shaderio::CULL_BLOCK_SIZE;
        NRI::Buffer* cullViewBuffer = m_irradianceCullViews[frameIndex].get();
        {
            shaderio::CullView cull{};
            std::copy(m_drawBucketStarts.begin(), m_drawBucketStarts.end(), cull.bucketStart);
            cull.entryCount = entryCount;
            cull.blockCount = blockCount;
            cull.flags = 0; // everything
            memcpy(cullViewBuffer->map(0, sizeof(cull)), &cull, sizeof(cull));
            cullViewBuffer->unmap();
        }
        ViewDrawResources draws;
        draws.VisibleInstances = m_renderGraph.CreateBuffer("Irradiance Probe Visible Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
        draws.Commands = m_renderGraph.CreateBuffer("Irradiance Probe Draw Commands", { sizeof(shaderio::MeshTasksIndirectCommand) * entryCount, NRI::BufferUsage::Indirect });
        draws.Counts = m_renderGraph.CreateBuffer("Irradiance Probe Draw Counts", { sizeof(shaderio::CullCounts), NRI::BufferUsage::Indirect });
        draws.LateInstances = m_renderGraph.CreateBuffer("Irradiance Probe Late Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
        const RGBuffer flags = m_renderGraph.CreateBuffer("Irradiance Probe Cull Flags", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
        const RGBuffer blocks = m_renderGraph.CreateBuffer("Irradiance Probe Cull Blocks", { sizeof(shaderio::CullBlock) * blockCount, NRI::BufferUsage::Storage });
        addCullingDispatchPass("Irradiance Probe Draws", draws, flags, blocks, entryCount, RGTexture{}, m_irradianceFaceUniforms[frameIndex][0].get(), cullViewBuffer);

        // The faces: row k holds probe k's six (+X, -X, +Y, -Y, +Z, -Z).
        constexpr uint32_t faceSize = shaderio::IrradianceProbeFaceSize;
        RGTextureDesc atlasDesc;
        atlasDesc.Size = RGSize::Absolute;
        atlasDesc.Width = 6 * faceSize;
        atlasDesc.Height = count * faceSize;
        atlasDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        const RGTexture atlas = m_renderGraph.CreateTexture("Irradiance Probe Faces", atlasDesc);
        atlasDesc.Format = NRI::ImageFormat::R32SINT;
        const RGTexture entity = m_renderGraph.CreateTexture("Irradiance Probe Entity IDs", atlasDesc);
        atlasDesc.Format = NRI::ImageFormat::None;
        atlasDesc.Usage = NRI::TextureUsage::DepthStencilAttachment;
        const RGTexture depth = m_renderGraph.CreateTexture("Irradiance Probe Depth", atlasDesc);
        const NRI::Extent2D atlasExtent{ 6 * faceSize, count * faceSize };
        const shaderio::PushConstantSkybox skyboxSun = makeSkyboxSunPush();
        const bool ddgi = m_frame.ddgiAdded;
        const uint32_t ddgiIndex = m_frame.ddgiWriteIndex;
        m_renderGraph.AddPass("Irradiance Probe Capture", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(atlas, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                builder.ColorTarget(entity, NRI::LoadOP::clear, NRI::StoreOP::store, { -1.0f, 0.0f, 0.0f, 0.0f });
                builder.DepthTarget(depth, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0 });
                builder.SetRenderArea(atlasExtent);
                if (ddgi)
                    ReadDDGI(builder, resources, ddgiIndex);
                if (resources.TLAS.IsValid())
                    builder.Read(resources.TLAS, RGBufferAccess::AccelerationStructureRead);
                ReadShadowMaps(builder, resources); // the lit faces use the shadow maps the frame has
                ReadGpuScene(builder, resources);
                ReadViewDraws(builder, draws);
                if (resources.MipFeedback.IsValid())
                {
                    builder.Read(resources.MipFeedback);
                    builder.Write(resources.MipFeedback);
                }
            },
            [this, draws, count, skyboxSun, atlasExtent](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                MeshletDrawCursor cursor = beginMeshletDraws(context, draws);
                cursor.references.ssaoTextureIndex = 0xFFFFFFFF;
                const uint32_t taskFlags = (cursor.taskFlags & ~shaderio::MESHLET_COUNT_TRIANGLES) | shaderio::MESHLET_CULL_FRUSTUM;
                cmd.setDepthTestEnable(true);
                cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                for (uint32_t attachment = 0; attachment < 2; ++attachment)
                {
                    cmd.setColorBlendEnable(attachment, false);
                    cmd.setColorWriteMask(attachment, NRI::ColorComponent::All);
                }
                for (uint32_t k = 0; k < count; ++k)
                {
                    for (uint32_t face = 0; face < 6; ++face)
                    {
                        const float x = static_cast<float>(face * faceSize);
                        const float y = static_cast<float>(k * faceSize);
                        const float size = static_cast<float>(faceSize);
                        cmd.setViewportWithCount({ x, y + size, size, -size }, 0.0f, 1.0f); // the graph's flipped viewport on the face
                        cmd.setScissorRect(static_cast<int32_t>(x), static_cast<int32_t>(y), faceSize, faceSize);
                        const uint64_t uniforms = m_irradianceFaceUniforms[frameIndex][k * 6 + face]->getDeviceAddress();
                        cursor.references.matrixReference = uniforms;
                        cursor.taskFlags = taskFlags | shaderio::MESHLET_SHADE_OPAQUE;
                        drawMeshletBucket(cmd, cursor, RenderBucket::Opaque, *m_transparentLitPipeline, NRI::CullMode::Back, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::OpaqueDoubleSided, *m_transparentLitPipeline, NRI::CullMode::None, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::Mask, *m_transparentLitPipeline, NRI::CullMode::Back, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::MaskDoubleSided, *m_transparentLitPipeline, NRI::CullMode::None, true, false);
                        cursor.taskFlags = taskFlags;
                        drawMeshletBucket(cmd, cursor, RenderBucket::Unlit, *m_unlitPipeline, NRI::CullMode::Back, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::UnlitDoubleSided, *m_unlitPipeline, NRI::CullMode::None, true, false);
                        if (m_skyboxPipeline && m_environmentCubemap)
                        {
                            cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_skyboxPipeline);
                            cursor.boundPipeline = m_skyboxPipeline.get();
                            cmd.setCullMode(NRI::CullMode::None);
                            cmd.setDepthWriteEnable(false);
                            shaderio::PushConstantSkybox skyboxPush = skyboxSun;
                            skyboxPush.matrixReference = uniforms;
                            skyboxPush.cubemapIndex = m_environmentCubemap->GetDescriptorIndexSlot();
                            cmd.pushData(&skyboxPush, sizeof(shaderio::PushConstantSkybox));
                            cmd.drawMeshTasks(1, 1, 1);
                            cmd.setDepthWriteEnable(true);
                        }
                    }
                }
                cmd.setViewportWithCount({ 0.0f, static_cast<float>(atlasExtent.height), static_cast<float>(atlasExtent.width), -static_cast<float>(atlasExtent.height) }, 0.0f, 1.0f);
                cmd.setScissorWithCount(atlasExtent);
            });

        // The faces into each probe's spherical harmonics (the volume's buffer is outside the graph: a full barrier after).
        NRI::Buffer* probeListBuffer = m_irradianceProbeListBuffers[frameIndex].get();
        m_renderGraph.AddPass("Irradiance Probe SH", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                builder.Read(atlas);
            },
            [this, atlas, count, shBuffer, probeListBuffer](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                shaderio::PushConstantIrradianceProbeSH push{};
                push.probeIndicesReference = probeListBuffer->getDeviceAddress();
                push.probeSHReference = shBuffer->getDeviceAddress();
                push.atlasTextureIndex = uniformData.imageHeapIndexOffset + context.Slot(atlas);
                push.probesThisFrame = count;
                cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_irradianceSHPipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch(count, 1, 1);
                cmd.executionBarrier();
            });
    }

    void Renderer::updateIrradianceVolumeUniforms()
    {
        // Each capture face's copy of the frame's uniforms, as a reflection probe's faces have them; the shadow maps stay (declared by
        // the capture pass). No irradiance volume inside its own capture (the forward shading does not read it anyway).
        const float nearPlane = WorldUnits::FromMeters(0.05f);
        const glm::mat4 projection = VolumeFaceProjection(nearPlane);
        for (uint32_t k = 0; k < m_irradianceCaptureFrame.Count; ++k)
        {
            const glm::vec3 center = m_irradianceCaptureFrame.Centers[k];
            for (uint32_t face = 0; face < 6; ++face)
            {
                const glm::mat4 view = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) *
                                       glm::lookAt(center, center + kVolumeDirections[face], kVolumeUps[face]);
                shaderio::UniformBufferObject uniforms = uniformData;
                uniforms.view = uniforms.prevView = uniforms.frozenView = view;
                uniforms.proj = uniforms.nonJitteredProj = uniforms.prevProj = uniforms.frozenProj = projection;
                uniforms.invViewProj = glm::inverse(projection * view);
                uniforms.cameraWorldPos = uniforms.frozenCameraWorldPos = glm::vec4(center, 0.0f);
                uniforms.frustum = uniforms.frozenFrustum = shaderio::Frustum{ projection * view };
                uniforms.jitterOffset = glm::vec2(0.0f);
                uniforms.prevJitterOffset = glm::vec2(0.0f);
                uniforms.lodErrorThreshold = m_lodErrorPixels / static_cast<float>(shaderio::IrradianceProbeFaceSize);
                uniforms.lodCameraNear = nearPlane;
                uniforms.clusterStatsReference = 0;
                if (!m_frame.ddgiAdded)
                    uniforms.enableDDGI = 0;
                uniforms.screenGITextureIndex = 0xFFFFFFFF;
                uniforms.screenGIDistanceTextureIndex = 0xFFFFFFFF;
                uniforms.aerialPerspectiveTextureIndex = 0xFFFFFFFF;
                uniforms.reflectionProbesReference = 0;
                uniforms.reflectionProbeCount = 0;
                uniforms.irradianceVolumeReference = 0;
                uniforms.volumetricFogTextureIndex = 0xFFFFFFFF; // the main camera's froxels
                memcpy(m_irradianceFaceUniformsMapped[frameIndex][k * 6 + face], &uniforms, sizeof(shaderio::UniformBufferObject));
            }
        }
    }
}
