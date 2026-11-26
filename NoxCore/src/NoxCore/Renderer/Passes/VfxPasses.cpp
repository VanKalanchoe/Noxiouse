// GPU Compute VFX Simulation (docs/VFX_And_Volumes_Plan_2026-11.md step V1):
// Persistent per-entity GPU particle pools (DeadList, ping-pong AliveLists, atomic counters, IndirectArgs) simulated by
// VfxSimulate.slang and drawn indirectly by Particles.slang in addParticlePass().
#include "NoxCore/Renderer/Renderer.h"
#include "FrameGraphResources.h"
#include "NoxCore/Asset/VolumeAsset.h"

#include <algorithm>
#include <cstring>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    void Renderer::createVfxComputePipelines(bool forceCompile)
    {
        nri::PipelineDesc desc{};
        desc.type = NRI_PIPELINE_COMPUTE;
        desc.forceCompile = forceCompile;
        desc.shaders.push_back({ NRI_SHADER_STAGE_COMPUTE, "initMain", "assets/shaders/VfxSimulate.slang" });
        m_vfxInitPipeline = m_device.createPipeline(desc, m_shaderCompiler.handle());

        desc.shaders.front().entryPoint = "spawnMain";
        m_vfxSpawnPipeline = m_device.createPipeline(desc, m_shaderCompiler.handle());

        desc.shaders.front().entryPoint = "updateMain";
        m_vfxUpdatePipeline = m_device.createPipeline(desc, m_shaderCompiler.handle());

        desc.shaders.front().entryPoint = "finalizeMain";
        m_vfxFinalizePipeline = m_device.createPipeline(desc, m_shaderCompiler.handle());
    }

    void Renderer::SubmitGpuParticleEmitter(uint64_t entityId, const ParticleBatchSettings& settings,
                                            const shaderio::GpuVfxEmitterParams& params, bool restart, int32_t sceneEntityHandle,
                                            const VfxMeshParticleOptions& meshOptions)
    {
        if (params.maxParticles == 0u)
            return;
        GpuVfxSubmission sub;
        sub.EntityId = entityId;
        sub.SceneEntityHandle = sceneEntityHandle;
        sub.Settings = settings;
        sub.Params = params;
        sub.Restart = restart;
        sub.MeshOptions = meshOptions;
        m_gpuVfxSubmissions.push_back(sub);
    }

    void Renderer::addVfxSimulatePass()
    {
        ++m_vfxFrameCounter;

        auto releasePoolRefs = [&](GpuVfxPool& pool)
        {
            if (pool.Particles) releaseBufferRefs(pool.Particles.handle());
            if (pool.RenderParticles) releaseBufferRefs(pool.RenderParticles.handle());
            if (pool.DeadList) releaseBufferRefs(pool.DeadList.handle());
            if (pool.AliveLists[0]) releaseBufferRefs(pool.AliveLists[0].handle());
            if (pool.AliveLists[1]) releaseBufferRefs(pool.AliveLists[1].handle());
            if (pool.Counters) releaseBufferRefs(pool.Counters.handle());
            if (pool.IndirectArgs) releaseBufferRefs(pool.IndirectArgs.handle());
            for (nri::Buffer& pb : pool.ParamsBuffers)
                if (pb) releaseBufferRefs(pb.handle());
        };

        // Evict pools whose entity has not been submitted for 120 frames (deleted or disabled).
        for (auto it = m_gpuVfxPools.begin(); it != m_gpuVfxPools.end(); )
        {
            if (m_vfxFrameCounter > it->second->LastUsedFrame + 120u)
            {
                releasePoolRefs(*it->second);
                it = m_gpuVfxPools.erase(it);
            }
            else
                ++it;
        }

        if (m_gpuVfxSubmissions.empty() || !m_vfxInitPipeline || !m_vfxSpawnPipeline || !m_vfxUpdatePipeline || !m_vfxFinalizePipeline)
            return;

        struct PreparedDispatch
        {
            GpuVfxPool* Pool = nullptr;
            uint32_t AliveInIdx = 0;
            uint32_t AliveOutIdx = 1;
            uint32_t SpawnCount = 0;
            bool NeedsInit = false;
            shaderio::PushConstantVfxSimulate Push{};
        };
        std::vector<PreparedDispatch> dispatches;
        dispatches.reserve(m_gpuVfxSubmissions.size());

        const glm::vec3 camera(uniformData.cameraWorldPos);
        const uint32_t framesInFlight = static_cast<uint32_t>(std::max<size_t>(m_uniformBuffers.size(), 1u));
        const uint64_t uboRef = (frameIndex < m_uniformBuffers.size() && m_uniformBuffers[frameIndex])
            ? bufferReadRef(m_uniformBuffers[frameIndex]) : 0ull;
        const bool hasSkinnedBuf = frameIndex < m_skinnedVertexBuffers.size() && m_skinnedVertexBuffers[frameIndex];
        const uint64_t skinnedBufRef = hasSkinnedBuf ? bufferReadRef(m_skinnedVertexBuffers[frameIndex]) : 0ull;
        const uint64_t staticVertBufRef = m_vertexStream.GetBuffer() ? bufferReadRef(m_vertexStream.GetBuffer()) : 0ull;
        bool anyDepthCollision = false;
        bool anySkinnedMeshSample = false;

        for (const GpuVfxSubmission& sub : m_gpuVfxSubmissions)
        {
            const uint32_t desiredCap = std::clamp(sub.Params.maxParticles, 64u, 250000u);
            auto& slot = m_gpuVfxPools[sub.EntityId];
            if (!slot || slot->Capacity != desiredCap)
            {
                if (slot)
                    releasePoolRefs(*slot);
                auto pool = std::make_unique<GpuVfxPool>();
                pool->Capacity = desiredCap;
                pool->Initialized = false;
                const auto makeDeviceBuffer = [&](uint64_t bytes, NriBufferUsage usage = NRI_BUFFER_USAGE_STORAGE_STATIC) -> nri::Buffer
                {
                    nri::BufferDesc bd{};
                    bd.size = bytes;
                    bd.usage = usage;
                    return m_device.createBuffer(bd);
                };
                pool->Particles = makeDeviceBuffer(sizeof(shaderio::GpuVfxParticle) * desiredCap);
                pool->RenderParticles = makeDeviceBuffer(sizeof(shaderio::GpuParticle) * desiredCap);
                pool->DeadList = makeDeviceBuffer(sizeof(uint32_t) * desiredCap);
                pool->AliveLists[0] = makeDeviceBuffer(sizeof(uint32_t) * desiredCap);
                pool->AliveLists[1] = makeDeviceBuffer(sizeof(uint32_t) * desiredCap);
                pool->Counters = makeDeviceBuffer(sizeof(shaderio::GpuVfxCounters));
                pool->IndirectArgs = makeDeviceBuffer(sizeof(shaderio::GpuVfxIndirectCommand), NRI_BUFFER_USAGE_INDIRECT_STATIC);
                pool->ParamsBuffers.resize(framesInFlight);
                pool->ParamsMapped.resize(framesInFlight, nullptr);
                for (uint32_t f = 0; f < framesInFlight; ++f)
                {
                    nri::BufferDesc pd{};
                    pd.size = sizeof(shaderio::GpuVfxEmitterParams);
                    pd.usage = NRI_BUFFER_USAGE_UNIFORM;
                    pool->ParamsBuffers[f] = m_device.createBuffer(pd);
                    pool->ParamsMapped[f] = pool->ParamsBuffers[f].map();
                }
                slot = std::move(pool);
            }

            GpuVfxPool& pool = *slot;
            pool.LastUsedFrame = m_vfxFrameCounter;

            shaderio::GpuVfxEmitterParams params = sub.Params;
            params.maxParticles = pool.Capacity;
            params.frameSeed = static_cast<uint32_t>(m_vfxFrameCounter * 1664525u + static_cast<uint32_t>(sub.EntityId));
            params.matrixReference = uboRef;
            params.depthTextureIndex = 0xFFFFFFFFu;
            params.normalTextureIndex = 0xFFFFFFFFu;
            params.collisionParams.w = static_cast<float>(m_frame.renderExtent.width);
            params.collisionParams2.x = static_cast<float>(m_frame.renderExtent.height);
            if ((params.moduleFlags & shaderio::VFX_MOD_DEPTH_COLLISION) != 0u)
                anyDepthCollision = true;

            // Step V2: Resolve static or compute-skinned mesh vertex stream for MeshSurface spawning.
            params.meshVertexReference = 0ull;
            params.meshVertexOffset = 0u;
            params.meshVertexCount = 0u;
            params.meshIsSkinned = 0u;
            if ((params.moduleFlags & shaderio::VFX_MOD_MESH_SURFACE) != 0u && sub.SceneEntityHandle >= 0)
            {
                for (uint32_t instSlot : m_drawList)
                {
                    const shaderio::GpuInstance& inst = m_gpuScene.GetInstances().Get(instSlot);
                    if (inst.entityID != sub.SceneEntityHandle || inst.meshIndex == GpuScene::InvalidSlot)
                        continue;
                    const shaderio::GpuMesh& mesh = m_gpuScene.GetMeshes().Get(inst.meshIndex);
                    if (mesh.vertexCount == 0u)
                        continue;
                    if (inst.skinnedVertexOffset != shaderio::NoSkinnedVertices && skinnedBufRef != 0ull)
                    {
                        params.meshVertexReference = skinnedBufRef;
                        params.meshVertexOffset = inst.skinnedVertexOffset;
                        params.meshVertexCount = mesh.vertexCount;
                        params.meshIsSkinned = 1u;
                        anySkinnedMeshSample = true;
                    }
                    else if (staticVertBufRef != 0ull && mesh.verticesOffset != 0xFFFFFFFFu)
                    {
                        params.meshVertexReference = staticVertBufRef;
                        params.meshVertexOffset = mesh.verticesOffset;
                        params.meshVertexCount = mesh.vertexCount;
                        params.meshIsSkinned = 0u;
                    }
                    params.meshModelMatrix = m_gpuScene.GetTransforms().Get(instSlot).world;
                    break;
                }
            }

            const uint32_t fIdx = frameIndex % framesInFlight;
            std::memcpy(pool.ParamsMapped[fIdx], &params, sizeof(params));

            const bool needsInit = !pool.Initialized || sub.Restart;
            pool.Initialized = true;

            const uint32_t aliveInIdx = pool.PingPong & 1u;
            const uint32_t aliveOutIdx = 1u - aliveInIdx;
            pool.PingPong = aliveOutIdx;

            PreparedDispatch disp;
            disp.Pool = &pool;
            disp.AliveInIdx = aliveInIdx;
            disp.AliveOutIdx = aliveOutIdx;
            disp.SpawnCount = params.spawnCount;
            disp.NeedsInit = needsInit;
            disp.Push.paramsReference = bufferReadRef(pool.ParamsBuffers[fIdx]);
            disp.Push.particlesReference = bufferWriteRef(pool.Particles);
            disp.Push.renderParticlesReference = bufferWriteRef(pool.RenderParticles);
            disp.Push.deadListReference = bufferWriteRef(pool.DeadList);
            disp.Push.aliveInReference = bufferWriteRef(pool.AliveLists[aliveInIdx]);
            disp.Push.aliveOutReference = bufferWriteRef(pool.AliveLists[aliveOutIdx]);
            disp.Push.countersReference = bufferWriteRef(pool.Counters);
            disp.Push.indirectArgsReference = bufferWriteRef(pool.IndirectArgs);
            dispatches.push_back(disp);

            // Route to either Mesh Particle pass (step V3b) or Sprite Particle pass (step V1).
            if (sub.MeshOptions.Enabled)
            {
                GpuVfxMeshBatch meshBatch;
                meshBatch.Settings = sub.Settings;
                meshBatch.MeshOptions = sub.MeshOptions;
                meshBatch.ParticlesRef = bufferReadRef(pool.Particles);
                meshBatch.RenderParticlesRef = bufferReadRef(pool.RenderParticles);
                meshBatch.AliveListRef = bufferReadRef(pool.AliveLists[aliveOutIdx]);
                meshBatch.CountersRef = bufferReadRef(pool.Counters);
                meshBatch.IndirectBuffer = &pool.IndirectArgs;
                m_vfxMeshBatches.push_back(meshBatch);
            }
            else
            {
                ParticleBatch batch;
                batch.Settings = sub.Settings;
                batch.First = 0u;
                batch.Count = pool.Capacity;
                batch.Distance = glm::length(glm::vec3(params.origin) - camera);
                batch.GpuRenderParticlesRef = bufferReadRef(pool.RenderParticles);
                batch.GpuCountersRef = bufferReadRef(pool.Counters);
                batch.GpuIndirectBuffer = &pool.IndirectArgs;
                m_particleBatches.push_back(batch);
            }
        }

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_renderGraph.AddPass("VFX Simulate", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                if (anyDepthCollision)
                {
                    if (resources.Depth.IsValid())
                        builder.Read(resources.Depth);
                    if (resources.GBufferNormal.IsValid())
                        builder.Read(resources.GBufferNormal);
                }
                if (anySkinnedMeshSample && resources.SkinnedVertices.IsValid())
                    builder.Read(resources.SkinnedVertices);
            },
            [this, res = &resources, anyDepthCollision, dispatches = std::move(dispatches)](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                const uint32_t depthSlot = (anyDepthCollision && res->Depth.IsValid()) ? context.Slot(res->Depth) : 0xFFFFFFFFu;
                const uint32_t normalSlot = (anyDepthCollision && res->GBufferNormal.IsValid()) ? context.Slot(res->GBufferNormal) : 0xFFFFFFFFu;
                auto computeBarrier = [&](const GpuVfxPool& pool)
                {
                    const nri::BufferBarrierDesc barriers[] = {
                        { pool.Particles.handle(),       { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead | nri::AccessBits::ShaderWrite, nri::StageBits::Compute } },
                        { pool.DeadList.handle(),        { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead | nri::AccessBits::ShaderWrite, nri::StageBits::Compute } },
                        { pool.AliveLists[0].handle(),   { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead | nri::AccessBits::ShaderWrite, nri::StageBits::Compute } },
                        { pool.AliveLists[1].handle(),   { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead | nri::AccessBits::ShaderWrite, nri::StageBits::Compute } },
                        { pool.Counters.handle(),        { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead | nri::AccessBits::ShaderWrite, nri::StageBits::Compute } },
                    };
                    cmd.resourceBarriers({}, std::span<const nri::BufferBarrierDesc>(barriers));
                };

                for (const PreparedDispatch& d : dispatches)
                {
                    const GpuVfxPool& pool = *d.Pool;
                    shaderio::PushConstantVfxSimulate push = d.Push;
                    push.depthTextureIndex = depthSlot;
                    push.normalTextureIndex = normalSlot;
                    if (d.NeedsInit)
                    {
                        cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_vfxInitPipeline);
                        cmd.pushData(&push, sizeof(push));
                        cmd.dispatch((pool.Capacity + 63u) / 64u, 1u, 1u);
                        computeBarrier(pool);
                    }
                    if (d.SpawnCount > 0u)
                    {
                        cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_vfxSpawnPipeline);
                        cmd.pushData(&push, sizeof(push));
                        cmd.dispatch((d.SpawnCount + 63u) / 64u, 1u, 1u);
                        computeBarrier(pool);
                    }
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_vfxUpdatePipeline);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((pool.Capacity + 63u) / 64u, 1u, 1u);
                    computeBarrier(pool);

                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_vfxFinalizePipeline);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch(1u, 1u, 1u);

                    const nri::BufferBarrierDesc toDraw[] = {
                        { pool.Particles.handle(),                 { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead, nri::StageBits::Mesh } },
                        { pool.AliveLists[d.AliveOutIdx].handle(), { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead, nri::StageBits::Mesh } },
                        { pool.RenderParticles.handle(),           { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead, nri::StageBits::Mesh | nri::StageBits::Fragment } },
                        { pool.Counters.handle(),                  { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead, nri::StageBits::Mesh } },
                        { pool.IndirectArgs.handle(),              { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::IndirectRead, nri::StageBits::Indirect } },
                    };
                    cmd.resourceBarriers({}, std::span<const nri::BufferBarrierDesc>(toDraw));
                }
            });
    }

    void Renderer::createVfxGraphicsPipelines(bool forceCompile)
    {
        nri::PipelineDesc ribbon{};
        ribbon.type = NRI_PIPELINE_GRAPHICS;
        ribbon.forceCompile = forceCompile;
        ribbon.colorFormats = { NRI_FORMAT_R16G16B16A16_SFLOAT };
        ribbon.shaders.push_back({ NRI_SHADER_STAGE_VERTEX, "vertMain", "assets/shaders/VfxRibbonBeam.slang" });
        ribbon.shaders.push_back({ NRI_SHADER_STAGE_FRAGMENT, "fragMain", "assets/shaders/VfxRibbonBeam.slang" });
        m_vfxRibbonPipeline = m_device.createPipeline(ribbon, m_shaderCompiler.handle());

        nri::PipelineDesc decal{};
        decal.type = NRI_PIPELINE_GRAPHICS;
        decal.forceCompile = forceCompile;
        decal.colorFormats = { NRI_FORMAT_R16G16B16A16_SFLOAT };
        decal.shaders.push_back({ NRI_SHADER_STAGE_VERTEX, "vertMain", "assets/shaders/VfxDecal.slang" });
        decal.shaders.push_back({ NRI_SHADER_STAGE_FRAGMENT, "fragMain", "assets/shaders/VfxDecal.slang" });
        m_vfxDecalPipeline = m_device.createPipeline(decal, m_shaderCompiler.handle());

        nri::PipelineDesc meshParticle{};
        meshParticle.type = NRI_PIPELINE_GRAPHICS;
        meshParticle.forceCompile = forceCompile;
        meshParticle.colorFormats = { NRI_FORMAT_R16G16B16A16_SFLOAT };
        meshParticle.shaders.push_back({ NRI_SHADER_STAGE_MESH, "meshMain", "assets/shaders/VfxMeshParticle.slang" });
        meshParticle.shaders.push_back({ NRI_SHADER_STAGE_FRAGMENT, "fragMain", "assets/shaders/VfxMeshParticle.slang" });
        m_vfxMeshParticlePipeline = m_device.createPipeline(meshParticle, m_shaderCompiler.handle());

        nri::PipelineDesc distortion{};
        distortion.type = NRI_PIPELINE_GRAPHICS;
        distortion.forceCompile = forceCompile;
        distortion.colorFormats = { NRI_FORMAT_R16G16B16A16_SFLOAT };
        distortion.shaders.push_back({ NRI_SHADER_STAGE_VERTEX, "vertMain", "assets/shaders/VfxDistortion.slang" });
        distortion.shaders.push_back({ NRI_SHADER_STAGE_FRAGMENT, "fragMain", "assets/shaders/VfxDistortion.slang" });
        m_vfxDistortionPipeline = m_device.createPipeline(distortion, m_shaderCompiler.handle());

        nri::PipelineDesc volume{};
        volume.type = NRI_PIPELINE_GRAPHICS;
        volume.forceCompile = forceCompile;
        volume.colorFormats = { NRI_FORMAT_R16G16B16A16_SFLOAT };
        volume.shaders.push_back({ NRI_SHADER_STAGE_VERTEX, "vertMain", "assets/shaders/HeterogeneousVolume.slang" });
        volume.shaders.push_back({ NRI_SHADER_STAGE_FRAGMENT, "fragMain", "assets/shaders/HeterogeneousVolume.slang" });
        m_vfxVolumePipeline = m_device.createPipeline(volume, m_shaderCompiler.handle());
    }

    void Renderer::SubmitVfxRibbon(const VfxRibbonSettings& settings, std::span<const shaderio::GpuVfxRibbonNode> nodes)
    {
        if (nodes.size() < 2)
            return;
        VfxRibbonBatch batch;
        batch.Settings = settings;
        batch.FirstNode = static_cast<uint32_t>(m_vfxRibbonNodes.size());
        batch.NodeCount = static_cast<uint32_t>(nodes.size());
        m_vfxRibbonNodes.insert(m_vfxRibbonNodes.end(), nodes.begin(), nodes.end());
        m_vfxRibbonBatches.push_back(batch);
    }

    void Renderer::SubmitVfxDecal(const shaderio::GpuVfxDecal& decal)
    {
        m_vfxDecals.push_back(decal);
    }

    void Renderer::addVfxDecalPass()
    {
        if (m_vfxDecals.empty() || !m_vfxDecalPipeline || m_frame.renderExtent.width == 0 || m_frame.renderExtent.height == 0)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.HDRScene.IsValid() || !resources.Depth.IsValid())
            return;

        const uint64_t required = sizeof(shaderio::GpuVfxDecal) * m_vfxDecals.size();
        if (required > m_vfxDecalBufferCapacity || m_vfxDecalBuffers.size() != MAX_FRAMES_IN_FLIGHT)
        {
            for (auto& old : m_vfxDecalBuffers)
            {
                if (old)
                {
                    releaseBufferRefs(old.handle());
                    std::scoped_lock lock(m_deferredReleasesMutex);
                    m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(old) });
                }
            }
            m_vfxDecalBuffers.clear();
            m_vfxDecalBuffersMapped.clear();
            m_vfxDecalBufferCapacity = std::max<uint64_t>(required * 2, sizeof(shaderio::GpuVfxDecal) * 64);
            for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
            {
                nri::Buffer buf = m_device.createBuffer(nri::BufferDesc{
                    .size = m_vfxDecalBufferCapacity,
                    .usage = NRI_BUFFER_USAGE_UNIFORM
                });
                m_vfxDecalBuffersMapped.push_back(buf.map(0, m_vfxDecalBufferCapacity));
                m_vfxDecalBuffers.push_back(std::move(buf));
            }
        }
        std::memcpy(m_vfxDecalBuffersMapped[frameIndex], m_vfxDecals.data(), required);

        const uint64_t uniformsRef = bufferReadRef(m_uniformBuffers[frameIndex]);
        const uint64_t decalsRef = bufferReadRef(m_vfxDecalBuffers[frameIndex]);
        std::vector<uint32_t> decalFlags;
        decalFlags.reserve(m_vfxDecals.size());
        for (const shaderio::GpuVfxDecal& d : m_vfxDecals)
            decalFlags.push_back(d.flags);
        const glm::vec2 viewportSize(static_cast<float>(m_frame.renderExtent.width), static_cast<float>(m_frame.renderExtent.height));

        m_renderGraph.AddPass("VFX Decals", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.HDRScene, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                builder.Read(resources.Depth);
                if (resources.GBufferNormal.IsValid())
                    builder.Read(resources.GBufferNormal);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, uniformsRef, decalsRef, decalFlags = std::move(decalFlags), viewportSize](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_vfxDecalPipeline);
                cmd.setVertexInput();
                cmd.setPrimitiveTopology(NRI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
                cmd.setPrimitiveRestartEnable(false);
                // Back-face culling with inward-wound cube in VfxDecal.slang renders the inside backfaces once per pixel
                // so decals still render cleanly even when the camera is inside the decal volume.
                cmd.setCullMode(NRI_CULL_MODE_BACK);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, true);
                cmd.setColorWriteMask(0, nri::ColorComponent::All);

                const nri::ColorBlendEquation additive{
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD,
                    NRI_BLEND_FACTOR_ZERO, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD
                };
                const nri::ColorBlendEquation covering{
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NRI_BLEND_OP_ADD,
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NRI_BLEND_OP_ADD
                };
                const uint32_t depthSlot = context.Slot(res->Depth);
                const uint32_t normalSlot = res->GBufferNormal.IsValid() ? context.Slot(res->GBufferNormal) : 0xFFFFFFFFu;

                for (uint32_t i = 0; i < static_cast<uint32_t>(decalFlags.size()); ++i)
                {
                    cmd.setColorBlendEquation(0, (decalFlags[i] & 1u) != 0u ? additive : covering);
                    shaderio::PushConstantVfxDecal push{};
                    push.matrixReference = uniformsRef;
                    push.decalsReference = decalsRef;
                    push.decalIndex = i;
                    push.depthTextureIndex = depthSlot;
                    push.normalTextureIndex = normalSlot;
                    push.viewportSize = viewportSize;
                    cmd.pushData(&push, sizeof(push));
                    cmd.draw(36u, 1u, 0u, 0u);
                }
                cmd.setColorBlendEnable(0, false);
            });
    }

    void Renderer::addVfxRibbonPass()
    {
        if (m_vfxRibbonBatches.empty() || m_vfxRibbonNodes.empty() || !m_vfxRibbonPipeline ||
            m_frame.renderExtent.width == 0 || m_frame.renderExtent.height == 0)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.HDRScene.IsValid() || !resources.Depth.IsValid())
            return;

        const uint64_t required = sizeof(shaderio::GpuVfxRibbonNode) * m_vfxRibbonNodes.size();
        if (required > m_vfxRibbonBufferCapacity || m_vfxRibbonBuffers.size() != MAX_FRAMES_IN_FLIGHT)
        {
            for (auto& old : m_vfxRibbonBuffers)
            {
                if (old)
                {
                    releaseBufferRefs(old.handle());
                    std::scoped_lock lock(m_deferredReleasesMutex);
                    m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(old) });
                }
            }
            m_vfxRibbonBuffers.clear();
            m_vfxRibbonBuffersMapped.clear();
            m_vfxRibbonBufferCapacity = std::max<uint64_t>(required * 2, sizeof(shaderio::GpuVfxRibbonNode) * 1024);
            for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
            {
                nri::Buffer buf = m_device.createBuffer(nri::BufferDesc{
                    .size = m_vfxRibbonBufferCapacity,
                    .usage = NRI_BUFFER_USAGE_UNIFORM
                });
                m_vfxRibbonBuffersMapped.push_back(buf.map(0, m_vfxRibbonBufferCapacity));
                m_vfxRibbonBuffers.push_back(std::move(buf));
            }
        }
        std::memcpy(m_vfxRibbonBuffersMapped[frameIndex], m_vfxRibbonNodes.data(), required);

        const uint64_t uniformsRef = bufferReadRef(m_uniformBuffers[frameIndex]);
        const uint64_t nodesRef = bufferReadRef(m_vfxRibbonBuffers[frameIndex]);
        std::vector<VfxRibbonBatch> batches = m_vfxRibbonBatches;

        m_renderGraph.AddPass("VFX Ribbons & Beams", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.HDRScene, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                builder.Read(resources.Depth);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, uniformsRef, nodesRef, batches = std::move(batches)](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_vfxRibbonPipeline);
                cmd.setVertexInput();
                cmd.setPrimitiveTopology(NRI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
                cmd.setPrimitiveRestartEnable(false);
                cmd.setCullMode(NRI_CULL_MODE_NONE);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, true);
                cmd.setColorWriteMask(0, nri::ColorComponent::All);

                const nri::ColorBlendEquation additive{
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD,
                    NRI_BLEND_FACTOR_ZERO, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD
                };
                const nri::ColorBlendEquation covering{
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NRI_BLEND_OP_ADD,
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NRI_BLEND_OP_ADD
                };
                const uint32_t depthSlot = context.Slot(res->Depth);

                for (const VfxRibbonBatch& batch : batches)
                {
                    if (batch.NodeCount < 2u)
                        continue;
                    cmd.setColorBlendEquation(0, batch.Settings.Additive ? additive : covering);
                    shaderio::PushConstantVfxRibbon push{};
                    push.matrixReference = uniformsRef;
                    push.nodesReference = nodesRef;
                    push.firstNode = batch.FirstNode;
                    push.nodeCount = batch.NodeCount;
                    push.textureIndex = batch.Settings.TextureSlot;
                    push.depthTextureIndex = depthSlot;
                    push.flags = (batch.Settings.Soft ? shaderio::VFX_RIBBON_SOFT : 0u) |
                                 (batch.Settings.Additive ? shaderio::VFX_RIBBON_ADDITIVE : 0u) |
                                 (batch.Settings.Beam ? shaderio::VFX_RIBBON_BEAM : 0u);
                    push.softDistance = batch.Settings.SoftDistance;
                    push.uvTileU = batch.Settings.UvTileU;
                    push.uvScrollU = batch.Settings.UvScrollU;
                    cmd.pushData(&push, sizeof(push));
                    cmd.draw((batch.NodeCount - 1u) * 6u, 1u, 0u, 0u);
                }
                cmd.setColorBlendEnable(0, false);
            });
    }

    void Renderer::SubmitVfxDistortion(const shaderio::GpuVfxDistortion& distortion)
    {
        m_vfxDistortions.push_back(distortion);
    }

    void Renderer::addVfxMeshParticlePass()
    {
        if (m_vfxMeshBatches.empty() || !m_vfxMeshParticlePipeline ||
            m_frame.renderExtent.width == 0 || m_frame.renderExtent.height == 0)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.HDRScene.IsValid() || !resources.Depth.IsValid())
            return;

        const uint64_t uniformsRef = bufferReadRef(m_uniformBuffers[frameIndex]);
        const uint64_t verticesRef = m_vertexStream.GetBuffer() ? bufferReadRef(m_vertexStream.GetBuffer()) : 0ull;
        const uint64_t indicesRef = m_rtIndexStream.GetBuffer() ? bufferReadRef(m_rtIndexStream.GetBuffer()) : 0ull;
        std::vector<GpuVfxMeshBatch> batches = m_vfxMeshBatches;

        m_renderGraph.AddPass("VFX Mesh Particles", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.HDRScene, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                builder.Read(resources.Depth);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, uniformsRef, verticesRef, indicesRef, batches = std::move(batches)](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_vfxMeshParticlePipeline);
                cmd.setCullMode(NRI_CULL_MODE_BACK);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, true);
                cmd.setColorWriteMask(0, nri::ColorComponent::All);

                const nri::ColorBlendEquation additive{
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD,
                    NRI_BLEND_FACTOR_ZERO, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD
                };
                const nri::ColorBlendEquation covering{
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NRI_BLEND_OP_ADD,
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NRI_BLEND_OP_ADD
                };
                const uint32_t depthSlot = context.Slot(res->Depth);

                for (const GpuVfxMeshBatch& batch : batches)
                {
                    if (!batch.IndirectBuffer)
                        continue;
                    cmd.setColorBlendEquation(0, batch.Settings.Additive ? additive : covering);
                    shaderio::PushConstantVfxMeshParticle push{};
                    push.matrixReference = uniformsRef;
                    push.particlesReference = batch.ParticlesRef;
                    push.renderParticlesReference = batch.RenderParticlesRef;
                    push.aliveListReference = batch.AliveListRef;
                    push.countersReference = batch.CountersRef;
                    const bool hasCustomMesh = batch.MeshOptions.TriangleCount > 0u && verticesRef != 0ull && indicesRef != 0ull;
                    push.verticesReference = hasCustomMesh ? verticesRef : 0ull;
                    push.indicesReference = hasCustomMesh ? indicesRef : 0ull;
                    push.verticesOffset = batch.MeshOptions.VerticesOffset;
                    push.indicesOffset = batch.MeshOptions.IndicesOffset;
                    push.triangleCount = hasCustomMesh ? batch.MeshOptions.TriangleCount : 8u;
                    push.textureIndex = batch.Settings.TextureSlot;
                    push.depthTextureIndex = depthSlot;
                    push.flags = (batch.Settings.Lit ? shaderio::PARTICLE_LIT : 0u) |
                                 (batch.Settings.Additive ? shaderio::PARTICLE_ADDITIVE : 0u) |
                                 (batch.MeshOptions.AlignToVelocity ? shaderio::VFX_MESH_ALIGN_VELOCITY : 0u);
                    push.roughness = 0.35f;
                    push.metallic = 0.1f;
                    cmd.pushData(&push, sizeof(push));
                    nri::drawMeshTasksIndirect(cmd, *batch.IndirectBuffer, 0, 1, 12u);
                }
                cmd.setColorBlendEnable(0, false);
            });
    }

    void Renderer::addVfxDistortionPass()
    {
        if (m_vfxDistortions.empty() || !m_vfxDistortionPipeline ||
            m_frame.renderExtent.width == 0 || m_frame.renderExtent.height == 0)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.HDRScene.IsValid() || !resources.Depth.IsValid())
            return;

        const uint64_t required = sizeof(shaderio::GpuVfxDistortion) * m_vfxDistortions.size();
        if (required > m_vfxDistortionBufferCapacity || m_vfxDistortionBuffers.size() != MAX_FRAMES_IN_FLIGHT)
        {
            for (auto& old : m_vfxDistortionBuffers)
            {
                if (old)
                {
                    releaseBufferRefs(old.handle());
                    std::scoped_lock lock(m_deferredReleasesMutex);
                    m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(old) });
                }
            }
            m_vfxDistortionBuffers.clear();
            m_vfxDistortionBuffersMapped.clear();
            m_vfxDistortionBufferCapacity = std::max<uint64_t>(required * 2, sizeof(shaderio::GpuVfxDistortion) * 32);
            for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
            {
                nri::Buffer buf = m_device.createBuffer(nri::BufferDesc{
                    .size = m_vfxDistortionBufferCapacity,
                    .usage = NRI_BUFFER_USAGE_UNIFORM
                });
                m_vfxDistortionBuffersMapped.push_back(buf.map(0, m_vfxDistortionBufferCapacity));
                m_vfxDistortionBuffers.push_back(std::move(buf));
            }
        }
        std::memcpy(m_vfxDistortionBuffersMapped[frameIndex], m_vfxDistortions.data(), required);

        RGTextureDesc copyDesc;
        copyDesc.Format = NRI_FORMAT_R16G16B16A16_SFLOAT;
        const RGTexture sceneCopy = m_renderGraph.CreateTexture("VFX Distortion Scene Copy", copyDesc);
        m_renderGraph.AddPass("VFX Distortion Copy", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.HDRScene, RGTextureAccess::CopySource);
                builder.Write(sceneCopy, RGTextureAccess::CopyDestination);
            },
            [this, res = &resources, sceneCopy](RGPassContext& context)
            {
                nriCmdCopyTexture(context.Cmd().handle(), context.Texture(res->HDRScene), context.Texture(sceneCopy),
                                  m_frame.renderExtent.width, m_frame.renderExtent.height);
            });

        const uint64_t uniformsRef = bufferReadRef(m_uniformBuffers[frameIndex]);
        const uint64_t distortionsRef = bufferReadRef(m_vfxDistortionBuffers[frameIndex]);
        const uint32_t count = static_cast<uint32_t>(m_vfxDistortions.size());

        m_renderGraph.AddPass("VFX Distortion", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.HDRScene, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                builder.Read(sceneCopy);
                builder.Read(resources.Depth);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, sceneCopy, uniformsRef, distortionsRef, count](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_vfxDistortionPipeline);
                cmd.setVertexInput();
                cmd.setPrimitiveTopology(NRI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
                cmd.setPrimitiveRestartEnable(false);
                cmd.setCullMode(NRI_CULL_MODE_NONE);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, nri::ColorComponent::All);

                const uint32_t colorSlot = context.Slot(sceneCopy);
                const uint32_t depthSlot = context.Slot(res->Depth);
                for (uint32_t i = 0; i < count; ++i)
                {
                    shaderio::PushConstantVfxDistortion push{};
                    push.matrixReference = uniformsRef;
                    push.distortionsReference = distortionsRef;
                    push.distortionIndex = i;
                    push.sceneColorIndex = colorSlot;
                    push.depthTextureIndex = depthSlot;
                    cmd.pushData(&push, sizeof(push));
                    cmd.draw(6u, 1u, 0u, 0u);
                }
            });
    }

    void Renderer::SubmitHeterogeneousVolume(const GpuVolumeSubmission& submission)
    {
        m_vfxVolumeSubmissions.push_back(submission);
    }

    void Renderer::addHeterogeneousVolumePass()
    {
        // Evict idle volume streams after 120 frames.
        auto releaseStream = [&](GpuVolumeStream& s)
        {
            for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
            {
                if (s.Density[i].Buffer) releaseBufferRefs(s.Density[i].Buffer.handle());
                if (s.Temperature[i].Buffer) releaseBufferRefs(s.Temperature[i].Buffer.handle());
                if (s.Flame[i].Buffer) releaseBufferRefs(s.Flame[i].Buffer.handle());
            }
        };
        for (auto it = m_vfxVolumeStreams.begin(); it != m_vfxVolumeStreams.end(); )
        {
            if (m_vfxFrameCounter > it->second->LastUsedFrame + 120u)
            {
                releaseStream(*it->second);
                it = m_vfxVolumeStreams.erase(it);
            }
            else
                ++it;
        }

        if (m_vfxVolumeSubmissions.empty() || !m_vfxVolumePipeline ||
            m_frame.renderExtent.width == 0 || m_frame.renderExtent.height == 0)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.HDRScene.IsValid() || !resources.Depth.IsValid())
            return;

        auto uploadChannel = [&](GpuVolumeChannelSlot& slot, const std::vector<uint8_t>& bytes, uint64_t minCap) -> uint64_t
        {
            if (bytes.empty())
                return 0ull;
            // Pad to 4-byte multiple so uint32_t FP16 pair loads never read past the end.
            const uint64_t padded = (bytes.size() + 3ull) & ~3ull;
            if (padded > slot.Capacity || !slot.Buffer)
            {
                if (slot.Buffer)
                {
                    releaseBufferRefs(slot.Buffer.handle());
                    std::scoped_lock lock(m_deferredReleasesMutex);
                    m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(slot.Buffer) });
                }
                slot.Capacity = std::max<uint64_t>({ padded, minCap, 4096ull });
                slot.Buffer = m_device.createBuffer(nri::BufferDesc{ .size = slot.Capacity, .usage = NRI_BUFFER_USAGE_UNIFORM });
                slot.Mapped = slot.Buffer.map(0, slot.Capacity);
            }
            std::memcpy(slot.Mapped, bytes.data(), bytes.size());
            if (padded > bytes.size())
                std::memset(static_cast<uint8_t*>(slot.Mapped) + bytes.size(), 0, padded - bytes.size());
            return bufferReadRef(slot.Buffer);
        };

        std::vector<shaderio::GpuHeterogeneousVolume> gpuVolumes;
        gpuVolumes.reserve(m_vfxVolumeSubmissions.size());
        std::vector<uint8_t> scratchBytes;

        for (const GpuVolumeSubmission& sub : m_vfxVolumeSubmissions)
        {
            shaderio::GpuHeterogeneousVolume gpu = sub.Params;
            if (sub.Asset && sub.Asset->IsValid())
            {
                auto& streamPtr = m_vfxVolumeStreams[sub.EntityId];
                if (!streamPtr)
                    streamPtr = std::make_unique<GpuVolumeStream>();
                GpuVolumeStream& stream = *streamPtr;
                stream.LastUsedFrame = m_vfxFrameCounter;

                if (stream.AssetHandle != static_cast<uint64_t>(sub.Asset->Handle))
                {
                    stream.AssetHandle = static_cast<uint64_t>(sub.Asset->Handle);
                    stream.SlotFrame.fill(~0u);
                }

                const uint32_t fIdx = sub.FrameIndex % sub.Asset->GetFrameCount();
                if (stream.SlotFrame[frameIndex] != fIdx)
                {
                    // Pre-size channel buffers to the sequence maximum so they never reallocate mid-playback.
                    uint64_t maxDens = 0, maxTemp = 0, maxFlame = 0;
                    for (uint32_t fi = 0; fi < sub.Asset->GetFrameCount(); ++fi)
                    {
                        const VolumeFrameRecord& fr = sub.Asset->GetFrame(fi);
                        maxDens = std::max(maxDens, (fr.DensityBytes + 3ull) & ~3ull);
                        maxTemp = std::max(maxTemp, (fr.TemperatureBytes + 3ull) & ~3ull);
                        maxFlame = std::max(maxFlame, (fr.FlameBytes + 3ull) & ~3ull);
                    }

                    const VolumeFrameRecord& rec = sub.Asset->GetFrame(fIdx);
                    stream.SlotFrame[frameIndex] = fIdx;
                    stream.SlotDimX[frameIndex] = rec.DimX;
                    stream.SlotDimY[frameIndex] = rec.DimY;
                    stream.SlotDimZ[frameIndex] = rec.DimZ;
                    uint32_t flags = 0u;

                    if (sub.Asset->ReadFrameChannel(fIdx, VolumeChannel::Density, scratchBytes))
                    {
                        uploadChannel(stream.Density[frameIndex], scratchBytes, maxDens);
                        flags |= shaderio::VOLUME_FLAG_HAS_DENSITY;
                    }
                    if (sub.Asset->ReadFrameChannel(fIdx, VolumeChannel::Temperature, scratchBytes))
                    {
                        uploadChannel(stream.Temperature[frameIndex], scratchBytes, maxTemp);
                        flags |= shaderio::VOLUME_FLAG_HAS_TEMPERATURE;
                    }
                    if (sub.Asset->ReadFrameChannel(fIdx, VolumeChannel::Flame, scratchBytes))
                    {
                        uploadChannel(stream.Flame[frameIndex], scratchBytes, maxFlame);
                        flags |= shaderio::VOLUME_FLAG_HAS_FLAME;
                    }
                    stream.SlotChannelFlags[frameIndex] = flags;
                }

                const uint32_t flags = stream.SlotChannelFlags[frameIndex];
                gpu.densityBufferRef = (flags & shaderio::VOLUME_FLAG_HAS_DENSITY) && stream.Density[frameIndex].Buffer
                    ? bufferReadRef(stream.Density[frameIndex].Buffer) : 0ull;
                gpu.temperatureBufferRef = (flags & shaderio::VOLUME_FLAG_HAS_TEMPERATURE) && stream.Temperature[frameIndex].Buffer
                    ? bufferReadRef(stream.Temperature[frameIndex].Buffer) : 0ull;
                gpu.flameBufferRef = (flags & shaderio::VOLUME_FLAG_HAS_FLAME) && stream.Flame[frameIndex].Buffer
                    ? bufferReadRef(stream.Flame[frameIndex].Buffer) : 0ull;
                gpu.dimX = stream.SlotDimX[frameIndex];
                gpu.dimY = stream.SlotDimY[frameIndex];
                gpu.dimZ = stream.SlotDimZ[frameIndex];
                gpu.flags = flags;
            }
            else
            {
                gpu.flags = shaderio::VOLUME_FLAG_PROCEDURAL;
            }
            gpuVolumes.push_back(gpu);
        }

        const uint64_t required = sizeof(shaderio::GpuHeterogeneousVolume) * gpuVolumes.size();
        if (required > m_vfxVolumeBufferCapacity || m_vfxVolumeBuffers.size() != MAX_FRAMES_IN_FLIGHT)
        {
            for (auto& old : m_vfxVolumeBuffers)
            {
                if (old)
                {
                    releaseBufferRefs(old.handle());
                    std::scoped_lock lock(m_deferredReleasesMutex);
                    m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(old) });
                }
            }
            m_vfxVolumeBuffers.clear();
            m_vfxVolumeBuffersMapped.clear();
            m_vfxVolumeBufferCapacity = std::max<uint64_t>(required * 2, sizeof(shaderio::GpuHeterogeneousVolume) * 16);
            for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
            {
                nri::Buffer buf = m_device.createBuffer(nri::BufferDesc{
                    .size = m_vfxVolumeBufferCapacity,
                    .usage = NRI_BUFFER_USAGE_UNIFORM
                });
                m_vfxVolumeBuffersMapped.push_back(buf.map(0, m_vfxVolumeBufferCapacity));
                m_vfxVolumeBuffers.push_back(std::move(buf));
            }
        }
        std::memcpy(m_vfxVolumeBuffersMapped[frameIndex], gpuVolumes.data(), required);

        const uint64_t uniformsRef = bufferReadRef(m_uniformBuffers[frameIndex]);
        const uint64_t volumesRef = bufferReadRef(m_vfxVolumeBuffers[frameIndex]);
        const uint32_t count = static_cast<uint32_t>(gpuVolumes.size());
        const uint32_t curFrame = static_cast<uint32_t>(m_vfxFrameCounter);
        const glm::vec2 viewportSize(static_cast<float>(m_frame.renderExtent.width), static_cast<float>(m_frame.renderExtent.height));

        m_renderGraph.AddPass("Heterogeneous Volumes", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.HDRScene, NRI_LOAD_OP_LOAD, NRI_STORE_OP_STORE);
                builder.Read(resources.Depth);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, uniformsRef, volumesRef, count, curFrame, viewportSize](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_vfxVolumePipeline);
                cmd.setVertexInput();
                cmd.setPrimitiveTopology(NRI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
                cmd.setPrimitiveRestartEnable(false);
                cmd.setCullMode(NRI_CULL_MODE_BACK);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, true);
                cmd.setColorWriteMask(0, nri::ColorComponent::All);
                cmd.setColorBlendEquation(0, {
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NRI_BLEND_OP_ADD,
                    NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NRI_BLEND_OP_ADD
                });

                const uint32_t depthSlot = context.Slot(res->Depth);
                for (uint32_t i = 0; i < count; ++i)
                {
                    shaderio::PushConstantHeterogeneousVolume push{};
                    push.matrixReference = uniformsRef;
                    push.volumesReference = volumesRef;
                    push.volumeIndex = i;
                    push.depthTextureIndex = depthSlot;
                    push.frameIndex = curFrame;
                    push.viewportSize = viewportSize;
                    cmd.pushData(&push, sizeof(push));
                    cmd.draw(36u, 1u, 0u, 0u);
                }
                cmd.setColorBlendEnable(0, false);
            });
    }
}
