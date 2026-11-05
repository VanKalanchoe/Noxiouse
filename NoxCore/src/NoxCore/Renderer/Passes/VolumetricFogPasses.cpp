// Volumetric fog (RS6, docs/Backlog_2026.md "Raster versions of the RT features"; Settings > Volumetric Fog): UE5's volumetric fog on
// the exponential height fog (VolumetricFog.slang). A froxel grid (8 x 8 pixels a cell, `slices` along the view ray out to its
// distance) lit per froxel -- the sun with its shadow maps (or a ray with RT shadows), point / spot lights with their atlas shadow maps,
// the sky around -- at a depth that moves every frame, blended with last frame's, then integrated along each column. HeightFog.slang
// applies it in front of the height fog (which then starts where the volume ends) for the lit scene, the sky and the forward passes.
// The textures are the renderer's own (2D arrays: the graph has no array textures): a full barrier after each step.
#include "NoxCore/Renderer/Renderer.h"

#include "FrameGraphResources.h"

namespace Nox
{
    namespace
    {
        constexpr uint32_t FogTileSize = 8; // pixels per froxel side
    }

    void Renderer::createVolumetricFogPipelines(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.type = NRI::PipelineType::Compute;
        desc.forceCompile = forceCompile;
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Compute, .entryPoint = "lightMain", .sourcePath = "assets/shaders/VolumetricFog.slang" });
        m_volumetricFogLightPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        desc.shaders.back().entryPoint = "integrateMain";
        m_volumetricFogIntegratePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::addVolumetricFogPass()
    {
        uniformData.volumetricFogTextureIndex = 0xFFFFFFFF;
        uniformData.volumetricFogSlices = 0;
        uniformData.volumetricFogDistance = 0.0f;
        const bool pathTracing = m_pathTracingEnabled || m_debugMode == 18 || m_debugMode == 19;
        if (!m_volumetricFog || pathTracing || uniformData.heightFogDensity <= 0.0f || !m_volumetricFogLightPipeline || !m_volumetricFogIntegratePipeline)
        {
            m_fogHistoryValid = false;
            return;
        }

        // The grid for this render size; new textures when it changes (the history starts over).
        const glm::uvec3 grid((m_frame.renderExtent.width + FogTileSize - 1) / FogTileSize, (m_frame.renderExtent.height + FogTileSize - 1) / FogTileSize,
                              m_volumetricFogSlices);
        if (grid != m_fogGrid || !m_fogIntegrated)
        {
            auto release = [this](Ref<Texture2D>& texture, uint32_t& storage)
            {
                if (storage != ~0u)
                    m_resourceHeap->unregisterTexture(storage);
                storage = ~0u;
                if (texture)
                    deferAssetRelease(texture);
                texture = nullptr;
            };
            auto create = [this, grid](Ref<Texture2D>& texture, uint32_t& storage)
            {
                texture = m_device->createTexture(NRI::TextureDesc{
                    .width = grid.x,
                    .height = grid.y,
                    .arrayLayers = grid.z,
                    .isCubeMap = false,
                    .mipLevels = 1,
                    .sampleCount = 1,
                    .usage = NRI::TextureUsage::Storage,
                    .format = NRI::ImageFormat::R16G16B16A16_SFLOAT
                });
                m_resourceHeap->registerTexture(*texture, NRI::TextureUsage::ShaderResource);
                storage = m_resourceHeap->registerStorageTextureMip(*texture, 0);
            };
            for (uint32_t i = 0; i < 2; ++i)
            {
                release(m_fogLighting[i], m_fogLightingStorage[i]);
                create(m_fogLighting[i], m_fogLightingStorage[i]);
            }
            release(m_fogIntegrated, m_fogIntegratedStorage);
            create(m_fogIntegrated, m_fogIntegratedStorage);
            m_fogGrid = grid;
            m_fogHistoryValid = false;
        }

        const uint32_t writeIndex = 1 - m_fogHistoryIndex;
        shaderio::PushConstantVolumetricFog push{};
        push.invViewProj = uniformData.invViewProj;
        push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
        push.lightingStorageIndex = m_fogLightingStorage[writeIndex];
        push.historyTextureIndex = m_fogHistoryValid ? uniformData.imageHeapIndexOffset + m_fogLighting[m_fogHistoryIndex]->GetDescriptorIndexSlot() : 0xFFFFFFFF;
        push.integratedStorageIndex = m_fogIntegratedStorage;
        push.gridWidth = grid.x;
        push.gridHeight = grid.y;
        push.slices = grid.z;
        push.maxDistance = WorldUnits::FromMeters(m_volumetricFogDistanceMeters);
        push.anisotropy = m_volumetricFogAnisotropy;
        push.extinctionScale = m_volumetricFogExtinctionScale;
        push.historyWeight = 0.9f;
        push.previousCameraPos = glm::vec3(glm::inverse(uniformData.prevView)[3]);
        // A golden-ratio sequence of depths inside the froxels.
        push.jitter = static_cast<float>(std::fmod(static_cast<double>(m_sceneFrameCounter) * 0.6180339887498949, 1.0));
        push.albedo = m_volumetricFogAlbedo;
        push.localLights = m_volumetricFogLocalLights ? 1u : 0u;
        m_fogHistoryIndex = writeIndex;
        m_fogHistoryValid = true;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_renderGraph.AddPass("Volumetric Fog", RGPassFlags::NeverCull,
            [&](RGBuilder& builder)
            {
                ReadShadowMaps(builder, resources); // the sun's cascades and the local lights' atlas, when they run
                if (resources.TLAS.IsValid())
                    builder.Read(resources.TLAS, RGBufferAccess::AccelerationStructureRead);
            },
            [this, push, grid](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_volumetricFogLightPipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch((grid.x + 7) / 8, (grid.y + 7) / 8, grid.z);
                cmd.executionBarrier();
                cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_volumetricFogIntegratePipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch((grid.x + 7) / 8, (grid.y + 7) / 8, 1);
                // Read through the uniforms by the lighting, the sky and the forward passes later this frame.
                cmd.executionBarrier();
            });

        uniformData.volumetricFogTextureIndex = m_fogIntegrated->GetDescriptorIndexSlot();
        uniformData.volumetricFogSlices = grid.z;
        uniformData.volumetricFogDistance = push.maxDistance;
    }
}
