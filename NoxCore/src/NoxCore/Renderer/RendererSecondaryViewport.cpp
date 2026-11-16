#include "Renderer.h"

#include "NoxCore/Core/Window.h"

namespace Nox
{
    std::unique_ptr<NRI::Swapchain> Renderer::CreateWindowSwapchain(Nox::Window& window, uint32_t width, uint32_t height, bool vSync)
    {
        if (!m_device)
            return nullptr;
        NRI::SwapchainDesc desc{};
        desc.Width = std::max(width, 1u);
        desc.Height = std::max(height, 1u);
        desc.VSync = vSync;
        desc.Window = &window;
        return m_device->createSwapchain(desc);
    }

    std::unique_ptr<NRI::CommandAllocator> Renderer::CreateUICommandAllocator()
    {
        if (!m_device)
            return nullptr;
        return m_device->createCommandAllocator(NRI::CommandBufferReset::PerCommandBuffer, NRI::QueueType::Graphics);
    }

    std::unique_ptr<UI::UIUploadBuffers> Renderer::CreateUIUploadBuffers()
    {
        return m_uiRenderer ? m_uiRenderer->CreateUploadBuffers() : nullptr;
    }

    void Renderer::WaitDeviceIdle()
    {
        if (m_device)
            m_device->waitIdle();
    }

    bool Renderer::RenderSecondaryUIViewport(
        NRI::Swapchain& swapchain,
        NRI::CommandAllocator& /*allocator*/,
        NRI::CommandBuffer& cmd,
        UI::UIUploadBuffers& uploads,
        const UI::UIDrawList& drawList,
        const glm::vec2& viewportSize,
        const glm::vec4& clearColor)
    {
        if (!m_device || !m_uiRenderer || !m_resourceHeap)
            return true;

        const NRI::Extent2D extent = swapchain.getExtent();
        if (extent.width == 0 || extent.height == 0)
            return true;

        const uint32_t slot = frameIndex % MAX_FRAMES_IN_FLIGHT;
        uint32_t imageIndex = 0;
        if (swapchain.acquireNextImage(slot, imageIndex) == NRI::FrameResult::ResizeRequired)
            return false;

        // The window's own buffers: acquireNextImage just waited for this swapchain's previous submission in `slot`, the
        // last one that read the slot's buffers, so they are written from the start. (The main window's buffers are only
        // guarded by the main swapchain's fences, which do not cover this window's submissions.)
        uploads.Reset();
        const UI::UIBatch batch = m_uiRenderer->UploadDrawList(uploads, drawList, slot);

        cmd.begin(0);
        applyCommandBufferBaseline(cmd);
        cmd.transitionSwapchainLayout(swapchain, imageIndex, NRI::TextureLayout::Undefined, NRI::TextureLayout::ColorAttachment);

        NRI::RenderDesc renderDesc{};
        renderDesc.renderArea = extent;
        renderDesc.isSwapchainPass = true;
        NRI::RenderAttachDesc colorAttach{};
        colorAttach.attachmentSwapchain = &swapchain;
        colorAttach.resolveImageIndex = imageIndex;
        colorAttach.loadOP = NRI::LoadOP::clear;
        colorAttach.storeOP = NRI::StoreOP::store;
        colorAttach.clearColor = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };
        renderDesc.colorAttachments.push_back(colorAttach);

        cmd.beginRendering(renderDesc);
        m_uiRenderer->RenderBatch(
            cmd,
            uploads,
            batch,
            { static_cast<float>(extent.width), static_cast<float>(extent.height) },
            m_resourceHeap->getImageHeapIndexOffset(),
            slot
        );
        cmd.endRendering();
        cmd.transitionSwapchainLayout(swapchain, imageIndex, NRI::TextureLayout::ColorAttachment, NRI::TextureLayout::Present);
        cmd.end(0);

        NRI::CommandBuffer* cmdPtr = &cmd;
        m_device->submitCommandBuffers(std::span<NRI::CommandBuffer* const>(&cmdPtr, 1), swapchain, slot, imageIndex);

        if (swapchain.present(slot, imageIndex) == NRI::FrameResult::ResizeRequired)
            return false;

        (void)viewportSize;
        return true;
    }
}
