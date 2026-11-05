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
#include <bit>

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
                ReadShadowMaps(builder, resources);
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
        // Contact shadows (the lighting applies them where a shadow map shadowed).
        uniformData.contactShadowLength = m_contactShadows ? WorldUnits::FromMeters(m_contactShadowLengthMeters) : 0.0f;
        uniformData.contactShadowSamples = m_contactShadowSamples;
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
    namespace
    {
        // The cameras' infinite reverse-Z perspective (depth = near / view depth), square.
        glm::mat4 ReverseZPerspective(float tanHalfFov, float nearPlane)
        {
            const float f = 1.0f / tanHalfFov;
            return glm::mat4(f, 0.0f, 0.0f, 0.0f,
                             0.0f, f, 0.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, 1.0f,
                             0.0f, 0.0f, nearPlane, 0.0f);
        }

        // Every other bit of a Morton code: a cell's x (or, shifted, y).
        uint32_t MortonCompact(uint32_t code)
        {
            code &= 0x55555555u;
            code = (code | (code >> 1)) & 0x33333333u;
            code = (code | (code >> 2)) & 0x0F0F0F0Fu;
            code = (code | (code >> 4)) & 0x00FF00FFu;
            code = (code | (code >> 8)) & 0x0000FFFFu;
            return code;
        }

        // A cube's faces (+X, -X, +Y, -Y, +Z, -Z, GpuShadowView order) and an up vector for each.
        constexpr glm::vec3 kCubeDirections[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        constexpr glm::vec3 kCubeUps[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 1, 0 } };
        constexpr const char* kLocalCullNames[Renderer::MaxLocalShadowLights] = {
            "Local Shadow 0 Culling", "Local Shadow 1 Culling", "Local Shadow 2 Culling", "Local Shadow 3 Culling",
            "Local Shadow 4 Culling", "Local Shadow 5 Culling", "Local Shadow 6 Culling", "Local Shadow 7 Culling",
            "Local Shadow 8 Culling", "Local Shadow 9 Culling", "Local Shadow 10 Culling", "Local Shadow 11 Culling",
            "Local Shadow 12 Culling", "Local Shadow 13 Culling", "Local Shadow 14 Culling", "Local Shadow 15 Culling" };
        constexpr uint32_t kMinLocalTile = 128;
    }

    void Renderer::addLocalShadowPasses()
    {
        m_localShadowLightCount = 0;
        m_localShadows.viewCount = 0;
        const bool pathTracing = m_pathTracingEnabled || m_debugMode == 18 || m_debugMode == 19;
        const uint32_t entryCount = static_cast<uint32_t>(m_drawList.size());
        const bool cullingReady = std::all_of(m_instanceCullingPipelines.begin(), m_instanceCullingPipelines.end(), [](const auto& pipeline) { return pipeline != nullptr; });
        if (m_localShadowMode != LocalShadowMode::ShadowMaps || pathTracing || entryCount == 0 || !cullingReady ||
            !m_shadowDepthPipeline || !m_shadowDepthMaskPipeline)
            return;

        // The candidates: local lights whose reach the camera sees, by how much light they could bring to it.
        struct Candidate
        {
            uint32_t LightIndex;
            float Priority;
            uint32_t Tile;
        };
        std::vector<Candidate> candidates;
        const glm::mat4 invView = glm::inverse(uniformData.view);
        const glm::vec3 cameraPosition(invView[3]);
        const float projScale = uniformData.nonJitteredProj[1][1] * 0.5f * static_cast<float>(m_frame.renderExtent.height);
        const float minDistance = WorldUnits::FromMeters(1.0f);
        const uint32_t maxTile = std::max(std::bit_floor(m_localShadowMaxResolution), kMinLocalTile);
        for (uint32_t index = 0; index < static_cast<uint32_t>(m_lightBufferObjects.size()); ++index)
        {
            const shaderio::LightData& light = m_lightBufferObjects[index];
            const uint32_t type = static_cast<uint32_t>(light.position.w);
            if (type == static_cast<uint32_t>(shaderio::LightType::Directional) || light.color.w <= 0.0f)
                continue;
            const glm::vec3 position(light.position);
            const float range = light.direction.w; // 0: infinite
            if (range > 0.0f)
            {
                bool visible = true;
                for (uint32_t plane = 0; plane < 4 && visible; ++plane)
                    visible = glm::dot(glm::vec3(uniformData.frustum.planes[plane]), position) + uniformData.frustum.planes[plane].w >= -range;
                if (!visible)
                    continue;
            }
            const bool area = type == static_cast<uint32_t>(shaderio::LightType::Rect) || type == static_cast<uint32_t>(shaderio::LightType::Disc);
            float power = glm::dot(glm::vec3(light.color), glm::vec3(0.2126f, 0.7152f, 0.0722f)) * light.color.w;
            if (area)
                power *= 4.0f * glm::length(glm::vec3(light.spotParams)) * light.spotParams.w; // radiance x area
            const float distance = glm::length(position - cameraPosition);
            const float priority = power / std::max(distance * distance, minDistance * minDistance);
            // Texels for its reach on screen (the camera inside it, or an infinite range: the largest).
            uint32_t tile = maxTile;
            if (range > 0.0f && distance > range)
            {
                const float screenRadius = range / distance * projScale;
                tile = std::clamp(std::bit_ceil(static_cast<uint32_t>(std::max(screenRadius, 1.0f))), kMinLocalTile, maxTile);
            }
            candidates.push_back({ index, priority, tile });
        }
        if (candidates.empty())
            return;
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.Priority > b.Priority; });
        if (candidates.size() > m_localShadowMaxLights)
            candidates.resize(m_localShadowMaxLights);

        // Which views each light needs: a spot light its cone; the others a cube, a rect / disc light without the faces wholly behind it.
        auto faceUsed = [&](const shaderio::LightData& light, uint32_t face)
        {
            const uint32_t type = static_cast<uint32_t>(light.position.w);
            if (type != static_cast<uint32_t>(shaderio::LightType::Rect) && type != static_cast<uint32_t>(shaderio::LightType::Disc))
                return true;
            const glm::vec3 front(light.direction);
            const glm::vec3 direction = kCubeDirections[face];
            const glm::vec3 up = kCubeUps[face];
            const glm::vec3 right = glm::cross(direction, up);
            // The face's widest direction towards the front (its corner rays: direction +- right +- up).
            return glm::dot(direction, front) + std::abs(glm::dot(right, front)) + std::abs(glm::dot(up, front)) > 0.0f;
        };
        auto viewCount = [&](const Candidate& candidate)
        {
            const shaderio::LightData& light = m_lightBufferObjects[candidate.LightIndex];
            if (static_cast<uint32_t>(light.position.w) == static_cast<uint32_t>(shaderio::LightType::Spot))
                return 1u;
            uint32_t faces = 0;
            for (uint32_t face = 0; face < 6; ++face)
                faces += faceUsed(light, face) ? 1u : 0u;
            return faces;
        };

        // Fit the atlas (cells of the smallest tile): halve the largest tiles until everything fits; at the smallest, drop the least
        // important lights.
        const uint32_t atlasSize = std::bit_floor(m_localShadowAtlasSize);
        const uint32_t cellsPerSide = atlasSize / kMinLocalTile;
        auto cellsNeeded = [&]()
        {
            uint64_t cells = 0;
            for (const Candidate& candidate : candidates)
            {
                const uint64_t side = candidate.Tile / kMinLocalTile;
                cells += side * side * viewCount(candidate);
            }
            return cells;
        };
        while (!candidates.empty() && cellsNeeded() > static_cast<uint64_t>(cellsPerSide) * cellsPerSide)
        {
            auto largest = std::max_element(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.Tile < b.Tile; });
            if (largest->Tile > kMinLocalTile)
                largest->Tile /= 2;
            else
                candidates.pop_back();
        }
        if (candidates.empty())
            return;

        // Per frame slot: the GpuLocalShadows buffer, the per-light view table, the culling views (each view's uniforms on demand).
        if (!m_localShadowBuffers[frameIndex])
        {
            m_localShadowBuffers[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::GpuLocalShadows), .usage = NRI::BufferUsage::Storage });
            m_localShadowBuffersMapped[frameIndex] = m_localShadowBuffers[frameIndex]->map(0, sizeof(shaderio::GpuLocalShadows));
            for (uint32_t light = 0; light < MaxLocalShadowLights; ++light)
                m_localShadowCullViews[frameIndex][light] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::CullView), .usage = NRI::BufferUsage::Storage });
        }
        const uint32_t lightCount = static_cast<uint32_t>(m_lightBufferObjects.size());
        if (m_localShadowLightViewCapacity[frameIndex] < lightCount)
        {
            if (m_localShadowLightViewBuffers[frameIndex])
            {
                std::scoped_lock lock(m_deferredReleasesMutex);
                m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT, std::move(m_localShadowLightViewBuffers[frameIndex]) });
            }
            const uint32_t capacity = std::max(lightCount * 2, 16u);
            m_localShadowLightViewBuffers[frameIndex] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(uint32_t) * capacity, .usage = NRI::BufferUsage::Storage });
            m_localShadowLightViewBuffersMapped[frameIndex] = m_localShadowLightViewBuffers[frameIndex]->map(0, sizeof(uint32_t) * capacity);
            m_localShadowLightViewCapacity[frameIndex] = capacity;
        }

        // The views: matrices, tiles placed in Morton order from the largest (each then lands aligned to its own size).
        struct PendingView
        {
            uint32_t View;
            uint32_t Tile;
        };
        std::vector<PendingView> pending;
        shaderio::GpuLocalShadows& shadows = m_localShadows;
        shadows = {};
        shadows.atlasSize = atlasSize;
        shadows.filter = m_shadowFilter;
        shadows.normalBias = m_shadowNormalBias;
        shadows.depthBias = m_shadowDepthBias;
        shadows.filterTexels = m_shadowFilterTexels;
        shadows.maxFilterTexels = std::max(m_shadowMaxFilterTexels, m_shadowFilterTexels);
        const float nearPlane = WorldUnits::FromMeters(0.02f);
        uint32_t viewIndex = 0;
        for (const Candidate& candidate : candidates)
        {
            const shaderio::LightData& light = m_lightBufferObjects[candidate.LightIndex];
            const glm::vec3 position(light.position);
            const bool spot = static_cast<uint32_t>(light.position.w) == static_cast<uint32_t>(shaderio::LightType::Spot);
            const uint32_t tile = candidate.Tile;
            // A border of texels around the face (the filter's reach) by a wider field of view.
            const uint32_t border = std::clamp(static_cast<uint32_t>(std::ceil(shadows.maxFilterTexels)) + 2u, 2u, tile / 8u);
            const float widen = static_cast<float>(tile) / static_cast<float>(tile - 2u * border);

            LocalShadowLight& entry = m_localShadowLights[m_localShadowLightCount++];
            entry.LightIndex = candidate.LightIndex;
            entry.FirstView = viewIndex;
            entry.ViewCount = spot ? 1u : 6u;
            for (uint32_t face = 0; face < entry.ViewCount; ++face)
            {
                glm::vec3 direction;
                glm::vec3 up;
                float tanHalf;
                if (spot)
                {
                    // The cone's outer angle (SubmitLight: spotParams.y = -cos(outer) x spotParams.x).
                    const float cosOuter = light.spotParams.x > 0.0f ? std::clamp(-light.spotParams.y / light.spotParams.x, -1.0f, 1.0f) : 0.0f;
                    tanHalf = std::tan(std::min(std::acos(cosOuter), glm::radians(80.0f)));
                    direction = glm::normalize(glm::vec3(light.direction));
                    up = std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
                }
                else
                {
                    tanHalf = 1.0f;
                    direction = kCubeDirections[face];
                    up = kCubeUps[face];
                }
                tanHalf *= widen;

                const glm::mat4 view = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * glm::lookAt(position, position + direction, up);
                const glm::mat4 projection = ReverseZPerspective(tanHalf, nearPlane);
                m_localShadowViews[viewIndex] = view;
                m_localShadowProjections[viewIndex] = projection;
                shaderio::GpuShadowView& gpuView = shadows.views[viewIndex];
                gpuView.viewProj = projection * view;
                gpuView.nearPlane = nearPlane;
                gpuView.texelPerDepth = 2.0f * tanHalf / static_cast<float>(tile);
                gpuView.tileTexels = tile;
                gpuView.borderTexels = border;
                gpuView.atlasRect = glm::vec4(0.0f); // no tile: lit
                if (spot || faceUsed(light, face))
                    pending.push_back({ viewIndex, tile });
                ++viewIndex;
            }
        }
        shadows.viewCount = viewIndex;
        std::stable_sort(pending.begin(), pending.end(), [](const PendingView& a, const PendingView& b) { return a.Tile > b.Tile; });
        uint32_t cursor = 0;
        for (const PendingView& view : pending)
        {
            const uint32_t x = MortonCompact(cursor) * kMinLocalTile;
            const uint32_t y = MortonCompact(cursor >> 1) * kMinLocalTile;
            const float scale = 1.0f / static_cast<float>(atlasSize);
            shadows.views[view.View].atlasRect = glm::vec4(x * scale, y * scale, view.Tile * scale, view.Tile * scale);
            const uint32_t side = view.Tile / kMinLocalTile;
            cursor += side * side;
        }

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_frame.localShadowsAdded = true;
        const uint32_t blockCount = (entryCount + shaderio::CULL_BLOCK_SIZE - 1) / shaderio::CULL_BLOCK_SIZE;

        // Culling per light: a spot light's cone; a cube within its range (its side planes in x / z; an infinite range: everything).
        std::array<ViewDrawResources, MaxLocalShadowLights> draws{};
        for (uint32_t lightSlot = 0; lightSlot < m_localShadowLightCount; ++lightSlot)
        {
            const LocalShadowLight& entry = m_localShadowLights[lightSlot];
            const shaderio::LightData& light = m_lightBufferObjects[entry.LightIndex];
            const glm::vec3 position(light.position);
            const float range = light.direction.w;
            shaderio::CullView cull{};
            if (entry.ViewCount == 1)
            {
                cull.frustum = shaderio::Frustum{ shadows.views[entry.FirstView].viewProj };
                cull.flags = m_instanceCullingEnabled ? shaderio::CULL_INSTANCES : 0u;
            }
            else
            {
                cull.frustum.planes[0] = glm::vec4(1.0f, 0.0f, 0.0f, range - position.x);
                cull.frustum.planes[1] = glm::vec4(-1.0f, 0.0f, 0.0f, range + position.x);
                cull.frustum.planes[2] = glm::vec4(0.0f, 0.0f, 1.0f, range - position.z);
                cull.frustum.planes[3] = glm::vec4(0.0f, 0.0f, -1.0f, range + position.z);
                cull.flags = (m_instanceCullingEnabled && range > 0.0f) ? shaderio::CULL_INSTANCES : 0u;
            }
            cull.viewProj = shadows.views[entry.FirstView].viewProj;
            cull.previousViewProj = cull.viewProj;
            std::copy(m_drawBucketStarts.begin(), m_drawBucketStarts.end(), cull.bucketStart);
            cull.entryCount = entryCount;
            cull.blockCount = blockCount;
            NRI::Buffer* cullViewBuffer = m_localShadowCullViews[frameIndex][lightSlot].get();
            memcpy(cullViewBuffer->map(0, sizeof(cull)), &cull, sizeof(cull));
            cullViewBuffer->unmap();

            // The culling reads the first view's uniforms (the scene tables, the same in every view's copy).
            if (!m_localShadowViewUniforms[frameIndex][entry.FirstView])
            {
                m_localShadowViewUniforms[frameIndex][entry.FirstView] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::UniformBufferObject), .usage = NRI::BufferUsage::Uniform });
                m_localShadowViewUniformsMapped[frameIndex][entry.FirstView] = m_localShadowViewUniforms[frameIndex][entry.FirstView]->map(0, sizeof(shaderio::UniformBufferObject));
            }
            ViewDrawResources& lightDraws = draws[lightSlot];
            lightDraws.VisibleInstances = m_renderGraph.CreateBuffer("Local Shadow Visible Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
            lightDraws.Commands = m_renderGraph.CreateBuffer("Local Shadow Draw Commands", { sizeof(shaderio::MeshTasksIndirectCommand) * entryCount, NRI::BufferUsage::Indirect });
            lightDraws.Counts = m_renderGraph.CreateBuffer("Local Shadow Draw Counts", { sizeof(shaderio::CullCounts), NRI::BufferUsage::Indirect });
            lightDraws.LateInstances = m_renderGraph.CreateBuffer("Local Shadow Late Instances", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
            const RGBuffer flags = m_renderGraph.CreateBuffer("Local Shadow Cull Flags", { sizeof(uint32_t) * entryCount, NRI::BufferUsage::Storage });
            const RGBuffer blocks = m_renderGraph.CreateBuffer("Local Shadow Cull Blocks", { sizeof(shaderio::CullBlock) * blockCount, NRI::BufferUsage::Storage });
            addCullingDispatchPass(kLocalCullNames[lightSlot], lightDraws, flags, blocks, entryCount, RGTexture{},
                                   m_localShadowViewUniforms[frameIndex][entry.FirstView].get(), cullViewBuffer);
        }
        for (uint32_t view = 0; view < shadows.viewCount; ++view)
        {
            if (!m_localShadowViewUniforms[frameIndex][view])
            {
                m_localShadowViewUniforms[frameIndex][view] = m_device->createBuffer(NRI::BufferDesc{ .size = sizeof(shaderio::UniformBufferObject), .usage = NRI::BufferUsage::Uniform });
                m_localShadowViewUniformsMapped[frameIndex][view] = m_localShadowViewUniforms[frameIndex][view]->map(0, sizeof(shaderio::UniformBufferObject));
            }
        }

        // One depth pass into the atlas: per light its draws, per view its tile (viewport and scissor) and its uniforms.
        RGTextureDesc atlasDesc;
        atlasDesc.Size = RGSize::Absolute;
        atlasDesc.Width = atlasSize;
        atlasDesc.Height = atlasSize;
        atlasDesc.Format = NRI::ImageFormat::None; // the device's depth format
        atlasDesc.Usage = NRI::TextureUsage::DepthStencilAttachment;
        resources.LocalShadowAtlas = m_renderGraph.CreateTexture("Local Shadow Atlas", atlasDesc);
        const uint32_t shadowedLights = m_localShadowLightCount;
        const float slopeBias = m_shadowSlopeBias;
        m_renderGraph.AddPass("Local Light Shadows", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.DepthTarget(resources.LocalShadowAtlas, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0 }); // reverse Z
                builder.SetRenderArea({ atlasSize, atlasSize });
                ReadGpuScene(builder, resources);
                ReadIfValid(builder, resources.SkinnedVertices);
                for (uint32_t lightSlot = 0; lightSlot < shadowedLights; ++lightSlot)
                    ReadViewDraws(builder, draws[lightSlot]);
            },
            [this, draws, shadowedLights, atlasSize, slopeBias](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                cmd.setDepthTestEnable(true);
                cmd.setDepthCompareOp(NRI::CompareOp::GreaterOrEqual);
                cmd.setDepthClampEnable(true); // casters nearer than the near plane still cast (depth 1)
                cmd.setDepthBiasEnable(slopeBias > 0.0f);
                cmd.setDepthBias(0.0f, 0.0f, -slopeBias);
                for (uint32_t lightSlot = 0; lightSlot < shadowedLights; ++lightSlot)
                {
                    const LocalShadowLight& entry = m_localShadowLights[lightSlot];
                    MeshletDrawCursor cursor = beginMeshletDraws(context, draws[lightSlot]);
                    cursor.taskFlags &= ~shaderio::MESHLET_COUNT_TRIANGLES;
                    // Each face culls its own clusters (the task stage's frustum test against its uniforms).
                    cursor.taskFlags |= shaderio::MESHLET_CULL_FRUSTUM;
                    for (uint32_t view = entry.FirstView; view < entry.FirstView + entry.ViewCount; ++view)
                    {
                        const glm::vec4 rect = m_localShadows.views[view].atlasRect * static_cast<float>(atlasSize);
                        if (rect.z <= 0.0f)
                            continue;
                        // The graph's flipped viewport (RGViewport::FlippedY) on the tile.
                        cmd.setViewportWithCount({ rect.x, rect.y + rect.w, rect.z, -rect.w }, 0.0f, 1.0f);
                        cmd.setScissorRect(static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y), static_cast<uint32_t>(rect.z), static_cast<uint32_t>(rect.w));
                        cursor.references.matrixReference = m_localShadowViewUniforms[frameIndex][view]->getDeviceAddress();
                        drawMeshletBucket(cmd, cursor, RenderBucket::Opaque, *m_shadowDepthPipeline, NRI::CullMode::None, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::OpaqueDoubleSided, *m_shadowDepthPipeline, NRI::CullMode::None, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::Mask, *m_shadowDepthMaskPipeline, NRI::CullMode::None, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::MaskDoubleSided, *m_shadowDepthMaskPipeline, NRI::CullMode::None, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::Unlit, *m_shadowDepthMaskPipeline, NRI::CullMode::None, true, false);
                        drawMeshletBucket(cmd, cursor, RenderBucket::UnlitDoubleSided, *m_shadowDepthMaskPipeline, NRI::CullMode::None, true, false);
                    }
                }
                cmd.setDepthBiasEnable(false);
                cmd.setDepthClampEnable(false);
                cmd.setViewportWithCount({ 0.0f, static_cast<float>(atlasSize), static_cast<float>(atlasSize), -static_cast<float>(atlasSize) }, 0.0f, 1.0f);
                cmd.setScissorWithCount({ atlasSize, atlasSize });
            });
    }

    void Renderer::updateLocalShadowUniforms()
    {
        uniformData.localShadowsReference = 0;
        uniformData.localShadowLightViewsReference = 0;
        if (!m_frame.localShadowsAdded)
            return;

        // Each view's copy of the frame's uniforms (the task, mesh and culling stages); the cluster LOD stays the camera's.
        for (uint32_t view = 0; view < m_localShadows.viewCount; ++view)
        {
            shaderio::UniformBufferObject uniforms = uniformData;
            const glm::mat4& viewMatrix = m_localShadowViews[view];
            const glm::mat4& projection = m_localShadowProjections[view];
            uniforms.view = uniforms.prevView = viewMatrix;
            uniforms.proj = uniforms.nonJitteredProj = uniforms.prevProj = projection;
            uniforms.invViewProj = glm::inverse(projection * viewMatrix);
            uniforms.frustum = uniforms.frozenFrustum = shaderio::Frustum{ projection * viewMatrix };
            uniforms.jitterOffset = glm::vec2(0.0f);
            uniforms.prevJitterOffset = glm::vec2(0.0f);
            uniforms.clusterStatsReference = 0;
            uniforms.sunShadowCascadesReference = 0;
            memcpy(m_localShadowViewUniformsMapped[frameIndex][view], &uniforms, sizeof(shaderio::UniformBufferObject));
        }

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        constexpr uint32_t NoTexture = 0xFFFFFFFF;
        m_localShadows.atlasTextureIndex = m_renderGraph.GetSlotOr(resources.LocalShadowAtlas, NoTexture);
        if (m_localShadows.atlasTextureIndex == NoTexture) // culled: nothing read it
            return;
        memcpy(m_localShadowBuffersMapped[frameIndex], &m_localShadows, sizeof(shaderio::GpuLocalShadows));

        // Per light (its index in the light buffer): its views, or none.
        uint32_t* lightViews = static_cast<uint32_t*>(m_localShadowLightViewBuffersMapped[frameIndex]);
        std::fill(lightViews, lightViews + m_lightBufferObjects.size(), shaderio::LOCAL_SHADOW_NONE);
        for (uint32_t lightSlot = 0; lightSlot < m_localShadowLightCount; ++lightSlot)
        {
            const LocalShadowLight& entry = m_localShadowLights[lightSlot];
            lightViews[entry.LightIndex] = entry.FirstView | (entry.ViewCount << 16);
        }
        uniformData.localShadowsReference = m_localShadowBuffers[frameIndex]->getDeviceAddress();
        uniformData.localShadowLightViewsReference = m_localShadowLightViewBuffers[frameIndex]->getDeviceAddress();
    }
}
