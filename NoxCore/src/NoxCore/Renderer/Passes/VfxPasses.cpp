// GPU Compute VFX Simulation (docs/VFX_And_Volumes_Plan_2026-11.md step V1):
// Persistent per-entity GPU particle pools (DeadList, ping-pong AliveLists, atomic counters, IndirectArgs) simulated by
// VfxSimulate.slang and drawn indirectly by Particles.slang in addParticlePass().
#include "NoxCore/Renderer/Renderer.h"
#include "FrameGraphResources.h"

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
                                            const shaderio::GpuVfxEmitterParams& params, bool restart, int32_t sceneEntityHandle)
    {
        if (params.maxParticles == 0u)
            return;
        GpuVfxSubmission sub;
        sub.EntityId = entityId;
        sub.SceneEntityHandle = sceneEntityHandle;
        sub.Settings = settings;
        sub.Params = params;
        sub.Restart = restart;
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

            // Register this GPU pool as a ParticleBatch so addParticlePass() draws it via DrawMeshTasksIndirect.
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
                        { pool.RenderParticles.handle(), { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead, nri::StageBits::Mesh | nri::StageBits::Fragment } },
                        { pool.Counters.handle(),        { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::ShaderRead, nri::StageBits::Mesh } },
                        { pool.IndirectArgs.handle(),    { nri::AccessBits::ShaderWrite, nri::StageBits::Compute }, { nri::AccessBits::IndirectRead, nri::StageBits::Indirect } },
                    };
                    cmd.resourceBarriers({}, std::span<const nri::BufferBarrierDesc>(toDraw));
                }
            });
    }
}
