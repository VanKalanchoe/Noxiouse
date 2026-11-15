#include "UIRenderer.h"

#include <algorithm>
#include <cstring>

#include "NoxCore/Core/Log.h"

namespace Nox::UI
{
    UIRenderer::UIRenderer(NRI::Device& device, NRI::ShaderCompiler& shaderCompiler, Utils::NOXWatcher& fileWatcher)
        : m_Device(device), m_ShaderCompiler(shaderCompiler), m_FileWatcher(fileWatcher)
    {
        NOX_CORE_INFO("Nox::UI::UIRenderer Start");
        CreatePipeline(false);
        CreateBuffers();

        m_FileWatcher.watch(std::filesystem::path("assets/shaders/UI/UIPrimitives.slang"),
            [this](const std::filesystem::path&) { m_ReloadShader.store(true); });
    }

    UIRenderer::~UIRenderer()
    {
        NOX_CORE_INFO("Nox::UI::UIRenderer Shutdown");
    }

    void UIRenderer::BeginFrame()
    {
        m_PrimWriteOffset = 0;
        m_ClipWriteOffset = 0;
        m_MainBatch = {};

        if (m_ReloadShader.exchange(false))
        {
            try
            {
                CreatePipeline(true);
            }
            catch (const std::exception& e)
            {
                NOX_CORE_ERROR("UIPrimitives.slang hot-reload failed (keeping previous pipeline): {}", e.what());
            }
        }
    }

    void UIRenderer::CreatePipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats = { NRI::ImageFormat::Surface };
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/UI/UIPrimitives.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/UI/UIPrimitives.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/UI/UIPrimitives.slang"
        });

        auto pipeline = m_Device.createPipeline(desc, m_ShaderCompiler);
        if (pipeline)
            m_Pipeline = std::move(pipeline);
    }

    void UIRenderer::CreateBuffers()
    {
        const uint64_t primBytes = kMaxPrimitives * sizeof(shaderio::UIPrimitive);
        const uint64_t clipBytes = kMaxClipRects * sizeof(shaderio::UIClipRect);

        m_PrimitiveBuffers.reserve(kMaxFramesInFlight);
        m_PrimitiveBuffersMapped.reserve(kMaxFramesInFlight);
        m_ClipBuffers.reserve(kMaxFramesInFlight);
        m_ClipBuffersMapped.reserve(kMaxFramesInFlight);

        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            auto primBuf = m_Device.createBuffer(NRI::BufferDesc{ .size = primBytes, .usage = NRI::BufferUsage::Storage });
            void* primMapped = primBuf->map(0, primBytes);
            m_PrimitiveBuffers.emplace_back(std::move(primBuf));
            m_PrimitiveBuffersMapped.emplace_back(primMapped);

            auto clipBuf = m_Device.createBuffer(NRI::BufferDesc{ .size = clipBytes, .usage = NRI::BufferUsage::Storage });
            void* clipMapped = clipBuf->map(0, clipBytes);
            m_ClipBuffers.emplace_back(std::move(clipBuf));
            m_ClipBuffersMapped.emplace_back(clipMapped);
        }
    }

    UIBatch UIRenderer::UploadDrawList(const UIDrawList& drawList, uint32_t frameIndex)
    {
        const auto& prims = drawList.GetPrimitives();
        const auto& clips = drawList.GetClipRects();

        const uint32_t slot = frameIndex % kMaxFramesInFlight;
        const size_t availPrims = kMaxPrimitives > m_PrimWriteOffset ? (kMaxPrimitives - m_PrimWriteOffset) : 0;
        const size_t availClips = kMaxClipRects > m_ClipWriteOffset ? (kMaxClipRects - m_ClipWriteOffset) : 0;
        const size_t primCount = std::min(prims.size(), availPrims);
        const size_t clipCount = std::min(clips.size(), availClips);

        UIBatch batch{};
        batch.PrimOffset = m_PrimWriteOffset;
        batch.PrimCount = static_cast<uint32_t>(primCount);
        batch.ClipOffset = m_ClipWriteOffset;
        batch.ClipCount = static_cast<uint32_t>(clipCount);

        if (primCount > 0)
        {
            auto* dst = static_cast<shaderio::UIPrimitive*>(m_PrimitiveBuffersMapped[slot]) + m_PrimWriteOffset;
            std::memcpy(dst, prims.data(), primCount * sizeof(shaderio::UIPrimitive));
            m_PrimWriteOffset += static_cast<uint32_t>(primCount);
        }
        if (clipCount > 0)
        {
            auto* dst = static_cast<shaderio::UIClipRect*>(m_ClipBuffersMapped[slot]) + m_ClipWriteOffset;
            std::memcpy(dst, clips.data(), clipCount * sizeof(shaderio::UIClipRect));
            m_ClipWriteOffset += static_cast<uint32_t>(clipCount);
        }

        return batch;
    }

    void UIRenderer::RenderBatch(NRI::CommandBuffer& cmd, const UIBatch& batch, const glm::vec2& viewportSize, uint32_t imageHeapOffset, uint32_t frameIndex)
    {
        if (!m_Pipeline || batch.PrimCount == 0)
            return;

        const uint32_t slot = frameIndex % kMaxFramesInFlight;
        const float w = std::max(viewportSize.x, 1.0f);
        const float h = std::max(viewportSize.y, 1.0f);

        cmd.setViewportWithCount({ 0.0f, 0.0f, w, h }, 0.0f, 1.0f);
        cmd.setScissorWithCount({ static_cast<uint32_t>(w), static_cast<uint32_t>(h) });
        cmd.setVertexInput();
        cmd.setPrimitiveTopology(NRI::PrimitiveTopology::TriangleList);
        cmd.setPrimitiveRestartEnable(false);
        cmd.setRasterizerDiscardEnable(false);
        cmd.setPolygonMode(NRI::PolygonMode::Fill);
        cmd.setCullMode(NRI::CullMode::None);
        cmd.setFrontFace(NRI::FrontFace::CounterClockWise);
        cmd.setDepthBiasEnable(false);
        cmd.setDepthClampEnable(false);

        const uint32_t sampleCount = 1;
        const uint32_t sampleMask = 0xFFFFFFFF;
        cmd.setRasterizationSamples(sampleCount);
        cmd.setSampleMask(sampleCount, sampleMask);
        cmd.setAlphaToCoverageEnable(false);
        cmd.setAlphaToOneEnableEXT(false);

        cmd.setDepthTestEnable(false);
        cmd.setDepthWriteEnable(false);
        cmd.setDepthCompareOp(NRI::CompareOp::Always);
        cmd.setDepthBoundsTestEnable(false);
        cmd.setStencilTestEnable(false);

        const NRI::ColorBlendEquation blendEquation{
            .srcColorBlendFactor = NRI::BlendFactor::SrcAlpha,
            .dstColorBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
            .colorBlendOp = NRI::BlendOp::Add,
            .srcAlphaBlendFactor = NRI::BlendFactor::One,
            .dstAlphaBlendFactor = NRI::BlendFactor::OneMinusSrcAlpha,
            .alphaBlendOp = NRI::BlendOp::Add,
        };
        const uint32_t colorWriteMask = NRI::ColorComponent::R | NRI::ColorComponent::G | NRI::ColorComponent::B | NRI::ColorComponent::A;
        cmd.setColorBlendEnable(0, true);
        cmd.setColorBlendEquation(0, blendEquation);
        cmd.setColorWriteMask(0, colorWriteMask);
        cmd.setLogicOpEnable(false);

        cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_Pipeline);

        shaderio::UIPushConstants pc{};
        pc.primitiveReference = m_PrimitiveBuffers[slot]->getDeviceAddress() + static_cast<uint64_t>(batch.PrimOffset) * sizeof(shaderio::UIPrimitive);
        pc.clipRectReference = m_ClipBuffers[slot]->getDeviceAddress() + static_cast<uint64_t>(batch.ClipOffset) * sizeof(shaderio::UIClipRect);
        pc.screenSize = glm::vec2(w, h);
        pc.imageHeapIndexOffset = imageHeapOffset;
        pc.primitiveCount = batch.PrimCount;

        cmd.pushData(&pc, sizeof(shaderio::UIPushConstants));
        cmd.drawMeshTasks(1, 1, 1);
    }

    void UIRenderer::Upload(uint32_t frameIndex)
    {
        m_MainBatch = UploadDrawList(m_DrawList, frameIndex);
    }

    void UIRenderer::Render(NRI::CommandBuffer& cmd, const glm::vec2& viewportSize, uint32_t imageHeapOffset, uint32_t frameIndex)
    {
        RenderBatch(cmd, m_MainBatch, viewportSize, imageHeapOffset, frameIndex);
    }
}
