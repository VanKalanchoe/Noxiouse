// Post effects (PP1, docs/Backlog_2026.md; Settings > Post Effects): what UE / Godot put between the lighting and the display, every
// one off by default. Here the HDR passes before the tonemapper (PostEffects.slang): depth of field, then motion blur, then bloom's
// down / up chain; the tonemap pass (PostProcess.slang) adds the bloom and lens flares, grades the colour and applies sharpening, the
// LUT, vignette, grain and chromatic aberration.
#include "NoxCore/Renderer/Renderer.h"

#include <algorithm>

#include "FrameGraphResources.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Core/Log.h"

namespace Nox
{
    namespace
    {
        constexpr uint32_t BloomMips = 6;
    }

    void Renderer::createPostEffectPipelines(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.type = NRI::PipelineType::Compute;
        desc.forceCompile = forceCompile;
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Compute, .entryPoint = "dofMain", .sourcePath = "assets/shaders/PostEffects.slang" });
        m_dofPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        desc.shaders.back().entryPoint = "motionBlurMain";
        m_motionBlurPipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        desc.shaders.back().entryPoint = "bloomDownsampleMain";
        m_bloomDownsamplePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
        desc.shaders.back().entryPoint = "bloomUpsampleMain";
        m_bloomUpsamplePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::createSceneOutlinePipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = { NRI::ImageFormat::R16G16B16A16_SFLOAT }; // the HDR scene
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Vertex, .entryPoint = "vertMain", .sourcePath = "assets/shaders/SceneOutline.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain", .sourcePath = "assets/shaders/SceneOutline.slang" });
        m_sceneOutlinePipeline = m_device->createPipeline(desc, *m_shaderCompiler);
    }

    void Renderer::addSceneOutlinePass()
    {
        const PostProcessSettings& settings = activePostEffects();
        // From the G-buffer of this frame: not with the path tracer (its image does not come from it) or the debug views.
        if (!settings.Outlines || !m_sceneOutlinePipeline || m_frame.pathTracerActive || m_frame.restirPTActive || m_debugMode != 0)
            return;
        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        if (!resources.HDRScene.IsValid() || !resources.Depth.IsValid() || !resources.GBufferNormal.IsValid() || !resources.GBufferMaterial.IsValid())
            return;
        const NRI::Extent2D extent = m_frame.renderExtent;
        if (extent.width == 0 || extent.height == 0)
            return;

        shaderio::PushConstantSceneOutline push{};
        push.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
        push.toonOnly = settings.OutlineToonOnly ? 1u : 0u;
        push.color = glm::vec4(glm::clamp(settings.OutlineColor, glm::vec3(0.0f), glm::vec3(1.0f)), 1.0f);
        // The thickness is in output pixels: at a lower render resolution (DLSS) the same line is fewer render pixels.
        const float renderScale = m_frame.outputExtent.width > 0 ? static_cast<float>(extent.width) / static_cast<float>(m_frame.outputExtent.width) : 1.0f;
        const float depthSensitivity = std::clamp(settings.OutlineDepthSensitivity, 0.0f, 1.0f);
        const float normalSensitivity = std::clamp(settings.OutlineNormalSensitivity, 0.0f, 1.0f);
        push.params = glm::vec4(std::max(settings.OutlineThickness, 0.0f) * renderScale,
                                depthSensitivity > 0.0f ? 0.01f + (1.0f - depthSensitivity) * 0.24f : 0.0f, // 1 % .. 25 % of the distance (0.5: ~13 %)
                                normalSensitivity > 0.0f ? 0.02f + (1.0f - normalSensitivity) * 0.68f : 0.0f, // ~11 .. ~72 degrees
                                std::max(WorldUnits::FromMeters(std::max(settings.OutlineFadeDistanceMeters, 0.0f)), 0.0f));
        push.params2 = glm::vec4(static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 0.0f);
        m_renderGraph.AddPass("Scene Outlines", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.ColorTarget(resources.HDRScene, NRI::LoadOP::load, NRI::StoreOP::store);
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferMaterial);
                builder.SetRenderArea(extent);
            },
            [this, push, res = &resources](RGPassContext& context) mutable
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                push.depthIndex = uniformData.imageHeapIndexOffset + context.Slot(res->Depth);
                push.normalIndex = uniformData.imageHeapIndexOffset + context.Slot(res->GBufferNormal);
                push.materialIndex = uniformData.imageHeapIndexOffset + context.Slot(res->GBufferMaterial);
                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_sceneOutlinePipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, true);
                cmd.setColorWriteMask(0, NRI::ColorComponent::All);
                // The scene times the pixel's colour (white: unchanged); alpha stays.
                cmd.setColorBlendEquation(0, {
                    .srcColorBlendFactor = NRI::BlendFactor::Zero, .dstColorBlendFactor = NRI::BlendFactor::SrcColor, .colorBlendOp = NRI::BlendOp::Add,
                    .srcAlphaBlendFactor = NRI::BlendFactor::Zero, .dstAlphaBlendFactor = NRI::BlendFactor::One, .alphaBlendOp = NRI::BlendOp::Add });
                cmd.pushData(&push, sizeof(push));
                cmd.draw(3, 1, 0, 0);
                cmd.setColorBlendEnable(0, false);
            });
    }

    Renderer::PostEffectOutputs Renderer::addPostEffectPasses(RGTexture hdrSource)
    {
        PostEffectOutputs outputs{ hdrSource, RGTexture{} };
        const PostEffectSettings& settings = activePostEffects();

        // The grading LUT, loaded when its file changes (display values as they are stored: no sRGB decode, no mips).
        if (settings.LutPath != m_postLutLoadedPath)
        {
            if (m_postLut)
                deferAssetRelease(m_postLut);
            m_postLut = nullptr;
            m_postLutLoadedPath = settings.LutPath;
            if (!settings.LutPath.empty())
            {
                TextureSpecification spec;
                spec.generateMips = false;
                spec.format = NRI::ImageFormat::RGBA8;
                spec.virtualTexture = VirtualTextureMode::Off;
                m_postLut = TextureImporter::LoadTexture2D(settings.LutPath, spec, this);
                if (!m_postLut)
                    NOX_CORE_WARN("Post effects: could not load the colour grading LUT '{}'", settings.LutPath);
            }
        }

        if (!hdrSource.IsValid() || m_debugMode != 0)
            return outputs;
        const RGTextureKey& key = m_renderGraph.GetTextureKey(hdrSource);
        const uint32_t width = key.Width;
        const uint32_t height = key.Height;
        if (width == 0 || height == 0)
            return outputs;
        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        RGTextureDesc imageDesc;
        imageDesc.Size = RGSize::Absolute;
        imageDesc.Width = width;
        imageDesc.Height = height;
        imageDesc.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        imageDesc.Usage = NRI::TextureUsage::Storage;
        const uint64_t uniforms = m_uniformBuffers[frameIndex]->getDeviceAddress();

        // Depth of field: needs the depth and a perspective camera (the depth's view distance is near / depth).
        const bool perspective = uniformData.nonJitteredProj[3][3] == 0.0f;
        if (settings.DepthOfField && m_dofPipeline && resources.Depth.IsValid() && perspective)
        {
            const RGTexture output = m_renderGraph.CreateTexture("Depth of Field", imageDesc);
            const RGTexture source = outputs.Hdr;
            const float proj11 = uniformData.nonJitteredProj[1][1];
            const float focalLength = WorldUnits::FromMeters(0.012f) * proj11; // a 24 mm-tall sensor behind this field of view
            const float fStop = std::max(settings.FStop, 0.5f);
            shaderio::PushConstantPostEffect push{};
            push.matrixReference = uniforms;
            push.width = width;
            push.height = height;
            push.params = glm::vec4(WorldUnits::FromMeters(std::max(settings.FocusDistanceMeters, 0.05f)),
                                    (focalLength / fStop) * (proj11 * 0.5f), // aperture x focal length / sensor height
                                    std::max(settings.MaxBlurPixels, 0.0f), static_cast<float>(height));
            push.params2 = glm::vec4(uniformData.nonJitteredProj[3][2], 0.0f, 0.0f, 0.0f);
            m_renderGraph.AddPass("Depth of Field", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(source);
                    builder.Read(resources.Depth);
                    builder.Write(output, RGTextureAccess::StorageWrite);
                },
                [this, push, source, output, res = &resources](RGPassContext& context) mutable
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get()); // (after DLSS: NGX may have bound its own)
                    push.sourceIndex = uniformData.imageHeapIndexOffset + context.Slot(source);
                    push.auxIndex = uniformData.imageHeapIndexOffset + context.Slot(res->Depth);
                    push.outputIndex = context.StorageSlot(output);
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_dofPipeline);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                });
            outputs.Hdr = output;
            outputs.HdrChanged = true;
        }

        // Motion blur: needs the G-buffer's motion vectors (written this frame when it is on).
        if (settings.MotionBlur && m_motionBlurPipeline && m_frame.gbufferVelocityWritten && resources.GBufferVelocity.IsValid())
        {
            const RGTexture output = m_renderGraph.CreateTexture("Motion Blur", imageDesc);
            const RGTexture source = outputs.Hdr;
            shaderio::PushConstantPostEffect push{};
            push.matrixReference = uniforms;
            push.width = width;
            push.height = height;
            push.params = glm::vec4(std::clamp(settings.MotionBlurShutter, 0.0f, 1.0f), std::max(settings.MotionBlurMaxPixels, 1.0f), 12.0f, 0.0f);
            m_renderGraph.AddPass("Motion Blur", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(source);
                    builder.Read(resources.GBufferVelocity);
                    builder.Write(output, RGTextureAccess::StorageWrite);
                },
                [this, push, source, output, res = &resources](RGPassContext& context) mutable
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
                    push.sourceIndex = uniformData.imageHeapIndexOffset + context.Slot(source);
                    push.auxIndex = uniformData.imageHeapIndexOffset + context.Slot(res->GBufferVelocity);
                    push.outputIndex = context.StorageSlot(output);
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_motionBlurPipeline);
                    cmd.pushData(&push, sizeof(push));
                    cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                });
            outputs.Hdr = output;
            outputs.HdrChanged = true;
        }

        // Bloom: the down chain from half size, then up again (each mip averaged with the tent-filtered one below it).
        if (settings.Bloom && m_bloomDownsamplePipeline && m_bloomUpsamplePipeline)
        {
            RGTextureDesc bloomDesc = imageDesc;
            bloomDesc.Width = std::max(width / 2, 1u);
            bloomDesc.Height = std::max(height / 2, 1u);
            bloomDesc.MipLevels = BloomMips;
            const RGTexture down = m_renderGraph.CreateTexture("Bloom Down", bloomDesc);
            const RGTexture up = m_renderGraph.CreateTexture("Bloom Up", bloomDesc);
            const RGTexture source = outputs.Hdr;
            auto mipSize = [&](uint32_t mip) { return glm::uvec2(std::max(bloomDesc.Width >> mip, 1u), std::max(bloomDesc.Height >> mip, 1u)); };
            for (uint32_t mip = 0; mip < BloomMips; ++mip)
            {
                const glm::uvec2 size = mipSize(mip);
                const glm::vec2 sourceTexel = mip == 0 ? glm::vec2(1.0f / width, 1.0f / height) : 1.0f / glm::vec2(mipSize(mip - 1));
                shaderio::PushConstantPostEffect push{};
                push.matrixReference = uniforms;
                push.width = size.x;
                push.height = size.y;
                push.params = glm::vec4(mip == 0 ? 1.0f : 0.0f, mip == 0 ? 0.0f : static_cast<float>(mip - 1), sourceTexel.x, sourceTexel.y);
                m_renderGraph.AddPass(mip == 0 ? "Bloom Downsample" : "Bloom Downsample Mip", RGPassFlags::None,
                    [&, mip](RGBuilder& builder)
                    {
                        builder.Read(mip == 0 ? source : down);
                        builder.Write(down, RGTextureAccess::StorageWrite);
                    },
                    [this, push, mip, source, down](RGPassContext& context) mutable
                    {
                        NRI::CommandBuffer& cmd = context.Cmd();
                        cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
                        push.sourceIndex = uniformData.imageHeapIndexOffset + context.Slot(mip == 0 ? source : down);
                        push.outputIndex = context.StorageSlot(down, mip);
                        cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_bloomDownsamplePipeline);
                        cmd.pushData(&push, sizeof(push));
                        cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                    });
            }
            for (int mip = static_cast<int>(BloomMips) - 1; mip >= 0; --mip)
            {
                const glm::uvec2 size = mipSize(static_cast<uint32_t>(mip));
                const bool last = mip == static_cast<int>(BloomMips) - 1;
                const glm::vec2 nextTexel = last ? glm::vec2(0.0f) : 1.0f / glm::vec2(mipSize(static_cast<uint32_t>(mip) + 1));
                shaderio::PushConstantPostEffect push{};
                push.matrixReference = uniforms;
                push.width = size.x;
                push.height = size.y;
                push.sourceMip = static_cast<uint32_t>(mip);
                push.params = glm::vec4(std::max(settings.BloomRadius, 0.0f), last ? 1.0f : 0.0f, nextTexel.x, nextTexel.y);
                m_renderGraph.AddPass(last ? "Bloom Upsample" : "Bloom Upsample Mip", RGPassFlags::None,
                    [&](RGBuilder& builder)
                    {
                        builder.Read(down);
                        builder.Read(up);
                        builder.Write(up, RGTextureAccess::StorageWrite);
                    },
                    [this, push, mip, down, up](RGPassContext& context) mutable
                    {
                        NRI::CommandBuffer& cmd = context.Cmd();
                        cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
                        push.sourceIndex = uniformData.imageHeapIndexOffset + context.Slot(down);
                        push.auxIndex = uniformData.imageHeapIndexOffset + context.Slot(up);
                        push.outputIndex = context.StorageSlot(up, static_cast<uint32_t>(mip));
                        cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_bloomUpsamplePipeline);
                        cmd.pushData(&push, sizeof(push));
                        cmd.dispatch((push.width + 7) / 8, (push.height + 7) / 8, 1);
                    });
            }
            outputs.Bloom = up;
        }
        return outputs;
    }
}
