#include "CommandBufferVK.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "CommandAllocatorVK.h"
#include "DeviceVK.h"
#include "BufferVK.h"
#include "TextureVK.h"
#include "SwapchainVK.h"
#include "PipelineVK.h"
#include "DescriptorHeapVK.h"

namespace NRI
{
    CommandBufferVK::CommandBufferVK(DeviceVK& device, CommandAllocatorVK& allocator, uint32_t cbCount) : m_deviceVK(device)
    {
        vk::CommandBufferAllocateInfo allocInfo
        {
            .commandPool = *allocator.getNativePool(),
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = cbCount
        };
        m_commandBuffers = vk::raii::CommandBuffers(m_deviceVK.getDevice(), allocInfo);
    }

    void CommandBufferVK::begin(uint32_t index, bool oneTimeSubmit)
    {
        m_currentFrameIndex = index;

        m_commandBuffers[index].reset();

        vk::CommandBufferBeginInfo beginInfo{};
        if (oneTimeSubmit) beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        m_commandBuffers[index].begin(beginInfo);
    }

    void CommandBufferVK::end(uint32_t index)
    {
        // Safety check: Ensure the renderer is closing the exact frame slot it opened!
        assert(index == m_currentFrameIndex && "Attempting to end a frame slot that wasn't actively being recorded!");

        m_commandBuffers[index].end();
    }

    void CommandBufferVK::beginRendering(RenderDesc& desc)
    {
        auto translateLoadOp = [](LoadOP op)
        {
            switch (op)
            {
            case LoadOP::load: return vk::AttachmentLoadOp::eLoad;
            case LoadOP::clear: return vk::AttachmentLoadOp::eClear;
            case LoadOP::dontCare: return vk::AttachmentLoadOp::eDontCare;
            default: return vk::AttachmentLoadOp::eNoneEXT;
            }
        };

        auto translateStoreOp = [](StoreOP op)
        {
            switch (op)
            {
            case StoreOP::store: return vk::AttachmentStoreOp::eStore;
            case StoreOP::dontCare: return vk::AttachmentStoreOp::eDontCare;
            default: return vk::AttachmentStoreOp::eNoneEXT;
            }
        };

        std::vector<vk::RenderingAttachmentInfo> vkColorAttachments;
        vkColorAttachments.reserve(desc.colorAttachments.size());

        for (const auto& colorDesc : desc.colorAttachments)
        {
            vk::ImageView mainView = nullptr;
            if (colorDesc.attachment)
            {
                mainView = static_cast<TextureVK*>(colorDesc.attachment)->getNativeView();
            }
            else if (colorDesc.attachmentSwapchain)
            {
                mainView = dynamic_cast<SwapchainVK*>(colorDesc.attachmentSwapchain)->getNativeView(colorDesc.resolveImageIndex);
            }

            vk::ClearValue vkClearColor = vk::ClearColorValue(
                colorDesc.clearColor.r, colorDesc.clearColor.g, colorDesc.clearColor.b, colorDesc.clearColor.a
            );

            vk::RenderingAttachmentInfo colorAttachmentInfo
            {
                .imageView = mainView,
                .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .loadOp = translateLoadOp(colorDesc.loadOP),
                .storeOp = translateStoreOp(colorDesc.storeOP),
                .clearValue = vkClearColor
            };

            // Handle Resolve Target (MSAA) if provided
            vk::ImageView resolveView = nullptr;
            if (colorDesc.resolve)
            {
                resolveView = static_cast<TextureVK*>(colorDesc.resolve)->getNativeView();
            }
            else if (colorDesc.resolveSwapchain)
            {
                resolveView = static_cast<SwapchainVK*>(colorDesc.resolveSwapchain)->getNativeView(colorDesc.resolveImageIndex);
            }

            if (resolveView)
            {
                colorAttachmentInfo.resolveMode = vk::ResolveModeFlagBits::eAverage;
                colorAttachmentInfo.resolveImageView = resolveView;
                colorAttachmentInfo.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
            }

            vkColorAttachments.push_back(colorAttachmentInfo);
        }

        vk::RenderingAttachmentInfo depthAttachmentInfo{};
        bool hasDepth = (desc.depthAttachment.attachment != nullptr || desc.depthAttachment.attachmentSwapchain != nullptr);

        if (hasDepth)
        {
            vk::ImageView depthView = dynamic_cast<TextureVK*>(desc.depthAttachment.attachment)->getNativeView();

            vk::ClearValue vkClearDepth = vk::ClearDepthStencilValue
            (
                desc.depthAttachment.clearDepth.depth, desc.depthAttachment.clearDepth.stencil
            );

            depthAttachmentInfo =
            {
                .imageView = depthView,
                .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
                .loadOp = translateLoadOp(desc.depthAttachment.loadOP),
                .storeOp = translateStoreOp(desc.depthAttachment.storeOP),
                .clearValue = vkClearDepth
            };
        }

        vk::RenderingInfo renderingInfo =
        {
            .renderArea = {.offset = {0, 0}, .extent = {desc.renderArea.width, desc.renderArea.height}},
            .layerCount = 1,
            .colorAttachmentCount = static_cast<uint32_t>(vkColorAttachments.size()),
            .pColorAttachments = vkColorAttachments.data(),
            .pDepthAttachment = hasDepth ? &depthAttachmentInfo : nullptr
        };

        m_commandBuffers[m_currentFrameIndex].beginRendering(renderingInfo);
    }

    void CommandBufferVK::endRendering()
    {
        m_commandBuffers[m_currentFrameIndex].endRendering();
    }

    void CommandBufferVK::renderImGui()
    {
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), *m_commandBuffers[m_currentFrameIndex]);
    }
    
    void CommandBufferVK::copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, uint64_t deviceSize)
    {
        auto* vkSrc = dynamic_cast<BufferVK*>(&srcBuffer);
        auto* vkDst = dynamic_cast<BufferVK*>(&dstBuffer);

        m_commandBuffers[m_currentFrameIndex].copyBuffer(*vkSrc->getNativeBuffer(), *vkDst->getNativeBuffer(), vk::BufferCopy{.size = deviceSize});
    }

    void CommandBufferVK::bindPipeline(PipelineBindPoint bindPoint, Pipeline& pipeline)
    {
        auto* vkPip = dynamic_cast<PipelineVK*>(&pipeline);

        auto toVkPipelineBindPoint = [](PipelineBindPoint point)
        {
            switch (point)
            {
            case PipelineBindPoint::Graphics:
                return vk::PipelineBindPoint::eGraphics;

            case PipelineBindPoint::Compute:
                return vk::PipelineBindPoint::eCompute;
            }

            throw std::runtime_error("Unknown PipelineBindPoint");
        };

        vk::PipelineBindPoint vkBindPoint = toVkPipelineBindPoint(bindPoint);

        m_commandBuffers[m_currentFrameIndex].bindPipeline(vkBindPoint, vkPip->getNativePipeline());
    }

    void CommandBufferVK::setViewport(Extent2D swapChainExtent)
    {
        m_commandBuffers[m_currentFrameIndex].setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(swapChainExtent.width), static_cast<float>(swapChainExtent.height), 0.0f, 1.0f));
    }

    void CommandBufferVK::setScissor(Extent2D swapChainExtent)
    {
        m_commandBuffers[m_currentFrameIndex].setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), {.width = swapChainExtent.width, .height = swapChainExtent.height}));
    }

    void CommandBufferVK::bindDescriptorHeaps(DescriptorHeap* resourceHeap, DescriptorHeap* samplerHeap)
    {
        if (resourceHeap)
        {
            // Bind the heap containing resources (buffers and images)
            vk::BindHeapInfoEXT bindHeapInfoRes
            {
                .heapRange = 
            {
                    .address = resourceHeap->getGPUAddress(),
                    .size = resourceHeap->getSize()
                },
                // Put the reserved range after our descriptors, simplifies some calculations
                .reservedRangeOffset = resourceHeap->getReservedRangeOffset(),
                .reservedRangeSize = resourceHeap->getReservedRangeSize(),
            };
            m_commandBuffers[m_currentFrameIndex].bindResourceHeapEXT(bindHeapInfoRes);
        }

        if (samplerHeap)
        {
            // Bind the heap containing samplers
            vk::BindHeapInfoEXT bindHeapInfoSamplers
            {
                .heapRange = 
            {
                    .address = samplerHeap->getGPUAddress(),
                    .size = samplerHeap->getSize()
                },
                // Put the reserved range after our descriptors, simplifies some calculations
                .reservedRangeOffset = samplerHeap->getReservedRangeOffset(),
                .reservedRangeSize = samplerHeap->getReservedRangeSize(),
            };
            m_commandBuffers[m_currentFrameIndex].bindSamplerHeapEXT(bindHeapInfoSamplers);
        }
    }
    
    void CommandBufferVK::pushData(const void* data, uint32_t size)
    {
        vk::PushDataInfoEXT pushDataInfo
        {
            .data = {.address = data, .size = size}
        };
        m_commandBuffers[m_currentFrameIndex].pushDataEXT(pushDataInfo);
    }

    void CommandBufferVK::bindVertexBuffers(uint32_t firstBinding, Buffer& buffer, uint64_t offset)
    {
        auto& vkBuffer = dynamic_cast<BufferVK&>(buffer);
        m_commandBuffers[m_currentFrameIndex].bindVertexBuffers(firstBinding, *vkBuffer.getNativeBuffer(), offset);
    }

    void CommandBufferVK::bindIndexBuffer(Buffer& buffer, uint64_t offet)
    {
        auto& vkBuffer = dynamic_cast<BufferVK&>(buffer);
        m_commandBuffers[m_currentFrameIndex].bindIndexBuffer(vkBuffer.getNativeBuffer(), offet, vk::IndexType::eUint32);
    }

    void CommandBufferVK::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    {
        m_commandBuffers[m_currentFrameIndex].drawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void CommandBufferVK::transitionTextureLayout(Texture& texture, TextureLayout oldLayout, TextureLayout newLayout)
    {
        auto* vkTex = dynamic_cast<TextureVK*>(&texture);

        vk::ImageAspectFlags imageAspectFlags = (vkTex->getUsage() == TextureUsage::DepthStencilAttachment)
                                                    ? vk::ImageAspectFlagBits::eDepth
                                                    : vk::ImageAspectFlagBits::eColor;

        submitImageBarrier(vkTex->getNativeImage(), oldLayout, newLayout, imageAspectFlags);
    }

    void CommandBufferVK::transitionSwapchainLayout(Swapchain& swapchain, uint32_t imageIndex, TextureLayout oldLayout, TextureLayout newLayout)
    {
        auto* vkSwapchain = dynamic_cast<SwapchainVK*>(&swapchain);
        vk::Image rawImage = vkSwapchain->getNativeImage(imageIndex);

        submitImageBarrier(rawImage, oldLayout, newLayout, vk::ImageAspectFlagBits::eColor);
    }

    void CommandBufferVK::submitImageBarrier(vk::Image image, TextureLayout oldLayout, TextureLayout newLayout, vk::ImageAspectFlags aspectFlags)
    {
        vk::PipelineStageFlags2 srcStageMask = vk::PipelineStageFlagBits2::eNone;
        vk::PipelineStageFlags2 dstStageMask = vk::PipelineStageFlagBits2::eNone;
        vk::AccessFlags2 srcAccessMask = vk::AccessFlagBits2::eNone;
        vk::AccessFlags2 dstAccessMask = vk::AccessFlagBits2::eNone;

        getSyncFlags(oldLayout, true, srcStageMask, srcAccessMask);
        getSyncFlags(newLayout, false, dstStageMask, dstAccessMask);

        vk::ImageMemoryBarrier2 barrier =
        {
            .srcStageMask = srcStageMask,
            .srcAccessMask = srcAccessMask,
            .dstStageMask = dstStageMask,
            .dstAccessMask = dstAccessMask,
            .oldLayout = translateLayoutToVk(oldLayout),
            .newLayout = translateLayoutToVk(newLayout),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
                .aspectMask = aspectFlags,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        vk::DependencyInfo dependencyInfo =
        {
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier
        };

        m_commandBuffers[m_currentFrameIndex].pipelineBarrier2(dependencyInfo);
    }

    void CommandBufferVK::getSyncFlags(TextureLayout layout, bool isSource, vk::PipelineStageFlags2& stageMask, vk::AccessFlags2& accessMask) const
    {
        switch (layout)
        {
        case TextureLayout::Undefined:
            stageMask = vk::PipelineStageFlagBits2::eAllCommands;
            accessMask = {};
            break;

        case TextureLayout::ColorAttachment:
            stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
            accessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
            break;

        case TextureLayout::DepthAttachment:
            stageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
            accessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
            break;

        case TextureLayout::ShaderResource:
            stageMask = vk::PipelineStageFlagBits2::eFragmentShader;
            accessMask = vk::AccessFlagBits2::eShaderRead;
            break;

        case TextureLayout::Present:
            stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe;
            accessMask = {};
            break;

        default:
            stageMask = vk::PipelineStageFlagBits2::eAllCommands;
            accessMask = {};
            break;
        }
    }

    vk::ImageLayout CommandBufferVK::translateLayoutToVk(TextureLayout layout) const
    {
        switch (layout)
        {
        case TextureLayout::Undefined: return vk::ImageLayout::eUndefined;
        case TextureLayout::ColorAttachment: return vk::ImageLayout::eColorAttachmentOptimal;
        case TextureLayout::DepthAttachment: return vk::ImageLayout::eDepthAttachmentOptimal;
        case TextureLayout::ShaderResource: return vk::ImageLayout::eShaderReadOnlyOptimal;
        case TextureLayout::Present: return vk::ImageLayout::ePresentSrcKHR;
        default:
            throw std::runtime_error("Unsupported TextureLayout passed to Vulkan backend!");
        }
    }
}
