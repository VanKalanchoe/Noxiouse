// AVBOIT -- Adaptive Voxel-Based Order-Independent Transparency (Michal Drobot, Activision, "Advances in Real-Time Rendering",
// SIGGRAPH 2025; Unreal's AVBOIT): Settings > Order-Independent Transparency, method AVBOIT. Before the transparent surfaces are
// shaded into the OIT targets, every one of them (transparent meshes, alpha-blended particles) builds a low-resolution volume of the
// transmittance in front of each depth (render size / 8 columns x physical slices); the shading pass then weights each fragment by
// the transmittance in front of it (OITShared.slang has the whole method):
//   AVBOIT Clear      the buffers (occupancy, overflow, extinction)
//   AVBOIT Occupancy  the surfaces at the volume's resolution: the virtual depth slices they are in
//   AVBOIT Depth Warp one thread: the virtual resolution that fits the physical slices, empty depth ranges skipped
//   AVBOIT Splat      the surfaces again: their extinction per color channel into the two slices around them
//   AVBOIT Integrate  front to back per column: the transmittance volume (RGBA8 array, slices + 1 layers)
// The buffers and the volume are the renderer's own (sized for the render size and slices, made again when they change); each
// compute step ends with a full barrier, as the volumetric fog's.
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"
#include "NoxCore/Core/WorldUnits.h"

namespace Nox
{
    void Renderer::createAVBOITPipelines(bool forceCompile)
    {
        nri::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        // The prepasses draw at the volume's resolution into a placeholder target (nothing is written to it): their output is atomics.
        desc.colorFormats = { NRI_FORMAT_RGBA8 };
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_TASK, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_MESH, .entryPoint = "meshMain", .sourcePath = "assets/shaders/Material_PBR_Mesh.slang" });
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_FRAGMENT, .entryPoint = "fragMainAVBOITOccupancy", .sourcePath = "assets/shaders/Material_TransparentLit_Mesh.slang" });
        m_transparentLitAVBOITOccupancyPipeline = m_device.createPipeline(desc, m_shaderCompiler.handle());
        desc.shaders.back().entryPoint = "fragMainAVBOITSplat";
        m_transparentLitAVBOITSplatPipeline = m_device.createPipeline(desc, m_shaderCompiler.handle());
        desc.shaders.back() = { .stage = NRI_SHADER_STAGE_FRAGMENT, .entryPoint = "fragMainAVBOITOccupancy", .sourcePath = "assets/shaders/Material_Unlit_Mesh.slang" };
        m_unlitAVBOITOccupancyPipeline = m_device.createPipeline(desc, m_shaderCompiler.handle());
        desc.shaders.back().entryPoint = "fragMainAVBOITSplat";
        m_unlitAVBOITSplatPipeline = m_device.createPipeline(desc, m_shaderCompiler.handle());

        desc.shaders.clear();
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_MESH, .entryPoint = "meshMain", .sourcePath = "assets/shaders/Particles.slang" });
        desc.shaders.push_back({ .stage = NRI_SHADER_STAGE_FRAGMENT, .entryPoint = "fragMainAVBOITOccupancy", .sourcePath = "assets/shaders/Particles.slang" });
        m_particleAVBOITOccupancyPipeline = m_device.createPipeline(desc, m_shaderCompiler.handle());
        desc.shaders.back().entryPoint = "fragMainAVBOITSplat";
        m_particleAVBOITSplatPipeline = m_device.createPipeline(desc, m_shaderCompiler.handle());

        nri::PipelineDesc compute{};
        compute.type = NRI_PIPELINE_COMPUTE;
        compute.forceCompile = forceCompile;
        compute.shaders.push_back({ .stage = NRI_SHADER_STAGE_COMPUTE, .entryPoint = "lutMain", .sourcePath = "assets/shaders/AVBOITBuild.slang" });
        m_avboitLutPipeline = m_device.createPipeline(compute, m_shaderCompiler.handle());
        compute.shaders.back().entryPoint = "integrateMain";
        m_avboitIntegratePipeline = m_device.createPipeline(compute, m_shaderCompiler.handle());
    }

    void Renderer::declareAVBOITPrepassInputs(RGBuilder& builder)
    {
        // What the transparent shading reads (the splat shades each surface for its transmittance), less the targets: the scene depth
        // is read (each fragment tests itself against it), not attached -- the prepasses draw at the volume's resolution.
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        builder.Read(resources.Depth);
        if (resources.AerialPerspective.IsValid())
            builder.Read(resources.AerialPerspective);
        if (m_frame.ddgiAdded)
            ReadDDGI(builder, resources, m_frame.ddgiWriteIndex);
        if (m_frame.screenProbeGIAdded)
        {
            builder.Read(resources.ScreenProbeGI[m_frame.screenProbeGIWriteIndex]);
            builder.Read(resources.ScreenProbeGIDistance[m_frame.screenProbeGIWriteIndex]);
        }
        if (m_frame.ssgiAdded && resources.SSGI.IsValid() && resources.SSGIDepth.IsValid())
        {
            builder.Read(resources.SSGI);
            builder.Read(resources.SSGIDepth);
        }
        if (resources.TLAS.IsValid())
            builder.Read(resources.TLAS, RGBufferAccess::AccelerationStructureRead);
        ReadShadowMaps(builder, resources);
        ReadGpuScene(builder, resources);
        ReadViewDraws(builder, resources.CameraDraws);
        ReadViewDraws(builder, resources.CameraDraws, true);
        builder.Read(resources.MipFeedback);
        builder.Write(resources.MipFeedback);
        builder.SetRenderArea({ m_avboitGrid.x, m_avboitGrid.y });
    }

    void Renderer::addAVBOITPasses(bool meshes)
    {
        if (m_frame.avboitAdded || m_oitMethod != OITMethod::AVBOIT || !isOITActive() || !m_avboitLutPipeline || !m_avboitIntegratePipeline ||
            !m_transparentLitAVBOITOccupancyPipeline || !m_transparentLitAVBOITSplatPipeline || !m_unlitAVBOITOccupancyPipeline ||
            !m_unlitAVBOITSplatPipeline || !m_particleAVBOITOccupancyPipeline || !m_particleAVBOITSplatPipeline ||
            m_frame.renderExtent.width == 0 || m_frame.renderExtent.height == 0)
            return;

        // The alpha-blended particles that go into the OIT targets (addParticlePass), drawn far to near like there (no order needed).
        std::vector<ParticleBatch> particleBatches;
        const bool particleBuffer = m_particleBuffers.size() > frameIndex && m_particleBuffers[frameIndex];
        if (m_particleOITPipeline)
        {
            for (const ParticleBatch& batch : m_particleBatches)
            {
                if (!batch.Settings.Additive && (batch.GpuIndirectBuffer != nullptr || particleBuffer))
                    particleBatches.push_back(batch);
            }
        }
        if (!meshes && particleBatches.empty())
            return;
        m_frame.avboitAdded = true;

        // The volume for this render size and slice count: render size / 8 columns, slices + 1 layers (in front of each, behind all).
        const glm::uvec3 grid((m_frame.renderExtent.width + shaderio::AVBOIT_TILE_SIZE - 1) / shaderio::AVBOIT_TILE_SIZE,
                              (m_frame.renderExtent.height + shaderio::AVBOIT_TILE_SIZE - 1) / shaderio::AVBOIT_TILE_SIZE, m_avboitSlices);
        if (grid != m_avboitGrid || !m_avboitTransmittance)
        {
            if (m_avboitTransmittanceStorage != ~0u)
                m_resourceHeap.unregisterTexture(m_avboitTransmittanceStorage);
            m_avboitTransmittanceStorage = ~0u;
            if (m_avboitTransmittance)
                deferAssetRelease(m_avboitTransmittance);
            // The buffers the last frames still use go once they are done with them (the GPU is waited for: a resize is rare).
            m_device.waitIdle();
            const uint64_t columns = static_cast<uint64_t>(grid.x) * grid.y;
            m_avboitOccupancy = m_device.createBuffer(nri::BufferDesc{ .size = sizeof(uint32_t) * (shaderio::AVBOIT_VIRTUAL_SLICES / 32), .usage = NRI_BUFFER_USAGE_STORAGE });
            m_avboitDepthWarp = m_device.createBuffer(nri::BufferDesc{ .size = sizeof(shaderio::AvboitDepthWarp), .usage = NRI_BUFFER_USAGE_STORAGE });
            m_avboitExtinction = m_device.createBuffer(nri::BufferDesc{ .size = sizeof(uint32_t) * columns * grid.z, .usage = NRI_BUFFER_USAGE_STORAGE });
            m_avboitOverflow = m_device.createBuffer(nri::BufferDesc{ .size = sizeof(uint32_t) * 3 * columns, .usage = NRI_BUFFER_USAGE_STORAGE });
            m_avboitTransmittance = CreateRef<Texture2D>(m_device.createTexture(nri::TextureDesc{
                .width = grid.x,
                .height = grid.y,
                .arrayLayers = grid.z + 1,
                .isCubeMap = false,
                .mipLevels = 1,
                .sampleCount = 1,
                .usage = NRI_TEXTURE_USAGE_STORAGE,
                .format = NRI_FORMAT_RGBA8
            }));
            m_resourceHeap.registerTexture(*m_avboitTransmittance, NRI_TEXTURE_USAGE_SHADER_RESOURCE);
            m_avboitTransmittanceStorage = m_resourceHeap.registerStorageTextureMip(*m_avboitTransmittance, 0);
            m_avboitGrid = grid;
        }

        uniformData.oitMode = shaderio::OIT_AVBOIT;
        uniformData.avboitGridWidth = grid.x;
        uniformData.avboitGridHeight = grid.y;
        uniformData.avboitSlices = grid.z;
        uniformData.avboitDistance = WorldUnits::FromMeters(m_avboitDistanceMeters);
        uniformData.avboitLinearDistance = WorldUnits::FromMeters(m_avboitLinearMeters);
        uniformData.avboitTransmittanceTextureIndex = m_avboitTransmittance->GetDescriptorIndexSlot();
        uniformData.avboitOccupancyReference = bufferWriteRef(m_avboitOccupancy);
        uniformData.avboitLutReference = bufferWriteRef(m_avboitDepthWarp);
        uniformData.avboitExtinctionReference = bufferWriteRef(m_avboitExtinction);
        uniformData.avboitOverflowReference = bufferWriteRef(m_avboitOverflow);
        // (avboitDepthTextureIndex: the depth's slot once the graph is compiled, resolveFrameUniforms)

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        const uint64_t uniforms = bufferReadRef(m_uniformBuffers[frameIndex]);
        const uint64_t particles = particleBuffer ? bufferReadRef(m_particleBuffers[frameIndex]) : 0;
        const uint64_t columns = static_cast<uint64_t>(grid.x) * grid.y;
        nri::Buffer* occupancy = &m_avboitOccupancy;
        nri::Buffer* extinction = &m_avboitExtinction;
        nri::Buffer* overflow = &m_avboitOverflow;

        m_renderGraph.AddPass("AVBOIT Clear", RGPassFlags::NeverCull,
            [](RGBuilder&) {},
            [occupancy, extinction, overflow, columns, grid](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.fillBuffer(*occupancy, 0, sizeof(uint32_t) * (shaderio::AVBOIT_VIRTUAL_SLICES / 32), 0);
                cmd.fillBuffer(*extinction, 0, sizeof(uint32_t) * columns * grid.z, 0);
                cmd.fillBuffer(*overflow, 0, sizeof(uint32_t) * 3 * columns, 0xFFFFFFFFu); // no slice overflowed yet
                cmd.executionBarrier();
            });

        // The two raster prepasses: the transparent meshes (both culling phases, as the shading pass draws them) and the particles,
        // with the given fragment stages, nothing blended or written but their atomics.
        RGTextureDesc placeholderDesc;
        placeholderDesc.Size = RGSize::Absolute;
        placeholderDesc.Width = grid.x;
        placeholderDesc.Height = grid.y;
        placeholderDesc.Format = NRI_FORMAT_RGBA8;
        auto addPrepass = [&](const char* name, nri::Pipeline* litPipeline, nri::Pipeline* unlitPipeline, nri::Pipeline* particlePipeline)
        {
            const RGTexture placeholder = m_renderGraph.CreateTexture(name, placeholderDesc);
            m_renderGraph.AddPass(name, RGPassFlags::Raster | RGPassFlags::NeverCull,
                [&](RGBuilder& builder)
                {
                    builder.ColorTarget(placeholder, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 0.0f });
                    declareAVBOITPrepassInputs(builder);
                },
                [this, res = &resources, meshes, litPipeline, unlitPipeline, particlePipeline, particleBatches, particles, uniforms](RGPassContext& context)
                {
                    nri::CommandBuffer& cmd = context.Cmd();
                    cmd.setDepthTestEnable(false);
                    cmd.setDepthWriteEnable(false);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, 0u); // nothing written: the output is the atomics
                    if (meshes)
                    {
                        MeshletDrawCursor cursor = beginMeshletDraws(context, res->CameraDraws);
                        cursor.boundPipeline = nullptr;
                        // Never the refraction source here (the cursor's slot stays none): glass refracting the scene covers it fully in
                        // the shading pass (it shows the opaque scene behind it as its own color), but what it lets through of the
                        // transparent surfaces behind it is its tinted transmission -- splatted as "covers fully", every glass behind
                        // the nearest got no weight and vanished.
                        auto drawBuckets = [&](MeshletDrawCursor& drawCursor)
                        {
                            drawMeshletBucket(cmd, drawCursor, RenderBucket::Transparent, *litPipeline, NRI_CULL_MODE_BACK, false, false);
                            drawMeshletBucket(cmd, drawCursor, RenderBucket::TransparentDoubleSided, *litPipeline, NRI_CULL_MODE_NONE, false, false);
                            drawMeshletBucket(cmd, drawCursor, RenderBucket::TransparentUnlit, *unlitPipeline, NRI_CULL_MODE_BACK, false, false);
                            drawMeshletBucket(cmd, drawCursor, RenderBucket::TransparentUnlitDoubleSided, *unlitPipeline, NRI_CULL_MODE_NONE, false, false);
                        };
                        drawBuckets(cursor);
                        MeshletDrawCursor lateCursor = beginMeshletDraws(context, res->CameraDraws, true);
                        lateCursor.boundPipeline = cursor.boundPipeline;
                        drawBuckets(lateCursor);
                        cmd.setDepthTestEnable(false);
                        cmd.setDepthWriteEnable(false);
                        cmd.setColorBlendEnable(0, false);
                        cmd.setColorWriteMask(0, 0u); // nothing written: the output is the atomics
                    }
                    if (!particleBatches.empty())
                    {
                        cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, *particlePipeline);
                        cmd.setCullMode(NRI_CULL_MODE_NONE);
                        const uint32_t depthSlot = context.Slot(res->Depth);
                        for (const ParticleBatch& batch : particleBatches)
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
                            push.flags = batch.Settings.Soft ? shaderio::PARTICLE_SOFT : 0u; // (only its coverage counts here: not lit)
                            push.depthTextureIndex = depthSlot;
                            push.softDistance = batch.Settings.SoftDistance;
                            cmd.pushData(&push, sizeof(push));
                            if (batch.GpuIndirectBuffer)
                                nri::drawMeshTasksIndirect(cmd, *batch.GpuIndirectBuffer, 0, 1, 12u);
                            else
                                nri::drawMeshTasks(cmd, (batch.Count + 31) / 32, 1, 1);
                        }
                    }
                    cmd.setColorWriteMask(0, nri::ColorComponent::All);
                    cmd.setDepthWriteEnable(true);
                });
        };

        addPrepass("AVBOIT Occupancy", &m_transparentLitAVBOITOccupancyPipeline, &m_unlitAVBOITOccupancyPipeline, &m_particleAVBOITOccupancyPipeline);

        m_renderGraph.AddPass("AVBOIT Depth Warp", RGPassFlags::NeverCull,
            [](RGBuilder&) {},
            [this, uniforms](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.executionBarrier(); // the occupancy is complete
                shaderio::PushConstantAVBOIT push{};
                push.matrixReference = uniforms;
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_avboitLutPipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch(1, 1, 1);
                cmd.executionBarrier(); // the splat reads it
            });

        addPrepass("AVBOIT Splat", &m_transparentLitAVBOITSplatPipeline, &m_unlitAVBOITSplatPipeline, &m_particleAVBOITSplatPipeline);

        const uint32_t storage = m_avboitTransmittanceStorage;
        m_renderGraph.AddPass("AVBOIT Integrate", RGPassFlags::NeverCull,
            [](RGBuilder&) {},
            [this, uniforms, storage, grid](RGPassContext& context)
            {
                nri::CommandBuffer& cmd = context.Cmd();
                cmd.executionBarrier(); // every splat is in
                shaderio::PushConstantAVBOIT push{};
                push.matrixReference = uniforms;
                push.transmittanceStorageIndex = storage;
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_avboitIntegratePipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch((grid.x + 7) / 8, (grid.y + 7) / 8, 1);
                cmd.executionBarrier(); // the transparent shading samples the volume (through the uniforms)
            });
    }
}
