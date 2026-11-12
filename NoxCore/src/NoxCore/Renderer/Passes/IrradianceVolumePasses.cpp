// The irradiance volume (RS5a, docs/Backlog_2026.md "Raster versions of the RT features"; Diffuse GI: Raster Probe Volume): the
// diffuse GI without rays, measured against the screen-probe GI and DDGI. A grid of probes over the Irradiance Volume entity's box (or
// the whole scene's), lit at runtime the way Enlighten / Frostbite relight precomputed visibility:
//   - Capture (what a probe sees): six small faces (IrradianceProbeFaceSize) drawn as an unlit G-buffer -- diffuse albedo, emission,
//     normal, depth (Material_TransparentLit_Mesh.slang captureFragMain) -- each probe culling the draw list to the boxes within its
//     reach (four cells, at least 20 m), each face its clusters; then packed into the probe's surface cache and its distance SH (IrradianceProbeSH.slang
//     packMain). Once per probe, nearest the camera first, at most Probes per Frame a frame; again only for probes near where the
//     scene changed (instances added, removed, moved or given another material: GpuScene::TakeChangedBounds).
//   - Relight (the light it gets): every frame Relights per Frame probes in turn, each surface cache texel lit by the sun (through the
//     volume's own sun shadow map: one orthographic view over the whole box, cached like a cascade and drawn again when the sun or
//     the scene inside moves), the local lights, the sky, emission and the volume's own light (further bounces), projected onto L1
//     SH (relightMain). Time of day and moving lights cost no capture.
// The deferred lighting looks the volume up per pixel (IrradianceVolumeCommon.slang). UE's volumetric lightmap, lit at runtime.
#include "NoxCore/Renderer/Renderer.h"

#include <algorithm>
#include <numeric>

#include "FrameGraphResources.h"

namespace Nox
{
    namespace
    {
        constexpr glm::vec3 kVolumeDirections[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        constexpr glm::vec3 kVolumeUps[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 1, 0 } };
        constexpr uint32_t MaxProbesPerAxis = 32;
        constexpr uint32_t SunShadowResolution = 2048;
        // Graph names must outlive the graph: a literal per captured probe.
        constexpr const char* kCaptureCullNames[shaderio::MaxIrradianceProbesPerFrame] = {
            "Irradiance Probe 0 Culling", "Irradiance Probe 1 Culling", "Irradiance Probe 2 Culling", "Irradiance Probe 3 Culling",
            "Irradiance Probe 4 Culling", "Irradiance Probe 5 Culling", "Irradiance Probe 6 Culling", "Irradiance Probe 7 Culling",
            "Irradiance Probe 8 Culling", "Irradiance Probe 9 Culling", "Irradiance Probe 10 Culling", "Irradiance Probe 11 Culling",
            "Irradiance Probe 12 Culling", "Irradiance Probe 13 Culling", "Irradiance Probe 14 Culling", "Irradiance Probe 15 Culling" };

        glm::mat4 VolumeFaceProjection(float nearPlane)
        {
            // The cameras' infinite reverse-Z perspective, 90 degrees, square (the pack shader's texel directions assume it).
            return glm::mat4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, nearPlane, 0.0f);
        }

        // Reverse-Z orthographic projection of a light's view box (ShadowMapPasses.cpp's): x / y within +-halfSize, view z from 0
        // (depth 1) to depthRange (depth 0).
        glm::mat4 VolumeSunProjection(float halfSize, float depthRange)
        {
            return glm::mat4(1.0f / halfSize, 0.0f, 0.0f, 0.0f,
                             0.0f, 1.0f / halfSize, 0.0f, 0.0f,
                             0.0f, 0.0f, -1.0f / depthRange, 0.0f,
                             0.0f, 0.0f, 1.0f, 1.0f);
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
        NRI::PipelineDesc capture{};
        capture.forceCompile = forceCompile;
        capture.colorFormats = { NRI::ImageFormat::R16G16B16A16_SFLOAT, NRI::ImageFormat::R16G16B16A16_SFLOAT };
        capture.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
        capture.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = "assets/shaders/Material_PBR_Mesh.slang" });
        capture.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "captureFragMain", .sourcePath = "assets/shaders/Material_TransparentLit_Mesh.slang" });
        m_irradianceCapturePipeline = m_device->createPipeline(capture, *m_shaderCompiler);

        NRI::PipelineDesc desc{};
        desc.type = NRI::PipelineType::Compute;
        desc.forceCompile = forceCompile;
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Compute, .entryPoint = "packMain", .sourcePath = "assets/shaders/IrradianceProbeSH.slang" });
        m_irradiancePackPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        desc.shaders.back().entryPoint = "relightMain";
        m_irradianceRelightPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::addIrradianceVolumePasses()
    {
        IrradianceVolumeState& state = m_irradianceVolume;
        uniformData.irradianceVolumeReference = 0;
        m_irradianceCaptureFrame.Count = 0;
        m_irradianceSunMap = RGTexture{};
        m_irradianceSunDrawn = false;
        state.Automatic = false;
        const bool pathTracing = m_pathTracingEnabled || m_debugMode == 18 || m_debugMode == 19;
        if (m_diffuseGIMode != 4 || pathTracing || !m_irradianceCapturePipeline || !m_irradiancePackPipeline || !m_irradianceRelightPipeline)
        {
            // Scene changes go untracked meanwhile: every probe is captured again (in turn) when the volume comes back.
            state.RequeueAll = true;
            state.SunValid = false;
            return;
        }

        // No Irradiance Volume entity: an automatic one over the scene's bounds (refreshed every second; snapped to whole meters so
        // small changes do not capture it again). Probes at least 2 m apart, at most 32 per axis.
        IrradianceVolumeSubmission automatic;
        if (!m_irradianceVolumeSubmission.Valid)
        {
            if (!m_sceneBoundsValid || m_sceneFrameCounter - m_sceneBoundsFrame > 60)
            {
                m_sceneBoundsValid = m_gpuScene.ComputeWorldBounds(m_sceneBoundsMin, m_sceneBoundsMax);
                m_sceneBoundsFrame = m_sceneFrameCounter;
            }
            if (!m_sceneBoundsValid)
                return;
            const double meter = static_cast<double>(WorldUnits::FromMeters(1.0f));
            const glm::dvec3 worldMin = glm::floor((m_renderOrigin + glm::dvec3(m_sceneBoundsMin)) / meter) * meter;
            const glm::dvec3 worldMax = glm::ceil((m_renderOrigin + glm::dvec3(m_sceneBoundsMax)) / meter) * meter;
            const glm::vec3 extents = glm::max(glm::vec3(0.5 * (worldMax - worldMin)), glm::vec3(WorldUnits::FromMeters(1.0f)));
            automatic.Valid = true;
            automatic.Id = ~0ull;
            automatic.World = glm::translate(glm::dmat4(1.0), 0.5 * (worldMin + worldMax));
            automatic.Extents = extents;
            // At least 2 m apart, at most 32 per axis and about 2048 in all.
            const glm::vec3 size = 2.0f * extents;
            const float volumeSpacing = std::cbrt(size.x * size.y * size.z / 2048.0f);
            automatic.Spacing = std::max({ WorldUnits::FromMeters(2.0f), std::max({ size.x, size.y, size.z }) / 31.0f, volumeSpacing });
            automatic.Intensity = 1.0f;
            automatic.Revision = state.Id == ~0ull ? state.Revision : 0u;
            state.Automatic = true;
        }
        const IrradianceVolumeSubmission& submission = m_irradianceVolumeSubmission.Valid ? m_irradianceVolumeSubmission : automatic;

        // The grid: probes spacing apart over the box (world size: the extents times the entity's scale), at most 32 per axis.
        const glm::mat4 world = ToRenderSpace(submission.World);
        const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
        const glm::vec3 cameraPosition = glm::vec3(glm::inverse(uniformData.view)[3]);
        if (state.Id != submission.Id || state.World != submission.World || state.Extents != submission.Extents ||
            state.Spacing != submission.Spacing || state.Revision != submission.Revision)
        {
            glm::uvec3 counts;
            for (int axis = 0; axis < 3; ++axis)
                counts[axis] = std::clamp(static_cast<uint32_t>(std::round(2.0f * submission.Extents[axis] * scale[axis] / submission.Spacing)) + 1u, 2u, MaxProbesPerAxis);
            const uint32_t probeCount = counts.x * counts.y * counts.z;
            // The surface cache: 8 x 8 texels a face (6 KiB a probe), 4 x 4 for the largest grids (L1 SH needs little).
            const uint32_t cacheFaceSize = probeCount <= 4096 ? 8u : 4u;
            if (!state.SH || state.ProbeCount != probeCount || state.CacheFaceSize != cacheFaceSize)
            {
                {
                    std::scoped_lock lock(m_deferredReleasesMutex);
                    if (state.SH)
                        m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(state.SH) });
                    if (state.SurfaceCache)
                        m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(state.SurfaceCache) });
                }
                state.SH = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(glm::vec4) * shaderio::IrradianceProbeSHStride * probeCount, .usage = NRI::BufferUsage::Storage });
                state.SurfaceCache = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(glm::uvec4) * 6ull * cacheFaceSize * cacheFaceSize * probeCount,
                                                                             .usage = NRI::BufferUsage::Storage });
            }
            // Moved, resized or asked to: captured again from the start (the lookup leaves each probe out until it is lit).
            state.NeedsClear = true;
            state.Id = submission.Id;
            state.World = submission.World;
            state.Extents = submission.Extents;
            state.Spacing = submission.Spacing;
            state.Revision = submission.Revision;
            state.Counts = counts;
            state.ProbeCount = probeCount;
            state.CacheFaceSize = cacheFaceSize;
            state.Cached.assign(probeCount, 0);
            state.Queued.assign(probeCount, 0);
            state.CaptureQueue.clear();
            state.QueueHead = 0;
            state.CachedCount = 0;
            state.RelightCursor = 0;
            state.RequeueAll = true;
            state.SunValid = false;
        }
        const glm::vec3 cell = 2.0f * submission.Extents / glm::vec3(state.Counts - 1u);
        auto probeLocal = [&](uint32_t probe)
        {
            const glm::uvec3 coord(probe % state.Counts.x, (probe / state.Counts.x) % state.Counts.y, probe / (state.Counts.x * state.Counts.y));
            return glm::vec3(coord) * cell - submission.Extents;
        };
        auto enqueue = [&](uint32_t probe)
        {
            if (state.Queued[probe])
                return;
            state.Queued[probe] = 1;
            state.CaptureQueue.push_back(probe);
        };

        // Every probe into the capture queue, nearest the camera first (where the viewer is fills first).
        if (state.RequeueAll)
        {
            state.RequeueAll = false;
            std::vector<uint32_t> order(state.ProbeCount);
            std::iota(order.begin(), order.end(), 0u);
            std::vector<float> distances(state.ProbeCount);
            for (uint32_t probe = 0; probe < state.ProbeCount; ++probe)
            {
                const glm::vec3 offset = glm::vec3(world * glm::vec4(probeLocal(probe), 1.0f)) - cameraPosition;
                distances[probe] = glm::dot(offset, offset);
            }
            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return distances[a] < distances[b]; });
            state.CaptureQueue.erase(state.CaptureQueue.begin(), state.CaptureQueue.begin() + static_cast<std::ptrdiff_t>(state.QueueHead));
            state.QueueHead = 0;
            for (uint32_t probe : order)
                enqueue(probe);
        }
        // What changed in the scene: the probes within reach of it captured again (a moved table, a door). A new render origin moves
        // every position but no probe's view (the cache is relative to the probe); more changes than were tracked may have changed
        // anything.
        const glm::mat4 worldToLocal = glm::inverse(world);
        const float minScale = std::max(std::min({ scale.x, scale.y, scale.z }), 1e-6f);
        if (m_frameBoundsLost)
        {
            for (uint32_t probe = 0; probe < state.ProbeCount; ++probe)
                enqueue(probe);
        }
        else
        {
            const glm::vec3 reachLocal = 2.0f * cell; // probes two cells around a change see it closely
            for (uint32_t index = 0; index < m_frameChangedBoundsStatic; ++index)
            {
                const glm::vec4& sphere = m_frameChangedBounds[index];
                const glm::vec3 local = glm::vec3(worldToLocal * glm::vec4(glm::vec3(sphere), 1.0f));
                const glm::vec3 radius = glm::vec3(sphere.w / minScale) + reachLocal;
                const glm::vec3 lo = glm::clamp(glm::ceil((local - radius + submission.Extents) / cell), glm::vec3(0.0f), glm::vec3(state.Counts - 1u));
                const glm::vec3 hi = glm::clamp(glm::floor((local + radius + submission.Extents) / cell), glm::vec3(-1.0f), glm::vec3(state.Counts - 1u));
                for (int z = static_cast<int>(lo.z); z <= static_cast<int>(hi.z); ++z)
                    for (int y = static_cast<int>(lo.y); y <= static_cast<int>(hi.y); ++y)
                        for (int x = static_cast<int>(lo.x); x <= static_cast<int>(hi.x); ++x)
                            enqueue((static_cast<uint32_t>(z) * state.Counts.y + static_cast<uint32_t>(y)) * state.Counts.x + static_cast<uint32_t>(x));
            }
        }

        if (!m_irradianceTableBuffers[frameIndex])
        {
            m_irradianceTableBuffers[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::GpuIrradianceVolume), .usage = NRI::BufferUsage::Storage });
            m_irradianceTableBuffersMapped[frameIndex] = m_irradianceTableBuffers[frameIndex]->map(0, sizeof(shaderio::GpuIrradianceVolume));
            m_irradianceProbeListBuffers[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(uint32_t) * shaderio::MaxIrradianceProbesPerFrame, .usage = NRI::BufferUsage::Storage });
            m_irradianceProbeListBuffersMapped[frameIndex] = m_irradianceProbeListBuffers[frameIndex]->map(0, sizeof(uint32_t) * shaderio::MaxIrradianceProbesPerFrame);
            m_irradianceRelightListBuffers[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(uint32_t) * shaderio::MaxIrradianceRelightsPerFrame, .usage = NRI::BufferUsage::Storage });
            m_irradianceRelightListBuffersMapped[frameIndex] = m_irradianceRelightListBuffers[frameIndex]->map(0, sizeof(uint32_t) * shaderio::MaxIrradianceRelightsPerFrame);
            for (auto& cullView : m_irradianceCullViews[frameIndex])
                cullView = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::CullView), .usage = NRI::BufferUsage::Storage });
            m_irradianceSunUniforms[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::UniformBufferObject), .usage = NRI::BufferUsage::Uniform });
            m_irradianceSunUniformsMapped[frameIndex] = m_irradianceSunUniforms[frameIndex]->map(0, sizeof(shaderio::UniformBufferObject));
            m_irradianceSunCullViews[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::CullView), .usage = NRI::BufferUsage::Storage });
        }
        shaderio::GpuIrradianceVolume& volume = m_irradianceTable;
        volume = {};
        volume.localToWorld = world;
        volume.worldToLocal = worldToLocal;
        volume.extents = glm::vec4(submission.Extents, submission.Intensity);
        volume.countX = state.Counts.x;
        volume.countY = state.Counts.y;
        volume.countZ = state.Counts.z;
        volume.probeCount = state.ProbeCount;
        volume.probeSHReference = state.SH->getDeviceAddress();
        volume.surfaceCacheReference = state.SurfaceCache->getDeviceAddress();
        volume.cacheFaceSize = state.CacheFaceSize;
        volume.sunShadowTextureIndex = 0xFFFFFFFF;
        // A quarter of a cell along the normal (in local units).
        volume.normalBias = 0.25f * std::min({ cell.x, cell.y, cell.z });
        // The table's contents are final after Compile (the sun map's slot): updateIrradianceVolumeUniforms.
        const uint64_t tableAddress = m_irradianceTableBuffers[frameIndex]->getDeviceAddress();
        uniformData.irradianceVolumeReference = tableAddress;

        // A cleared volume (zero SH: nothing lit) before its first probes are.
        NRI::Buffer* shBuffer = state.SH.get();
        if (state.NeedsClear)
        {
            state.NeedsClear = false;
            const uint64_t bytes = sizeof(glm::vec4) * shaderio::IrradianceProbeSHStride * state.ProbeCount;
            m_renderGraph.AddPass("Irradiance Volume Clear", RGPassFlags::NeverCull,
                [](RGBuilder&) {},
                [shBuffer, bytes](RGPassContext& context)
                {
                    context.Cmd().fillBuffer(*shBuffer, 0, bytes, 0);
                    context.Cmd().executionBarrier();
                });
        }

        const uint32_t entryCount = static_cast<uint32_t>(m_drawList.size());
        const bool drawReady = entryCount > 0 && m_shadowDepthPipeline && m_shadowDepthMaskPipeline &&
            std::all_of(m_instanceCullingPipelines.begin(), m_instanceCullingPipelines.end(), [](const auto& pipeline) { return pipeline != nullptr; });
        if (!drawReady)
            return;
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const uint32_t blockCount = (entryCount + shaderio::CULL_BLOCK_SIZE - 1) / shaderio::CULL_BLOCK_SIZE;

        // --- The volume's sun shadow map: one orthographic view over the whole box (its bounding sphere), cached as a cascade is. ---
        if (m_shadowMaskLightIndex < m_lightBufferObjects.size())
        {
            const shaderio::LightData& sun = m_lightBufferObjects[m_shadowMaskLightIndex];
            const glm::vec3 towardsSun = glm::normalize(glm::vec3(sun.direction));
            const glm::vec3 lightForward = -towardsSun;
            const glm::vec3 lightUp = std::abs(lightForward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::mat4 lightRotation = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * glm::lookAt(glm::vec3(0.0f), lightForward, lightUp);
            const glm::vec3 center(world[3]);
            const float radius = glm::length(scale * submission.Extents);
            const float casterReach = WorldUnits::FromMeters(100.0f);
            const glm::vec3 centerInLight = glm::vec3(lightRotation * glm::vec4(center, 1.0f));
            const float depthRange = 2.0f * radius + casterReach;
            const glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(-centerInLight.x, -centerInLight.y, -centerInLight.z + radius + casterReach)) * lightRotation;
            const glm::mat4 projection = VolumeSunProjection(radius, depthRange);

            RGTextureDesc mapDesc;
            mapDesc.Size = RGSize::Absolute;
            mapDesc.Width = SunShadowResolution;
            mapDesc.Height = SunShadowResolution;
            mapDesc.Format = NRI::ImageFormat::None; // the device's depth format
            mapDesc.Usage = NRI::TextureUsage::DepthStencilAttachment;
            const RGTextureHistory history = m_renderGraph.GetHistoryTexture("Irradiance Volume Sun Shadow", mapDesc, 1);
            if (history.WasReset || m_frameOriginMoved) // (a new render origin: the old view is in the old render space)
                state.SunValid = false;
            const GeometryStreamer::Stats& streaming = m_geometryStreamer.GetStats();
            const uint64_t pageEvents = streaming.PageLoads + streaming.PageEvictions;
            if (std::exchange(state.SunPageEvents, pageEvents) != pageEvents || view != state.SunView || projection != state.SunProjection)
                state.SunDirty = true;
            const float halfSize = 1.0f / state.SunProjection[0][0];
            for (uint32_t index = 0; index < m_frameChangedBoundsStatic && state.SunValid && !state.SunDirty; ++index)
            {
                const glm::vec4& sphere = m_frameChangedBounds[index];
                const glm::vec3 p(state.SunView * glm::vec4(glm::vec3(sphere), 1.0f));
                state.SunDirty = std::abs(p.x) - sphere.w <= halfSize && std::abs(p.y) - sphere.w <= halfSize && p.z - sphere.w <= state.SunDepthRange;
            }
            // Drawn at once when it has nothing yet; otherwise at most every fourth frame while it keeps changing (a sun crossing the
            // sky moves it every frame; the probes' light follows within a few frames).
            const uint64_t frame = m_sceneFrameCounter;
            if (!state.SunValid || (state.SunDirty && frame - state.SunDrawnFrame >= 4))
            {
                state.SunView = view;
                state.SunProjection = projection;
                state.SunTexelWorld = 2.0f * radius / static_cast<float>(SunShadowResolution);
                state.SunDepthRange = depthRange;
                state.SunDrawnFrame = frame;
                state.SunValid = true;
                state.SunDirty = false;
                m_irradianceSunDrawn = true;
                addSunDepthPasses("Irradiance Volume Sun Culling", "Irradiance Volume Sun Shadow", history.Textures[0], SunShadowResolution,
                                  projection * view, m_irradianceSunUniforms[frameIndex].get(), m_irradianceSunCullViews[frameIndex].get(),
                                  -4.0f * state.SunTexelWorld / depthRange);
            }
            m_irradianceSunMap = history.Textures[0];
            volume.sunViewProj = state.SunProjection * state.SunView;
            volume.sunShadowResolution = SunShadowResolution;
            volume.sunTexelWorld = state.SunTexelWorld;
            volume.sunDepthRange = state.SunDepthRange;
        }
        else
        {
            state.SunValid = false;
        }

        // --- This frame's captures: the queue's next probes. ---
        uint32_t count = 0;
        uint32_t* probeList = static_cast<uint32_t*>(m_irradianceProbeListBuffersMapped[frameIndex]);
        while (count < m_irradianceProbesPerFrame && state.QueueHead < state.CaptureQueue.size())
        {
            const uint32_t probe = state.CaptureQueue[state.QueueHead++];
            state.Queued[probe] = 0;
            probeList[count] = probe;
            m_irradianceCaptureFrame.Centers[count] = glm::vec3(world * glm::vec4(probeLocal(probe), 1.0f));
            if (!state.Cached[probe])
            {
                state.Cached[probe] = 1;
                ++state.CachedCount;
            }
            ++count;
        }
        if (state.QueueHead == state.CaptureQueue.size())
        {
            state.CaptureQueue.clear();
            state.QueueHead = 0;
        }
        m_irradianceCaptureFrame.Count = count;

        if (count > 0)
        {
            for (uint32_t face = 0; face < 6 * count; ++face)
            {
                if (!m_irradianceFaceUniforms[frameIndex][face])
                {
                    m_irradianceFaceUniforms[frameIndex][face] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::UniformBufferObject), .usage = NRI::BufferUsage::Uniform });
                    m_irradianceFaceUniformsMapped[frameIndex][face] = m_irradianceFaceUniforms[frameIndex][face]->map(0, sizeof(shaderio::UniformBufferObject));
                }
            }

            // Per probe the draw list within its reach (a box around it; the culling has side planes only, in x and z): what lies
            // farther shows as sky. Each face then culls its clusters to its view (the task stage).
            // Four cells, at least 20 m: the drawn instances (most of a capture's cost) grow with its square.
            const float reach = std::max(4.0f * std::max({ cell.x * scale.x, cell.y * scale.y, cell.z * scale.z }), WorldUnits::FromMeters(20.0f));
            std::array<ViewDrawResources, shaderio::MaxIrradianceProbesPerFrame> draws{};
            for (uint32_t k = 0; k < count; ++k)
            {
                const glm::vec3 center = m_irradianceCaptureFrame.Centers[k];
                shaderio::CullView cull{};
                cull.frustum.planes[0] = glm::vec4(1.0f, 0.0f, 0.0f, reach - center.x);
                cull.frustum.planes[1] = glm::vec4(-1.0f, 0.0f, 0.0f, reach + center.x);
                cull.frustum.planes[2] = glm::vec4(0.0f, 0.0f, 1.0f, reach - center.z);
                cull.frustum.planes[3] = glm::vec4(0.0f, 0.0f, -1.0f, reach + center.z);
                std::copy(m_drawBucketStarts.begin(), m_drawBucketStarts.end(), cull.bucketStart);
                cull.entryCount = entryCount;
                cull.blockCount = blockCount;
                cull.flags = shaderio::CULL_INSTANCES;
                NRI::Buffer* cullViewBuffer = m_irradianceCullViews[frameIndex][k].get();
                memcpy(cullViewBuffer->map(0, sizeof(cull)), &cull, sizeof(cull));
                cullViewBuffer->unmap();

                ViewDrawResources& probeDraws = draws[k];
                probeDraws.VisibleInstances = m_renderGraph.CreateBuffer("Irradiance Probe Visible Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
                probeDraws.Commands = m_renderGraph.CreateBuffer("Irradiance Probe Draw Commands", { sizeof(shaderio::MeshTasksIndirectCommand) * entryCount, NRI::BufferUsage::Indirect });
                probeDraws.Counts = m_renderGraph.CreateBuffer("Irradiance Probe Draw Counts", { sizeof(shaderio::CullCounts), NRI::BufferUsage::Indirect });
                probeDraws.LateInstances = m_renderGraph.CreateBuffer("Irradiance Probe Late Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
                const RGBuffer flags = m_renderGraph.CreateBuffer("Irradiance Probe Cull Flags", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
                const RGBuffer blocks = m_renderGraph.CreateBuffer("Irradiance Probe Cull Blocks", { sizeof(shaderio::CullBlock) * blockCount, NRI::BufferUsage::Storage });
                addCullingDispatchPass(kCaptureCullNames[k], probeDraws, flags, blocks, entryCount, RGTexture{}, m_irradianceFaceUniforms[frameIndex][k * 6].get(), cullViewBuffer);
            }

            // The faces: row k holds probe k's six (+X, -X, +Y, -Y, +Z, -Z), unlit.
            constexpr uint32_t faceSize = shaderio::IrradianceProbeFaceSize;
            RGTextureDesc atlasDesc;
            atlasDesc.Size = RGSize::Absolute;
            atlasDesc.Width = 6 * faceSize;
            atlasDesc.Height = count * faceSize;
            atlasDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
            const RGTexture albedo = m_renderGraph.CreateTexture("Irradiance Probe Albedo", atlasDesc);
            const RGTexture emission = m_renderGraph.CreateTexture("Irradiance Probe Emission", atlasDesc);
            atlasDesc.Format = NRI::ImageFormat::None;
            atlasDesc.Usage = NRI::TextureUsage::DepthStencilAttachment;
            const RGTexture depth = m_renderGraph.CreateTexture("Irradiance Probe Depth", atlasDesc);
            const NRI::Extent2D atlasExtent{ 6 * faceSize, count * faceSize };
            m_renderGraph.AddPass("Irradiance Probe Capture", RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.ColorTarget(albedo, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                    builder.ColorTarget(emission, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                    builder.DepthTarget(depth, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0 }); // reverse Z: 0 is the sky
                    builder.SetRenderArea(atlasExtent);
                    ReadGpuScene(builder, resources);
                    ReadIfValid(builder, resources.SkinnedVertices);
                    for (uint32_t k = 0; k < count; ++k)
                        ReadViewDraws(builder, draws[k]);
                },
                [this, draws, count, atlasExtent](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    cmd.setDepthTestEnable(true);
                    cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                    for (uint32_t attachment = 0; attachment < 2; ++attachment)
                    {
                        cmd.setColorBlendEnable(attachment, false);
                        cmd.setColorWriteMask(attachment, NRI::ColorComponent::All);
                    }
                    for (uint32_t k = 0; k < count; ++k)
                    {
                        MeshletDrawCursor cursor = beginMeshletDraws(context, draws[k]);
                        cursor.references.ssaoTextureIndex = 0xFFFFFFFF;
                        cursor.taskFlags = (cursor.taskFlags & ~shaderio::MESHLET_COUNT_TRIANGLES) | shaderio::MESHLET_CULL_FRUSTUM;
                        for (uint32_t face = 0; face < 6; ++face)
                        {
                            const float x = static_cast<float>(face * faceSize);
                            const float y = static_cast<float>(k * faceSize);
                            const float size = static_cast<float>(faceSize);
                            cmd.setViewportWithCount({ x, y + size, size, -size }, 0.0f, 1.0f); // the graph's flipped viewport on the face
                            cmd.setScissorRect(static_cast<int32_t>(x), static_cast<int32_t>(y), faceSize, faceSize);
                            cursor.references.matrixReference = m_irradianceFaceUniforms[frameIndex][k * 6 + face]->getDeviceAddress();
                            drawMeshletBucket(cmd, cursor, RenderBucket::Opaque, *m_irradianceCapturePipeline, NRI::CullMode::Back, true, false);
                            drawMeshletBucket(cmd, cursor, RenderBucket::OpaqueDoubleSided, *m_irradianceCapturePipeline, NRI::CullMode::None, true, false);
                            drawMeshletBucket(cmd, cursor, RenderBucket::Mask, *m_irradianceCapturePipeline, NRI::CullMode::Back, true, false);
                            drawMeshletBucket(cmd, cursor, RenderBucket::MaskDoubleSided, *m_irradianceCapturePipeline, NRI::CullMode::None, true, false);
                            drawMeshletBucket(cmd, cursor, RenderBucket::Unlit, *m_irradianceCapturePipeline, NRI::CullMode::Back, true, false);
                            drawMeshletBucket(cmd, cursor, RenderBucket::UnlitDoubleSided, *m_irradianceCapturePipeline, NRI::CullMode::None, true, false);
                        }
                    }
                    cmd.setViewportWithCount({ 0.0f, static_cast<float>(atlasExtent.height), static_cast<float>(atlasExtent.width), -static_cast<float>(atlasExtent.height) }, 0.0f, 1.0f);
                    cmd.setScissorWithCount(atlasExtent);
                });

            // The faces into each probe's surface cache and distance SH (the volume's buffers are outside the graph: a full barrier).
            NRI::Buffer* probeListBuffer = m_irradianceProbeListBuffers[frameIndex].get();
            const float maxDistance = 2.0f * glm::length(glm::vec3(world * glm::vec4(cell, 0.0f))); // two cells' diagonals (world units)
            m_renderGraph.AddPass("Irradiance Probe Pack", RGPassFlags::NeverCull,
                [&](RGBuilder& builder)
                {
                    builder.Read(albedo);
                    builder.Read(emission);
                    builder.Read(depth);
                },
                [this, albedo, emission, depth, count, tableAddress, probeListBuffer, maxDistance](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    shaderio::PushConstantIrradianceProbePack push{};
                    push.probeIndicesReference = probeListBuffer->getDeviceAddress();
                    push.volumeReference = tableAddress;
                    push.albedoTextureIndex = uniformData.imageHeapIndexOffset + context.Slot(albedo);
                    push.emissionTextureIndex = uniformData.imageHeapIndexOffset + context.Slot(emission);
                    push.depthTextureIndex = uniformData.imageHeapIndexOffset + context.Slot(depth);
                    push.probesThisFrame = count;
                    push.nearPlane = WorldUnits::FromMeters(0.05f); // the faces' projection (updateIrradianceVolumeUniforms)
                    push.maxDistance = maxDistance;
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_irradiancePackPipeline);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch(count, 1, 1);
                    cmd.executionBarrier();
                });
        }

        // --- This frame's relights: the probes just captured, then the captured ones in turn. ---
        uint32_t* relightList = static_cast<uint32_t*>(m_irradianceRelightListBuffersMapped[frameIndex]);
        uint32_t relightCount = 0;
        for (uint32_t k = 0; k < count; ++k)
            relightList[relightCount++] = probeList[k];
        const uint32_t relightBudget = std::min(m_irradianceRelightsPerFrame, shaderio::MaxIrradianceRelightsPerFrame);
        for (uint32_t scanned = 0; scanned < state.ProbeCount && relightCount < relightBudget && state.CachedCount > 0; ++scanned)
        {
            const uint32_t probe = state.RelightCursor;
            state.RelightCursor = (state.RelightCursor + 1) % state.ProbeCount;
            if (state.Cached[probe])
                relightList[relightCount++] = probe;
        }
        if (relightCount == 0)
            return;
        NRI::Buffer* relightListBuffer = m_irradianceRelightListBuffers[frameIndex].get();
        const RGTexture sunMap = m_irradianceSunMap;
        m_renderGraph.AddPass("Irradiance Probe Relight", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                if (sunMap.IsValid())
                    builder.Read(sunMap);
                ReadShadowMaps(builder, resources); // the local lights' atlas (and the sun's cascades) when they run
            },
            [this, relightCount, tableAddress, relightListBuffer](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                shaderio::PushConstantIrradianceRelight push{};
                push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                push.probeIndicesReference = relightListBuffer->getDeviceAddress();
                push.volumeReference = tableAddress;
                push.probeCount = relightCount;
                push.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_irradianceRelightPipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch(relightCount, 1, 1);
                cmd.executionBarrier();
            });
    }

    void Renderer::updateIrradianceVolumeUniforms()
    {
        if (uniformData.irradianceVolumeReference == 0)
            return;
        // The table, with the sun map's slot (graph texture: known after Compile).
        m_irradianceTable.sunShadowTextureIndex = m_irradianceSunMap.IsValid() ? m_renderGraph.GetSlotOr(m_irradianceSunMap, 0xFFFFFFFF) : 0xFFFFFFFF;
        memcpy(m_irradianceTableBuffersMapped[frameIndex], &m_irradianceTable, sizeof(shaderio::GpuIrradianceVolume));

        // The sun map's view: a copy of the frame's uniforms with it (the culling, task and mesh stages; the cluster LOD stays the
        // camera's).
        if (m_irradianceSunDrawn)
        {
            const IrradianceVolumeState& state = m_irradianceVolume;
            shaderio::UniformBufferObject uniforms = uniformData;
            uniforms.view = uniforms.prevView = state.SunView;
            uniforms.proj = uniforms.nonJitteredProj = uniforms.prevProj = state.SunProjection;
            uniforms.invViewProj = glm::inverse(state.SunProjection * state.SunView);
            uniforms.frustum = uniforms.frozenFrustum = shaderio::Frustum{ state.SunProjection * state.SunView };
            uniforms.jitterOffset = glm::vec2(0.0f);
            uniforms.prevJitterOffset = glm::vec2(0.0f);
            uniforms.clusterStatsReference = 0;
            uniforms.sunShadowCascadesReference = 0;
            memcpy(m_irradianceSunUniformsMapped[frameIndex], &uniforms, sizeof(shaderio::UniformBufferObject));
        }

        // Each capture face's copy of the frame's uniforms (its view; the cluster LOD from the probe at the face's resolution).
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
                uniforms.mipFeedbackReference = 0; // the faces report no mips
                memcpy(m_irradianceFaceUniformsMapped[frameIndex][k * 6 + face], &uniforms, sizeof(shaderio::UniformBufferObject));
            }
        }
    }
}
