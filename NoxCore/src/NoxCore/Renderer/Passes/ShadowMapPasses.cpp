// The sun's cascaded shadow maps (RS1, docs/Backlog.md "Raster versions of the RT features"; Settings > Sun Shadows: Shadow Maps):
// the raster version of the RT shadow mask, measured against it. UE5's directional light without virtual shadow maps:
//   - Stable cascades: the camera's view up to the shadow distance split by the cascade distribution exponent, each split's bounding
//     sphere as the cascade (its size does not change as the camera turns), its position snapped to whole texels in the light's
//     space -- against the world origin, not the render origin, so the edges stand still through an origin rebase too.
//   - Orthographic, reverse Z (1 towards the sun) like the cameras; casters in front of the near plane flattened onto it (depth
//     clamp, "pancaking") so the near plane stays close and the depth precise. Slope-scaled raster bias on the casters.
//   - Per cascade the camera's GPU culling (frustum only) and the visibility pass's meshlet draws into a depth target: opaque and
//     masked surfaces (alpha tested), unlit ones too, both faces -- what the RT shadow rays hit (ray_shadow_passes skips the
//     transparent ones). The cluster LOD stays the camera's, so a surface shadows the same triangles it is drawn with.
//   - The resolve (ShadowMapResolve.slang) writes the RT shadow mask's format into RawShadowMask; the deferred lighting reads it as
//     it reads the ray traced one. The forward and particle passes look the cascades up themselves (ShadowMaps.slang).
#include "NoxCore/Renderer/Renderer.h"

#include <algorithm>

#include "FrameGraphResources.h"

namespace Nox
{
    namespace
    {
        // Graph names must outlive the graph: a literal per cascade.
        constexpr const char* kCullNames[shaderio::MaxShadowCascades] = { "Sun Shadow Cascade 0 Culling", "Sun Shadow Cascade 1 Culling", "Sun Shadow Cascade 2 Culling", "Sun Shadow Cascade 3 Culling" };
        constexpr const char* kDepthNames[shaderio::MaxShadowCascades] = { "Sun Shadow Cascade 0", "Sun Shadow Cascade 1", "Sun Shadow Cascade 2", "Sun Shadow Cascade 3" };
        constexpr const char* kMapNames[shaderio::MaxShadowCascades] = { "Sun Shadow Map 0", "Sun Shadow Map 1", "Sun Shadow Map 2", "Sun Shadow Map 3" };

        // Reverse-Z orthographic projection of the light's view box: x / y within +-halfSize, view z (towards the scene) from 0 (depth 1)
        // to depthRange (depth 0) -- EditorCamera's orthographic projection.
        glm::mat4 ReverseZOrthographic(float halfSize, float depthRange)
        {
            return glm::mat4(1.0f / halfSize, 0.0f, 0.0f, 0.0f,
                             0.0f, 1.0f / halfSize, 0.0f, 0.0f,
                             0.0f, 0.0f, -1.0f / depthRange, 0.0f,
                             0.0f, 0.0f, 1.0f, 1.0f);
        }
    }

    void Renderer::createShadowMapPipelines(bool forceCompile)
    {
        // Depth only: the visibility pass's task and mesh stages (the cascade's view from its copy of the uniforms).
        NRI::PipelineDesc depth{};
        depth.forceCompile = forceCompile;
        depth.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/Material_PBR_MeshTask.slang" });
        depth.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = "assets/shaders/VisibilityBuffer.slang" });
        depth.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "depthFragMain", .sourcePath = "assets/shaders/VisibilityBuffer.slang" });
        m_shadowDepthPipeline = m_device->createPipeline(depth, *m_shaderCompiler);
        depth.shaders.back().entryPoint = "depthMaskFragMain";
        m_shadowDepthMaskPipeline = m_device->createPipeline(depth, *m_shaderCompiler);

        NRI::PipelineDesc resolve{};
        resolve.forceCompile = forceCompile;
        resolve.colorFormats = { NRI::ImageFormat::R16G16_SFLOAT }; // RawShadowMask
        resolve.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/ShadowMapResolve.slang" });
        resolve.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = "assets/shaders/ShadowMapResolve.slang" });
        resolve.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain", .sourcePath = "assets/shaders/ShadowMapResolve.slang" });
        m_shadowMapResolvePipeline = m_device->createPipeline(resolve, *m_shaderCompiler);
    }

    void Renderer::addShadowMapPasses()
    {
        const bool pathTracing = m_pathTracingEnabled || m_debugMode == 18 || m_debugMode == 19;
        const uint32_t entryCount = static_cast<uint32_t>(m_drawList.size());
        const bool cullingReady = std::all_of(m_instanceCullingPipelines.begin(), m_instanceCullingPipelines.end(), [](const auto& pipeline) { return pipeline != nullptr; });
        if (m_sunShadowMode != SunShadowMode::ShadowMaps || pathTracing || m_shadowMaskLightIndex == ~0u ||
            m_shadowMaskLightIndex >= m_lightBufferObjects.size() || entryCount == 0 || !cullingReady ||
            !m_shadowDepthPipeline || !m_shadowDepthMaskPipeline || !m_shadowMapResolvePipeline)
            return;

        // Per frame slot: each cascade's uniforms and culling view, the cascade table.
        if (!m_shadowCascadeBuffers[frameIndex])
        {
            for (uint32_t cascade = 0; cascade < shaderio::MaxShadowCascades; ++cascade)
            {
                m_shadowCascadeUniforms[frameIndex][cascade] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::UniformBufferObject), .usage = NRI::BufferUsage::Uniform });
                m_shadowCascadeUniformsMapped[frameIndex][cascade] = m_shadowCascadeUniforms[frameIndex][cascade]->map(0, sizeof(shaderio::UniformBufferObject));
                m_shadowCascadeCullViews[frameIndex][cascade] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::CullView), .usage = NRI::BufferUsage::Storage });
            }
            m_shadowCascadeBuffers[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::GpuShadowCascades), .usage = NRI::BufferUsage::Storage });
            m_shadowCascadeBuffersMapped[frameIndex] = m_shadowCascadeBuffers[frameIndex]->map(0, sizeof(shaderio::GpuShadowCascades));
        }

        // The camera (render space): position and axes from its view (Nox views look down +z), its projection's extents.
        const glm::mat4 invView = glm::inverse(uniformData.view);
        const glm::vec3 cameraPosition(invView[3]);
        const glm::vec3 cameraForward = glm::normalize(glm::vec3(invView[2]));
        const glm::mat4& proj = uniformData.nonJitteredProj;
        const bool orthographic = proj[3][3] != 0.0f;
        const float halfX = 1.0f / proj[0][0]; // perspective: per unit of distance
        const float halfY = 1.0f / proj[1][1];
        const float lateralSq = halfX * halfX + halfY * halfY;
        float nearDistance;
        float farDistance;
        if (orthographic)
        {
            // An orthographic view has no perspective to spend texels on: its whole depth range (EditorCamera's orthographic projection).
            const float range = -1.0f / proj[2][2];
            farDistance = proj[3][2] * range;
            nearDistance = farDistance - range;
        }
        else
        {
            nearDistance = proj[3][2]; // the infinite reverse-Z projection's near plane
            farDistance = nearDistance + WorldUnits::FromMeters(m_shadowDistanceMeters);
        }

        // The sun: from it into the scene is the light view's +z (the cameras' convention: a flipped right-handed look-at).
        const shaderio::LightData& sun = m_lightBufferObjects[m_shadowMaskLightIndex];
        const glm::vec3 towardsSun = glm::normalize(glm::vec3(sun.direction));
        const glm::vec3 lightForward = -towardsSun;
        const glm::vec3 lightUp = std::abs(lightForward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 lightRotation = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * glm::lookAt(glm::vec3(0.0f), lightForward, lightUp);
        const glm::dmat3 lightRotationD = glm::dmat3(glm::mat3(lightRotation));
        const glm::dvec3 originInLight = lightRotationD * m_renderOrigin;

        const uint32_t count = std::clamp(m_shadowCascadeCount, 1u, shaderio::MaxShadowCascades);
        const uint32_t resolution = m_shadowMapResolution;
        const float exponent = m_shadowCascadeExponent;
        const float exponentTotal = std::pow(exponent, static_cast<float>(count)) - 1.0f;
        // Casters this far towards the sun beyond the cascade keep their depth (farther ones flatten onto the near plane).
        const float casterReach = WorldUnits::FromMeters(100.0f);

        shaderio::GpuShadowCascades& cascades = m_shadowCascades;
        cascades = {};
        cascades.count = count;
        cascades.filter = m_shadowFilter;
        cascades.resolution = resolution;
        cascades.normalBias = m_shadowNormalBias;
        cascades.depthBias = m_shadowDepthBias;
        cascades.blendFraction = m_shadowCascadeBlend;
        cascades.filterTexels = m_shadowFilterTexels;
        cascades.maxFilterTexels = std::max(m_shadowMaxFilterTexels, m_shadowFilterTexels);
        cascades.lightDirection = glm::vec4(towardsSun, std::tan(std::max(sun.spotParams.z, 0.0f)));

        std::array<float, shaderio::MaxShadowCascades> slopeBiasClamp{};
        float splitNear = nearDistance;
        for (uint32_t cascade = 0; cascade < count; ++cascade)
        {
            // UE's cascade distribution: each cascade `exponent` times as long as the one before.
            const float t = exponent > 1.0001f ? (std::pow(exponent, static_cast<float>(cascade + 1)) - 1.0f) / exponentTotal
                                               : static_cast<float>(cascade + 1) / static_cast<float>(count);
            const float splitFar = nearDistance + (farDistance - nearDistance) * t;

            // The smallest sphere around the slice's corners: on the view axis, from the slice's depths and the view's extents alone.
            float centerDistance;
            float radius;
            if (orthographic)
            {
                centerDistance = 0.5f * (splitNear + splitFar);
                radius = std::sqrt(0.25f * (splitFar - splitNear) * (splitFar - splitNear) + lateralSq);
            }
            else
            {
                centerDistance = 0.5f * (splitNear + splitFar) * (1.0f + lateralSq);
                if (centerDistance >= splitFar)
                {
                    centerDistance = splitFar;
                    radius = splitFar * std::sqrt(lateralSq);
                }
                else
                {
                    radius = std::sqrt((splitFar - centerDistance) * (splitFar - centerDistance) + splitFar * splitFar * lateralSq);
                }
            }
            const float texelWorld = 2.0f * radius / static_cast<float>(resolution);

            // Whole texels in the light's space, counted from the world origin.
            const glm::vec3 center = cameraPosition + cameraForward * centerDistance;
            glm::dvec3 centerInLight = lightRotationD * (m_renderOrigin + glm::dvec3(center));
            centerInLight.x = std::floor(centerInLight.x / texelWorld) * texelWorld;
            centerInLight.y = std::floor(centerInLight.y / texelWorld) * texelWorld;
            const glm::vec3 snapped = glm::vec3(centerInLight - originInLight);

            // The sphere from depth casterReach to casterReach + its diameter.
            const float depthRange = 2.0f * radius + casterReach;
            const glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(-snapped.x, -snapped.y, -snapped.z + radius + casterReach)) * lightRotation;
            const glm::mat4 projection = ReverseZOrthographic(radius, depthRange);
            m_shadowCascadeViews[cascade] = view;
            m_shadowCascadeProjections[cascade] = projection;

            cascades.viewProj[cascade] = projection * view;
            cascades.splitFar[cascade] = splitFar;
            cascades.texelWorldSize[cascade] = texelWorld;
            cascades.depthRangeWorld[cascade] = depthRange;
            // A caster's slope bias stays under a few texels' depth (edge-on triangles would otherwise float away from their shadow).
            slopeBiasClamp[cascade] = -4.0f * texelWorld / depthRange;
            splitNear = splitFar;
        }

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_frame.shadowMapsAdded = true;
        m_frame.shadowCascadeCount = count;

        RGTextureDesc mapDesc;
        mapDesc.Size = RGSize::Absolute;
        mapDesc.Width = resolution;
        mapDesc.Height = resolution;
        mapDesc.Format = NRI::ImageFormat::None; // the device's depth format
        mapDesc.Usage = NRI::TextureUsage::DepthStencilAttachment;
        const NRI::Extent2D mapExtent{ resolution, resolution };
        const uint32_t blockCount = (entryCount + shaderio::CULL_BLOCK_SIZE - 1) / shaderio::CULL_BLOCK_SIZE;
        const float slopeBias = m_shadowSlopeBias;

        for (uint32_t cascade = 0; cascade < count; ++cascade)
        {
            NRI::Buffer* uniformBuffer = m_shadowCascadeUniforms[frameIndex][cascade].get();
            NRI::Buffer* cullViewBuffer = m_shadowCascadeCullViews[frameIndex][cascade].get();

            // What the cascade sees of the draw list: its side planes only (the culling has no near or far plane), so casters between
            // it and the sun stay.
            shaderio::CullView cull{};
            cull.frustum = shaderio::Frustum{ cascades.viewProj[cascade] };
            cull.viewProj = cascades.viewProj[cascade];
            cull.previousViewProj = cascades.viewProj[cascade];
            std::copy(m_drawBucketStarts.begin(), m_drawBucketStarts.end(), cull.bucketStart);
            cull.entryCount = entryCount;
            cull.blockCount = blockCount;
            cull.flags = m_instanceCullingEnabled ? shaderio::CULL_INSTANCES : 0u;
            memcpy(cullViewBuffer->map(0, sizeof(cull)), &cull, sizeof(cull));
            cullViewBuffer->unmap();

            ViewDrawResources draws;
            draws.VisibleInstances = m_renderGraph.CreateBuffer("Sun Shadow Visible Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
            draws.Commands = m_renderGraph.CreateBuffer("Sun Shadow Draw Commands", { sizeof(shaderio::MeshTasksIndirectCommand) * entryCount, NRI::BufferUsage::Indirect });
            draws.Counts = m_renderGraph.CreateBuffer("Sun Shadow Draw Counts", { sizeof(shaderio::CullCounts), NRI::BufferUsage::Indirect });
            draws.LateInstances = m_renderGraph.CreateBuffer("Sun Shadow Late Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
            const RGBuffer flags = m_renderGraph.CreateBuffer("Sun Shadow Cull Flags", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
            const RGBuffer blocks = m_renderGraph.CreateBuffer("Sun Shadow Cull Blocks", { sizeof(shaderio::CullBlock) * blockCount, NRI::BufferUsage::Storage });
            addCullingDispatchPass(kCullNames[cascade], draws, flags, blocks, entryCount, RGTexture{}, uniformBuffer, cullViewBuffer);

            const RGTexture map = m_renderGraph.CreateTexture(kMapNames[cascade], mapDesc);
            resources.SunShadowMaps[cascade] = map;
            m_renderGraph.AddPass(kDepthNames[cascade], RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.DepthTarget(map, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0 }); // reverse Z
                    builder.SetRenderArea(mapExtent);
                    ReadGpuScene(builder, resources);
                    ReadIfValid(builder, resources.SkinnedVertices);
                    ReadViewDraws(builder, draws);
                },
                [this, draws, uniformBuffer, slopeBias, biasClamp = slopeBiasClamp[cascade]](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    MeshletDrawCursor cursor = beginMeshletDraws(context, draws);
                    cursor.references.matrixReference = uniformBuffer->getDeviceAddress();
                    // ClusterStats count the camera's clusters only.
                    cursor.taskFlags &= ~shaderio::MESHLET_COUNT_TRIANGLES;

                    cmd.setDepthTestEnable(true);
                    cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                    cmd.setDepthClampEnable(true);
                    // Away from the sun is towards depth 0: a negative slope bias, clamped.
                    cmd.setDepthBiasEnable(slopeBias > 0.0f);
                    cmd.setDepthBias(0.0f, biasClamp, -slopeBias);

                    // Both faces of everything the RT shadow rays hit (ShadowMask.slang): opaque, masked (alpha tested), unlit.
                    drawMeshletBucket(cmd, cursor, RenderBucket::Opaque, *m_shadowDepthPipeline, NRI::CullMode::None, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::OpaqueDoubleSided, *m_shadowDepthPipeline, NRI::CullMode::None, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::Mask, *m_shadowDepthMaskPipeline, NRI::CullMode::None, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::MaskDoubleSided, *m_shadowDepthMaskPipeline, NRI::CullMode::None, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::Unlit, *m_shadowDepthMaskPipeline, NRI::CullMode::None, true, false);
                    drawMeshletBucket(cmd, cursor, RenderBucket::UnlitDoubleSided, *m_shadowDepthMaskPipeline, NRI::CullMode::None, true, false);

                    cmd.setDepthBiasEnable(false);
                    cmd.setDepthClampEnable(false);
                });
        }

        // The cascades into the shadow mask (the RT shadow mask's format; NRD does not run on it).
        m_renderGraph.AddPass("Sun Shadow Map Resolve", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferMaterial);
                ReadSunShadowMaps(builder, resources);
                builder.ColorTarget(resources.RawShadowMask, NRI::LoadOP::clear, NRI::StoreOP::store, { 65504.0f, 1.0f, 0.0f, 0.0f });
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_shadowMapResolvePipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantShadowMapResolve push{};
                push.invViewProj = uniformData.invViewProj;
                push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                push.cascadesReference = m_shadowCascadeBuffers[frameIndex]->getDeviceAddress();
                push.depthTextureIndex = context.Slot(res->Depth);
                push.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                push.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                push.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                push.viewportSize = glm::vec2(static_cast<float>(m_frame.renderExtent.width), static_cast<float>(m_frame.renderExtent.height));
                cmd.pushData(&push, sizeof(push));
                cmd.drawMeshTasks(1, 1, 1);
            });
    }

    void Renderer::updateShadowMapUniforms()
    {
        uniformData.sunShadowCascadesReference = 0;
        if (!m_frame.shadowMapsAdded)
            return;

        // Each cascade's copy of the frame's uniforms with its view: what the culling, task and mesh stages read. The cluster LOD keeps
        // the camera's (frozen* camera position and projection, lod*), the meshlet frustum test the cascade's.
        for (uint32_t cascade = 0; cascade < m_frame.shadowCascadeCount; ++cascade)
        {
            shaderio::UniformBufferObject uniforms = uniformData;
            const glm::mat4& view = m_shadowCascadeViews[cascade];
            const glm::mat4& projection = m_shadowCascadeProjections[cascade];
            uniforms.view = uniforms.prevView = view;
            uniforms.proj = uniforms.nonJitteredProj = uniforms.prevProj = projection;
            uniforms.invViewProj = glm::inverse(projection * view);
            uniforms.frustum = uniforms.frozenFrustum = shaderio::Frustum{ projection * view };
            uniforms.jitterOffset = glm::vec2(0.0f);
            uniforms.prevJitterOffset = glm::vec2(0.0f);
            uniforms.clusterStatsReference = 0;
            uniforms.sunShadowCascadesReference = 0;
            memcpy(m_shadowCascadeUniformsMapped[frameIndex][cascade], &uniforms, sizeof(shaderio::UniformBufferObject));
        }

        // The maps' slots (graph-owned textures: known after Compile). Culled (nothing read the mask), the cascades are not looked up.
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        constexpr uint32_t NoTexture = 0xFFFFFFFF;
        for (uint32_t cascade = 0; cascade < m_frame.shadowCascadeCount; ++cascade)
        {
            m_shadowCascades.textureIndex[cascade] = m_renderGraph.GetSlotOr(resources.SunShadowMaps[cascade], NoTexture);
            if (m_shadowCascades.textureIndex[cascade] == NoTexture)
                return;
        }
        memcpy(m_shadowCascadeBuffersMapped[frameIndex], &m_shadowCascades, sizeof(shaderio::GpuShadowCascades));
        uniformData.sunShadowCascadesReference = m_shadowCascadeBuffers[frameIndex]->getDeviceAddress();
    }
}
