// Screen-space reflections (RS4, docs/Backlog_2026.md "Raster versions of the RT features"; Settings > Screen-Space Reflections): the
// raster version of the RT reflections, measured against them. UE5's / Godot's SSR: the mirror ray marched through this frame's depth
// buffer (SSR.slang), the radiance at the hit from last frame's lit scene -- kept here as a history with a blur mip chain
// (ColorPyramid.slang), written after the forward passes for the next frame -- at the mip the roughness' cone covers. The deferred
// lighting blends the environment in where the trace found nothing. The RT reflections take precedence when they run.
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Nox
{
    void Renderer::createSSRPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = { NRI::ImageFormat::R16G16B16A16_SFLOAT };
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/SSR.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = "assets/shaders/SSR.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain", .sourcePath = "assets/shaders/SSR.slang" });
        m_ssrPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::addSSRPass()
    {
        const bool pathTracing = m_pathTracingEnabled || m_debugMode == 18 || m_debugMode == 19;
        if (!m_ssrEnabled || pathTracing || !m_ssrPipeline || !m_colorPyramidCopyPipeline || !m_colorPyramidDownsamplePipeline)
        {
            m_ssrHistoryValid = false;
            return;
        }

        // The lit scene history (two slots: last frame's read here, this frame's written by addSSRHistoryPass), full mip chain.
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        RGTextureDesc historyDesc;
        historyDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        historyDesc.Usage = NRI::TextureUsage::Storage;
        historyDesc.MipLevels = 1;
        for (uint32_t size = std::max(m_frame.renderExtent.width, m_frame.renderExtent.height); size > 1; size >>= 1)
            ++historyDesc.MipLevels;
        const RGTextureHistory history = m_renderGraph.GetHistoryTexture("SSR Scene Color", historyDesc, 2);
        resources.SSRHistory[0] = history.Textures[0];
        resources.SSRHistory[1] = history.Textures[1];
        m_frame.ssrHistoryMips = historyDesc.MipLevels;
        if (history.WasReset)
            m_ssrHistoryValid = false;

        // Nothing to reflect yet (the first frame, a new size), or the RT reflections run: only the history is written this frame.
        if (!m_ssrHistoryValid || m_frame.rtReflectionsAdded)
            return;

        RGTextureDesc ssrDesc;
        ssrDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        resources.SSR = m_renderGraph.CreateTexture("SSR", ssrDesc);
        m_frame.ssrAdded = true;
        const RGTexture historyRead = resources.SSRHistory[m_ssrHistoryIndex];
        m_renderGraph.AddPass("SSR", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferMaterial);
                builder.Read(historyRead);
                builder.ColorTarget(resources.SSR, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, historyRead, mips = historyDesc.MipLevels](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_ssrPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::All);

                shaderio::PushConstantSSR push{};
                push.invViewProj = uniformData.invViewProj;
                push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                push.depthTextureIndex = context.Slot(res->Depth);
                push.normalTextureIndex = context.Slot(res->GBufferNormal);
                push.materialTextureIndex = context.Slot(res->GBufferMaterial);
                push.historyTextureIndex = context.Slot(historyRead);
                push.historyMipCount = mips;
                push.maxSteps = m_ssrMaxSteps;
                push.viewportSize = glm::vec2(static_cast<float>(m_frame.renderExtent.width), static_cast<float>(m_frame.renderExtent.height));
                push.maxDistance = WorldUnits::FromMeters(m_ssrMaxDistanceMeters);
                push.thickness = WorldUnits::FromMeters(m_ssrThicknessMeters);
                push.maxRoughness = m_ssrMaxRoughness;
                push.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                cmd.pushData(&push, sizeof(push));
                cmd.drawMeshTasks(1, 1, 1);
            });
    }

    void Renderer::addSSRHistoryPass()
    {
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.SSRHistory[0].IsValid() || m_frame.restirPTActive || m_frame.pathTracerActive || !resources.HDRScene.IsValid())
            return;

        // This frame's lit scene (opaque, sky, transparent, particles) and its 2x2-average mips into the other slot, for next frame.
        const uint32_t writeIndex = 1 - m_ssrHistoryIndex;
        const RGTexture target = resources.SSRHistory[writeIndex];
        const RGTexture source = resources.HDRScene;
        const NRI::Extent2D extent = m_frame.renderExtent;
        for (uint32_t mip = 0; mip < m_frame.ssrHistoryMips; ++mip)
        {
            m_renderGraph.AddPass(mip == 0 ? "SSR History" : "SSR History Mip", RGPassFlags::NeverCull,
                [&, mip](RGBuilder& builder)
                {
                    builder.Read(mip == 0 ? source : target);
                    builder.Write(target, RGTextureAccess::StorageWrite);
                },
                [this, mip, source, target, extent](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, mip == 0 ? *m_colorPyramidCopyPipeline : *m_colorPyramidDownsamplePipeline);
                    shaderio::PushConstantColorPyramid push{};
                    push.sourceIndex = uniformData.imageHeapIndexOffset + context.Slot(mip == 0 ? source : target);
                    push.outputIndex = context.StorageSlot(target, mip);
                    push.sourceMip = mip == 0 ? 0 : mip - 1;
                    push.width = std::max(extent.width >> mip, 1u);
                    push.height = std::max(extent.height >> mip, 1u);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                });
        }
        m_ssrHistoryIndex = writeIndex;
        m_ssrHistoryValid = true;
    }
    // --- Reflection probes (RS4b) ----------------------------------------------------------------------------------------------
    // UE5's box reflection captures / Godot's ReflectionProbe: each probe's surroundings rendered into a cubemap from its position (the
    // editor's extra-view forward pass: opaque surfaces lit in the fragment shader, unlit ones, the sky), assembled, mipped and
    // prefiltered like the sky's map (PrefilterEnv.slang). Captured when new, moved, resized or asked to (Recapture), one probe a frame;
    // the lighting (reflection_probe_radiance) box-projects into them inside their boxes, the smaller boxes first.

    namespace
    {
        constexpr const char* kProbeCullNames[6] = { "Probe Capture +X Culling", "Probe Capture -X Culling", "Probe Capture +Y Culling",
                                                     "Probe Capture -Y Culling", "Probe Capture +Z Culling", "Probe Capture -Z Culling" };
        constexpr const char* kProbeFaceNames[6] = { "Probe Capture +X", "Probe Capture -X", "Probe Capture +Y", "Probe Capture -Y",
                                                     "Probe Capture +Z", "Probe Capture -Z" };
        constexpr const char* kProbeHdrNames[6] = { "Probe Face +X", "Probe Face -X", "Probe Face +Y", "Probe Face -Y", "Probe Face +Z", "Probe Face -Z" };
        constexpr glm::vec3 kProbeDirections[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        constexpr glm::vec3 kProbeUps[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 1, 0 } };
        constexpr uint64_t ProbeKeepFrames = 120; // a capture not submitted for this long is released

        uint32_t CubeMips(uint32_t size)
        {
            return static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(size)))) + 1;
        }
    }

    void Renderer::SubmitReflectionProbe(uint64_t id, const glm::dmat4& worldTransform, const ReflectionProbeComponent& probe)
    {
        ReflectionProbeSubmission submission;
        submission.Id = id;
        submission.World = worldTransform;
        submission.Extents = glm::max(probe.Extents, glm::vec3(1e-4f));
        submission.BlendDistance = std::max(probe.BlendDistance, 0.0f);
        submission.Intensity = std::max(probe.Intensity, 0.0f);
        submission.Resolution = std::clamp(std::bit_floor(std::max(probe.Resolution, 16u)), 16u, 1024u);
        submission.Revision = probe.Revision;
        m_probeSubmissions.push_back(submission);
    }

    void Renderer::createReflectionProbePipelines(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.type = NRI::PipelineType::Compute;
        desc.forceCompile = forceCompile;
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Compute, .entryPoint = "assembleMain", .sourcePath = "assets/shaders/ReflectionProbe.slang" });
        m_probeAssemblePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        desc.shaders.back().entryPoint = "downsampleMain";
        m_probeDownsamplePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        desc.shaders.back() = { .stage = NRI::ShaderStage::Compute, .entryPoint = "compMain", .sourcePath = "assets/shaders/PrefilterEnv.slang" };
        m_probePrefilterPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::addReflectionProbePasses()
    {
        uniformData.reflectionProbesReference = 0;
        uniformData.reflectionProbeCount = 0;
        m_probeCaptureFrame.Active = false;

        // Captures no longer submitted (their probe removed, its level unloaded) are released after a while.
        const uint64_t frame = m_sceneFrameCounter;
        for (const ReflectionProbeSubmission& submission : m_probeSubmissions)
            m_probeCaptures[submission.Id].LastSubmittedFrame = frame;
        for (auto it = m_probeCaptures.begin(); it != m_probeCaptures.end();)
        {
            if (frame - it->second.LastSubmittedFrame > ProbeKeepFrames)
            {
                for (uint32_t slot : it->second.RawStorage)
                    m_resourceHeap->unregisterTexture(slot);
                for (uint32_t slot : it->second.PrefilteredStorage)
                    m_resourceHeap->unregisterTexture(slot);
                if (it->second.Raw)
                    deferAssetRelease(it->second.Raw);
                if (it->second.Prefiltered)
                    deferAssetRelease(it->second.Prefiltered);
                it = m_probeCaptures.erase(it);
            }
            else
            {
                ++it;
            }
        }

        const bool pathTracing = m_pathTracingEnabled || m_debugMode == 18 || m_debugMode == 19;
        if (!m_reflectionProbesEnabled || pathTracing || m_probeSubmissions.empty() || !m_probeAssemblePipeline || !m_probeDownsamplePipeline ||
            !m_probePrefilterPipeline)
            return;

        if (!m_probeTableBuffers[frameIndex])
        {
            m_probeTableBuffers[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::GpuReflectionProbe) * shaderio::MaxReflectionProbes, .usage = NRI::BufferUsage::Storage });
            m_probeTableBuffersMapped[frameIndex] = m_probeTableBuffers[frameIndex]->map(0, sizeof(shaderio::GpuReflectionProbe) * shaderio::MaxReflectionProbes);
            m_probeFaceBuffers[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(glm::mat4) * 6 + sizeof(glm::vec4), .usage = NRI::BufferUsage::Storage });
            m_probeFaceBuffersMapped[frameIndex] = m_probeFaceBuffers[frameIndex]->map(0, sizeof(glm::mat4) * 6 + sizeof(glm::vec4));
            for (uint32_t face = 0; face < 6; ++face)
            {
                m_probeFaceUniforms[frameIndex][face] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::UniformBufferObject), .usage = NRI::BufferUsage::Uniform });
                m_probeFaceUniformsMapped[frameIndex][face] = m_probeFaceUniforms[frameIndex][face]->map(0, sizeof(shaderio::UniformBufferObject));
                m_probeFaceCullViews[frameIndex][face] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::CullView), .usage = NRI::BufferUsage::Storage });
            }
        }

        // Which probe to capture this frame: the first one that is new, moved, resized or asked to.
        const uint32_t entryCount = static_cast<uint32_t>(m_drawList.size());
        const bool cullingReady = entryCount > 0 && m_transparentLitPipeline && m_unlitPipeline &&
            std::all_of(m_instanceCullingPipelines.begin(), m_instanceCullingPipelines.end(), [](const auto& pipeline) { return pipeline != nullptr; });
        const ReflectionProbeSubmission* toCapture = nullptr;
        for (const ReflectionProbeSubmission& submission : m_probeSubmissions)
        {
            const ReflectionProbeCapture& capture = m_probeCaptures[submission.Id];
            if (!capture.Captured || capture.Resolution != submission.Resolution || capture.CapturedWorld != submission.World ||
                capture.CapturedExtents != submission.Extents || capture.CapturedRevision != submission.Revision)
            {
                toCapture = &submission;
                break;
            }
        }
        if (toCapture && cullingReady)
        {
            ReflectionProbeCapture& capture = m_probeCaptures[toCapture->Id];
            if (capture.Resolution != toCapture->Resolution || !capture.Raw)
            {
                for (uint32_t slot : capture.RawStorage)
                    m_resourceHeap->unregisterTexture(slot);
                for (uint32_t slot : capture.PrefilteredStorage)
                    m_resourceHeap->unregisterTexture(slot);
                if (capture.Raw)
                    deferAssetRelease(capture.Raw);
                if (capture.Prefiltered)
                    deferAssetRelease(capture.Prefiltered);
                capture.Resolution = toCapture->Resolution;
                capture.Raw = createEnvironmentCube(capture.Resolution, NRI::ImageFormat::R16G16B16A16_SFLOAT);
                m_resourceHeap->registerTexture(*capture.Raw, NRI::TextureUsage::ShaderResource);
                capture.Prefiltered = createEnvironmentCube(capture.Resolution, NRI::ImageFormat::R16G16B16A16_SFLOAT);
                m_resourceHeap->registerTexture(*capture.Prefiltered, NRI::TextureUsage::ShaderResource);
                capture.RawStorage.clear();
                capture.PrefilteredStorage.clear();
                for (uint32_t mip = 0; mip < capture.Raw->getMipLevels(); ++mip)
                    capture.RawStorage.push_back(m_resourceHeap->registerStorageTextureMip(*capture.Raw, mip));
                for (uint32_t mip = 0; mip < capture.Prefiltered->getMipLevels(); ++mip)
                    capture.PrefilteredStorage.push_back(m_resourceHeap->registerStorageTextureMip(*capture.Prefiltered, mip));
            }
            capture.Captured = true; // the passes below fill it before the lighting reads it
            capture.CapturedWorld = toCapture->World;
            capture.CapturedExtents = toCapture->Extents;
            capture.CapturedRevision = toCapture->Revision;

            // The six faces: 90-degree views along the axes from the probe (its entity's position).
            const glm::vec3 center = ToRenderSpace(glm::dvec3(toCapture->World[3]));
            const float nearPlane = WorldUnits::FromMeters(0.05f);
            m_probeCaptureFrame.Active = true;
            m_probeCaptureFrame.Center = center;
            m_probeCaptureFrame.Resolution = capture.Resolution;
            glm::mat4* faceMatrices = static_cast<glm::mat4*>(m_probeFaceBuffersMapped[frameIndex]);
            for (uint32_t face = 0; face < 6; ++face)
            {
                const glm::mat4 view = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) *
                                       glm::lookAt(center, center + kProbeDirections[face], kProbeUps[face]);
                // The cameras' infinite reverse-Z perspective, 90 degrees, square.
                const glm::mat4 projection(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, nearPlane, 0.0f);
                m_probeCaptureFrame.Views[face] = view;
                m_probeCaptureFrame.Projections[face] = projection;
                faceMatrices[face] = projection * view;
            }
            *reinterpret_cast<glm::vec4*>(faceMatrices + 6) = glm::vec4(center, 1.0f);

            const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
            const uint32_t blockCount = (entryCount + shaderio::CULL_BLOCK_SIZE - 1) / shaderio::CULL_BLOCK_SIZE;
            const NRI::Extent2D faceExtent{ capture.Resolution, capture.Resolution };
            const shaderio::PushConstantSkybox skyboxSun = makeSkyboxSunPush();
            const bool ddgi = m_frame.ddgiAdded;
            const uint32_t ddgiIndex = m_frame.ddgiWriteIndex;
            std::array<RGTexture, 6> faceHdr{};
            for (uint32_t face = 0; face < 6; ++face)
            {
                NRI::Buffer* uniformBuffer = m_probeFaceUniforms[frameIndex][face].get();
                NRI::Buffer* cullViewBuffer = m_probeFaceCullViews[frameIndex][face].get();
                shaderio::CullView cull{};
                cull.frustum = shaderio::Frustum{ faceMatrices[face] };
                cull.viewProj = faceMatrices[face];
                cull.previousViewProj = faceMatrices[face];
                std::copy(m_drawBucketStarts.begin(), m_drawBucketStarts.end(), cull.bucketStart);
                cull.entryCount = entryCount;
                cull.blockCount = blockCount;
                cull.flags = m_instanceCullingEnabled ? shaderio::CULL_INSTANCES : 0u;
                memcpy(cullViewBuffer->map(0, sizeof(cull)), &cull, sizeof(cull));
                cullViewBuffer->unmap();

                ViewDrawResources draws;
                draws.VisibleInstances = m_renderGraph.CreateBuffer("Probe Visible Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
                draws.Commands = m_renderGraph.CreateBuffer("Probe Draw Commands", { sizeof(shaderio::MeshTasksIndirectCommand) * entryCount, NRI::BufferUsage::Indirect });
                draws.Counts = m_renderGraph.CreateBuffer("Probe Draw Counts", { sizeof(shaderio::CullCounts), NRI::BufferUsage::Indirect });
                draws.LateInstances = m_renderGraph.CreateBuffer("Probe Late Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
                const RGBuffer flags = m_renderGraph.CreateBuffer("Probe Cull Flags", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
                const RGBuffer blocks = m_renderGraph.CreateBuffer("Probe Cull Blocks", { sizeof(shaderio::CullBlock) * blockCount, NRI::BufferUsage::Storage });
                addCullingDispatchPass(kProbeCullNames[face], draws, flags, blocks, entryCount, RGTexture{}, uniformBuffer, cullViewBuffer);

                RGTextureDesc targetDesc;
                targetDesc.Size = RGSize::Absolute;
                targetDesc.Width = capture.Resolution;
                targetDesc.Height = capture.Resolution;
                targetDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
                const RGTexture hdr = m_renderGraph.CreateTexture(kProbeHdrNames[face], targetDesc);
                faceHdr[face] = hdr;
                targetDesc.Format = NRI::ImageFormat::R32SINT;
                const RGTexture entity = m_renderGraph.CreateTexture("Probe Face Entity IDs", targetDesc);
                targetDesc.Format = NRI::ImageFormat::None;
                targetDesc.Usage = NRI::TextureUsage::DepthStencilAttachment;
                const RGTexture depth = m_renderGraph.CreateTexture("Probe Face Depth", targetDesc);

                // As the editor's extra views draw a camera: opaque and masked surfaces lit in the fragment shader, unlit ones, the sky.
                m_renderGraph.AddPass(kProbeFaceNames[face], RGPassFlags::Raster,
                    [&](RGBuilder& builder)
                    {
                        builder.ColorTarget(hdr, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                        builder.ColorTarget(entity, NRI::LoadOP::clear, NRI::StoreOP::store, { -1.0f, 0.0f, 0.0f, 0.0f });
                        builder.DepthTarget(depth, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0 });
                        if (ddgi)
                            ReadDDGI(builder, resources, ddgiIndex);
                        if (resources.TLAS.IsValid())
                            builder.Read(resources.TLAS, RGBufferAccess::AccelerationStructureRead);
                        ReadShadowMaps(builder, resources); // the lit faces use the shadow maps the frame has
                        builder.SetRenderArea(faceExtent);
                        ReadGpuScene(builder, resources);
                        ReadViewDraws(builder, draws);
                        if (resources.MipFeedback.IsValid())
                        {
                            builder.Read(resources.MipFeedback);
                            builder.Write(resources.MipFeedback);
                        }
                    },
                    [this, draws, uniformBuffer, skyboxSun](RGPassContext& context)
                    {
                        NRI::CommandBuffer& cmd = context.Cmd();
                        const uint64_t uniforms = uniformBuffer->getDeviceAddress();
                        MeshletDrawCursor cursor = beginMeshletDraws(context, draws);
                        cursor.references.matrixReference = uniforms;
                        cursor.references.ssaoTextureIndex = 0xFFFFFFFF;
                        cursor.taskFlags &= ~shaderio::MESHLET_COUNT_TRIANGLES;
                        const uint32_t taskFlags = cursor.taskFlags;
                        cmd.setDepthTestEnable(true);
                        cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                        cmd.setColorBlendEnable(0, false);
                        cmd.setColorWriteMask(0, NRI::ColorComponent::All);
                        cmd.setColorBlendEnable(1, false);
                        cmd.setColorWriteMask(1, NRI::ColorComponent::All);

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
                            cmd.setCullMode(NRI::CullMode::None);
                            cmd.setDepthWriteEnable(false);
                            cmd.setColorBlendEnable(0, false);
                            shaderio::PushConstantSkybox skyboxPush = skyboxSun;
                            skyboxPush.matrixReference = uniforms;
                            skyboxPush.cubemapIndex = m_environmentCubemap->GetDescriptorIndexSlot();
                            cmd.pushData(&skyboxPush, sizeof(shaderio::PushConstantSkybox));
                            cmd.drawMeshTasks(1, 1, 1);
                            cmd.setDepthWriteEnable(true);
                        }
                    });
            }

            // The faces into the cubemap, its mips, then the prefiltered cubemap (roughness by mip) -- textures outside the graph: a
            // full barrier after each step and at the end (the lighting reads them later this frame).
            ReflectionProbeCapture* target = &capture;
            m_renderGraph.AddPass("Probe Capture Filter", RGPassFlags::NeverCull,
                [&](RGBuilder& builder)
                {
                    for (const RGTexture& hdr : faceHdr)
                        builder.Read(hdr);
                },
                [this, faceHdr, target](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    const uint32_t size = target->Resolution;
                    shaderio::PushConstantProbeAssemble push{};
                    push.facesReference = m_probeFaceBuffers[frameIndex]->getDeviceAddress();
                    for (uint32_t face = 0; face < 6; ++face)
                        push.faceTextureIndex[face] = uniformData.imageHeapIndexOffset + context.Slot(faceHdr[face]);
                    push.outputStorageIndex = target->RawStorage[0];
                    push.outputSize = size;
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_probeAssemblePipeline);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((size + 15) / 16, (size + 15) / 16, 6);
                    cmd.executionBarrier();

                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_probeDownsamplePipeline);
                    for (uint32_t mip = 1; mip < static_cast<uint32_t>(target->RawStorage.size()); ++mip)
                    {
                        push.outputStorageIndex = target->RawStorage[mip];
                        push.outputSize = std::max(size >> mip, 1u);
                        push.sourceTextureIndex = uniformData.imageHeapIndexOffset + target->Raw->GetDescriptorIndexSlot();
                        push.outputMip = mip;
                        cmd.pushData(&push, sizeof(push));
                        cmd.dispatch((push.outputSize + 15) / 16, (push.outputSize + 15) / 16, 6);
                        cmd.executionBarrier();
                    }

                    struct PrefilterPushConstants // PrefilterEnv.slang
                    {
                        uint32_t envTextureIndex;
                        uint32_t cubemapStorageIndex;
                        uint32_t cubemapSize;
                        float roughness;
                    };
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_probePrefilterPipeline);
                    const uint32_t mips = static_cast<uint32_t>(target->PrefilteredStorage.size());
                    for (uint32_t mip = 0; mip < mips; ++mip)
                    {
                        PrefilterPushConstants filter{};
                        filter.envTextureIndex = uniformData.imageHeapIndexOffset + target->Raw->GetDescriptorIndexSlot();
                        filter.cubemapStorageIndex = target->PrefilteredStorage[mip];
                        filter.cubemapSize = std::max(size >> mip, 1u);
                        filter.roughness = mips > 1 ? static_cast<float>(mip) / static_cast<float>(mips - 1) : 0.0f;
                        cmd.pushData(&filter, sizeof(filter));
                        cmd.dispatch((filter.cubemapSize + 15) / 16, (filter.cubemapSize + 15) / 16, 6);
                    }
                    cmd.executionBarrier();
                });
        }

        // The table: every captured probe, the smallest boxes first (the more specific one wins where boxes overlap).
        struct TableEntry
        {
            float Volume;
            shaderio::GpuReflectionProbe Probe;
        };
        std::vector<TableEntry> entries;
        for (const ReflectionProbeSubmission& submission : m_probeSubmissions)
        {
            const ReflectionProbeCapture& capture = m_probeCaptures[submission.Id];
            if (!capture.Captured || !capture.Prefiltered)
                continue;
            const glm::mat4 world = ToRenderSpace(submission.World);
            const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
            TableEntry entry{};
            entry.Volume = submission.Extents.x * submission.Extents.y * submission.Extents.z * scale.x * scale.y * scale.z;
            entry.Probe.localToWorld = world;
            entry.Probe.worldToLocal = glm::inverse(world);
            entry.Probe.extents = glm::vec4(submission.Extents, submission.Intensity);
            entry.Probe.blend = glm::vec4(submission.BlendDistance / glm::max(scale, glm::vec3(1e-6f)), static_cast<float>(CubeMips(capture.Resolution)));
            // Captured at the entity's position then (the cubemap's directions are from there).
            entry.Probe.center = glm::vec4(ToRenderSpace(glm::dvec3(capture.CapturedWorld[3])), 1.0f);
            entry.Probe.cubeTextureIndex = capture.Prefiltered->GetDescriptorIndexSlot();
            entries.push_back(entry);
        }
        std::stable_sort(entries.begin(), entries.end(), [](const TableEntry& a, const TableEntry& b) { return a.Volume < b.Volume; });
        const uint32_t count = std::min(static_cast<uint32_t>(entries.size()), shaderio::MaxReflectionProbes);
        shaderio::GpuReflectionProbe* table = static_cast<shaderio::GpuReflectionProbe*>(m_probeTableBuffersMapped[frameIndex]);
        for (uint32_t i = 0; i < count; ++i)
            table[i] = entries[i].Probe;
        if (count > 0)
        {
            uniformData.reflectionProbesReference = m_probeTableBuffers[frameIndex]->getDeviceAddress();
            uniformData.reflectionProbeCount = count;
        }
    }

    void Renderer::updateReflectionProbeUniforms()
    {
        if (!m_probeCaptureFrame.Active)
            return;
        // Each capture face's copy of the frame's uniforms, as an extra view's: its camera, no jitter or history, nothing that is the
        // main camera's pixels (screen-probe GI, the air's volume), no probes inside the capture; the shadow maps stay (declared).
        for (uint32_t face = 0; face < 6; ++face)
        {
            shaderio::UniformBufferObject uniforms = uniformData;
            const glm::mat4& view = m_probeCaptureFrame.Views[face];
            const glm::mat4& projection = m_probeCaptureFrame.Projections[face];
            uniforms.view = uniforms.prevView = uniforms.frozenView = view;
            uniforms.proj = uniforms.nonJitteredProj = uniforms.prevProj = uniforms.frozenProj = projection;
            uniforms.invViewProj = glm::inverse(projection * view);
            uniforms.cameraWorldPos = uniforms.frozenCameraWorldPos = glm::vec4(m_probeCaptureFrame.Center, 0.0f);
            uniforms.frustum = uniforms.frozenFrustum = shaderio::Frustum{ projection * view };
            uniforms.jitterOffset = glm::vec2(0.0f);
            uniforms.prevJitterOffset = glm::vec2(0.0f);
            uniforms.lodErrorThreshold = m_lodErrorPixels / static_cast<float>(std::max(m_probeCaptureFrame.Resolution, 1u));
            uniforms.lodCameraNear = WorldUnits::FromMeters(0.05f);
            uniforms.clusterStatsReference = 0;
            if (!m_frame.ddgiAdded)
                uniforms.enableDDGI = 0;
            uniforms.screenGITextureIndex = 0xFFFFFFFF;
            uniforms.screenGIDistanceTextureIndex = 0xFFFFFFFF;
            uniforms.aerialPerspectiveTextureIndex = 0xFFFFFFFF;
            uniforms.reflectionProbesReference = 0;
            uniforms.reflectionProbeCount = 0;
            uniforms.irradianceVolumeReference = 0;
            memcpy(m_probeFaceUniformsMapped[frameIndex][face], &uniforms, sizeof(shaderio::UniformBufferObject));
        }
    }
}
