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
#include "NoxCore/Core/core.h"

namespace NRI
{
    static vk::PrimitiveTopology translateTopologyToVk(const PrimitiveTopology& topology)
    {
        switch (topology)
        {
        case PrimitiveTopology::TriangleList:  return vk::PrimitiveTopology::eTriangleList;
        }
        return vk::PrimitiveTopology::eTriangleList; // Default fallback
    }
    
    static vk::PolygonMode translatePolygonToVk(const PolygonMode& polygon)
    {
        switch (polygon)
        {
            case PolygonMode::Fill: return vk::PolygonMode::eFill;
        }
    }
    
    static vk::CullModeFlagBits translateCullModeToVk(const CullMode& cullMode)
    {
        switch (cullMode)
        {
            case CullMode::None: return vk::CullModeFlagBits::eNone;
            case CullMode::Back: return vk::CullModeFlagBits::eBack;
        }
    }
    
    static vk::FrontFace translateFrontFaceToVk(const FrontFace& frontFace)
    {
        switch (frontFace)
        {
            case FrontFace::ClockWise: return vk::FrontFace::eClockwise;
            case FrontFace::CounterClockWise: return vk::FrontFace::eCounterClockwise;
        }
    }
    
    static vk::CompareOp translateCompareOpToVk(const CompareOp& compareOp)
    {
        switch (compareOp)
        {
        case CompareOp::Less: return vk::CompareOp::eLess;
        case CompareOp::Greater: return vk::CompareOp::eGreater;
        case CompareOp::GreaterOrEqual : return vk::CompareOp::eGreaterOrEqual;
        }
    }
    
    static vk::BlendFactor translateBlendFactor(BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero:         return vk::BlendFactor::eZero;
        case BlendFactor::One:         return vk::BlendFactor::eOne;
        case BlendFactor::SrcAlpha:         return vk::BlendFactor::eSrcAlpha;
        case BlendFactor::OneMinusSrcAlpha: return vk::BlendFactor::eOneMinusSrcAlpha;
        }
        return vk::BlendFactor::eOne;
    }

    static vk::BlendOp translateBlendOp(BlendOp op)
    {
        switch (op)
        {
        case BlendOp::Add:             return vk::BlendOp::eAdd;
        }
        return vk::BlendOp::eAdd;
    }
    
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
            bool isIntegerFormat = false;
            
            if (colorDesc.attachment)
            {
                auto* textureVK = static_cast<TextureVK*>(colorDesc.attachment);
                mainView = textureVK->getNativeView();
                
                if (textureVK->getFormat() == vk::Format::eR32Sint) isIntegerFormat = true;
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
                if (isIntegerFormat)
                    colorAttachmentInfo.resolveMode = vk::ResolveModeFlagBits::eSampleZero;
                else
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
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData == nullptr || drawData->CmdListsCount == 0 || drawData->TotalVtxCount == 0)
            return;
        
        ImGui_ImplVulkan_RenderDrawData(drawData, *m_commandBuffers[m_currentFrameIndex]);
    }
    
    void CommandBufferVK::copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, const BufferCopyRegion& region)
    {
        auto* srcVK = dynamic_cast<BufferVK*>(&srcBuffer);
        auto* dstVK = dynamic_cast<BufferVK*>(&dstBuffer);
        
        uint64_t copySize = region.size;
        if (copySize == 0)
        {
            copySize = std::min(srcVK->getSize() - region.srcOffset, dstVK->getSize() - region.dstOffset);
        }
        
        vk::BufferCopy copyRegion
        {
            .srcOffset = region.srcOffset,
            .dstOffset = region.dstOffset,
            .size = copySize
        };

        m_commandBuffers[m_currentFrameIndex].copyBuffer(*srcVK->getNativeBuffer(), *dstVK->getNativeBuffer(), copyRegion);
    }

    void CommandBufferVK::bindPipeline(PipelineBindPoint bindPoint, Pipeline& pipeline)
    {
        auto* vkPip = dynamic_cast<PipelineVK*>(&pipeline);

        if (vkPip->isShaderObject())
        {
            // For Graphics: binds 4 stages ({Vert, Frag, Task, Mesh}) with active shaders and null handles
            // For Compute: binds 1 stage ({Compute}) with 1 shader handle
            m_commandBuffers[m_currentFrameIndex].bindShadersEXT(vkPip->getStages(), vkPip->getRawShaders());
        }
        else
        {
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
    }

    void CommandBufferVK::setViewport(Extent2D swapChainExtent)
    {
        m_commandBuffers[m_currentFrameIndex].setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(swapChainExtent.width), static_cast<float>(swapChainExtent.height), 0.0f, 1.0f));
    }

    void CommandBufferVK::setScissor(Extent2D swapChainExtent)
    {
        m_commandBuffers[m_currentFrameIndex].setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), {.width = swapChainExtent.width, .height = swapChainExtent.height}));
    }
    
    void CommandBufferVK::setViewportWithCount(const ViewportBounds& bounds, float minDepth, float maxDepth)
    {
        vk::Viewport viewport(bounds.x, bounds.y, bounds.width, bounds.height, minDepth, maxDepth);
        m_commandBuffers[m_currentFrameIndex].setViewportWithCount(viewport);
    }

    void CommandBufferVK::setScissorWithCount(const Extent2D& swapChainExtent)
    {
        vk::Rect2D scissor(vk::Offset2D(0, 0), {.width = swapChainExtent.width, .height = swapChainExtent.height});
        
        m_commandBuffers[m_currentFrameIndex].setScissorWithCount(scissor);
    }
    
    void CommandBufferVK::setVertexInput()
    {
        m_commandBuffers[m_currentFrameIndex].setVertexInputEXT({}, {});
    }
    
    void CommandBufferVK::setPrimitiveTopology(const PrimitiveTopology& topology)
    {
        vk::PrimitiveTopology vkTopolgy = translateTopologyToVk(topology);
        m_commandBuffers[m_currentFrameIndex].setPrimitiveTopology(vkTopolgy);
    }
    
    void CommandBufferVK::setPrimitiveRestartEnable(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setPrimitiveRestartEnable(enable);
    }
    
    void CommandBufferVK::setRasterizerDiscardEnable(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setRasterizerDiscardEnable(enable);
    }
    
    void CommandBufferVK::setPolygonMode(const PolygonMode& polygon)
    {
        vk::PolygonMode vkPolygon = translatePolygonToVk(polygon);
        m_commandBuffers[m_currentFrameIndex].setPolygonModeEXT(vkPolygon);
    }
    
    void CommandBufferVK::setCullMode(const CullMode& cullMode)
    {
        vk::CullModeFlagBits vkCullMode = translateCullModeToVk(cullMode);
        m_commandBuffers[m_currentFrameIndex].setCullMode(vkCullMode);
    }
    
    void CommandBufferVK::setFrontFace(const FrontFace& frontFace)
    {
        vk::FrontFace vkFrontFace = translateFrontFaceToVk(frontFace);
        m_commandBuffers[m_currentFrameIndex].setFrontFace(vkFrontFace);
    }
    
    void CommandBufferVK::setDepthBiasEnable(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setDepthBiasEnable(enable);
    }
    
    void CommandBufferVK::setDepthClampEnable(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setDepthClampEnableEXT(enable);
    }
    
    void CommandBufferVK::setLineWidth(float lineWidth)
    {
        m_commandBuffers[m_currentFrameIndex].setLineWidth(lineWidth);
    }
    
    void CommandBufferVK::setRasterizationSamples(uint32_t sampleCount)
    {
        m_commandBuffers[m_currentFrameIndex].setRasterizationSamplesEXT(static_cast<vk::SampleCountFlagBits>(sampleCount));
    }
    
    void CommandBufferVK::setSampleMask(uint32_t sampleCount, uint32_t sampleMask)
    {
        m_commandBuffers[m_currentFrameIndex].setSampleMaskEXT(static_cast<vk::SampleCountFlagBits>(sampleCount), sampleMask);
    }
    
    void CommandBufferVK::setAlphaToCoverageEnable(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setAlphaToCoverageEnableEXT(enable);
    }
    
    void CommandBufferVK::setAlphaToOneEnableEXT(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setAlphaToOneEnableEXT(enable);
    }
    
    void CommandBufferVK::setDepthTestEnable(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setDepthTestEnable(enable);
    }
    
    void CommandBufferVK::setDepthWriteEnable(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setDepthWriteEnable(enable);
    }
    
    void CommandBufferVK::setDepthCompareOp(const CompareOp& compareOp)
    {
        vk::CompareOp vkCompareOp = translateCompareOpToVk(compareOp);
        m_commandBuffers[m_currentFrameIndex].setDepthCompareOp(vkCompareOp);
    }
    
    void CommandBufferVK::setDepthBoundsTestEnable(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setDepthBoundsTestEnable(enable);
    }
    
    void CommandBufferVK::setStencilTestEnable(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setStencilTestEnable(enable);
    }
    
    void CommandBufferVK::setColorBlendEnable(uint32_t firstAttachment, bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setColorBlendEnableEXT(firstAttachment, enable);
    }
    
    void CommandBufferVK::setColorBlendEquation(uint32_t firstAttachment, const ColorBlendEquation& blendEquation)
    {
        vk::ColorBlendEquationEXT vkBlendQuation
        {
            .srcColorBlendFactor = translateBlendFactor(blendEquation.srcColorBlendFactor),
            .dstColorBlendFactor = translateBlendFactor(blendEquation.dstColorBlendFactor),
            .colorBlendOp = translateBlendOp(blendEquation.colorBlendOp),
            .srcAlphaBlendFactor = translateBlendFactor(blendEquation.srcAlphaBlendFactor),
            .dstAlphaBlendFactor = translateBlendFactor(blendEquation.dstAlphaBlendFactor),
            .alphaBlendOp = translateBlendOp(blendEquation.alphaBlendOp),
        };
        m_commandBuffers[m_currentFrameIndex].setColorBlendEquationEXT(firstAttachment, vkBlendQuation);
    }
    
    void CommandBufferVK::setColorWriteMask(uint32_t firstAttachment, uint32_t colorWriteMask)
    {
        vk::ColorComponentFlags vkColorMask(colorWriteMask);
        m_commandBuffers[m_currentFrameIndex].setColorWriteMaskEXT(firstAttachment, vkColorMask);
    }
    
    void CommandBufferVK::setLogicOpEnable(bool enable)
    {
        m_commandBuffers[m_currentFrameIndex].setLogicOpEnableEXT(enable);
    }

    /*void CommandBufferVK::set()
    {
        
    }*/
    
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
    
    void CommandBufferVK::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        m_commandBuffers[m_currentFrameIndex].draw(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void CommandBufferVK::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    {
        m_commandBuffers[m_currentFrameIndex].drawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }
    
    void CommandBufferVK::drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        m_commandBuffers[m_currentFrameIndex].drawMeshTasksEXT(groupCountX, groupCountY, groupCountZ);
    }
    
    void CommandBufferVK::drawMeshTasksIndirect(Buffer& indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        auto& vkBuffer = dynamic_cast<BufferVK&>(indirectBuffer);
        
        m_commandBuffers[m_currentFrameIndex].drawMeshTasksIndirectEXT(vkBuffer.getNativeBuffer(), offset, drawCount, stride);
    }
    
    void CommandBufferVK::dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        m_commandBuffers[m_currentFrameIndex].dispatch(groupCountX, groupCountY, groupCountZ);
    }
    
    void CommandBufferVK::drawMeshTasksIndirect(uint64_t indirectBufferDeviceAddress, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        NOX_CORE_ASSERT("My GPU doesnt support idk why will have to look");
        vk::DrawIndirect2InfoKHR drawInfo = 
        {
            .pNext = nullptr,
            .addressRange = 
            {
                .address = indirectBufferDeviceAddress + offset,
                .size = static_cast<vk::DeviceSize>(drawCount) * stride,
                .stride = stride,
            },
            .addressFlags = {},
            .drawCount = drawCount,
        };
        
        m_commandBuffers[m_currentFrameIndex].drawMeshTasksIndirect2EXT(drawInfo);
    }

    void CommandBufferVK::transitionTextureLayout(Texture& texture, TextureLayout oldLayout, TextureLayout newLayout)
    {
        auto* vkTex = dynamic_cast<TextureVK*>(&texture);

        vk::ImageAspectFlags imageAspectFlags = (vkTex->getUsage() == TextureUsage::DepthStencilAttachment)
                                                    ? vk::ImageAspectFlagBits::eDepth
                                                    : vk::ImageAspectFlagBits::eColor;

        submitImageBarrier(vkTex->getNativeImage(), oldLayout, newLayout, imageAspectFlags, vkTex->getArrayLayers(), vkTex->getMipLevels());
    }

    void CommandBufferVK::transitionSwapchainLayout(Swapchain& swapchain, uint32_t imageIndex, TextureLayout oldLayout, TextureLayout newLayout)
    {
        auto* vkSwapchain = dynamic_cast<SwapchainVK*>(&swapchain);
        vk::Image rawImage = vkSwapchain->getNativeImage(imageIndex);

        submitImageBarrier(rawImage, oldLayout, newLayout, vk::ImageAspectFlagBits::eColor, 1, 1);
    }
    
    void CommandBufferVK::resolveImage(Texture& srcTexture, Texture& dstTexture, uint32_t width, uint32_t height)
    {
        auto* vkSrc = dynamic_cast<TextureVK*>(&srcTexture);
        auto* vkDst = dynamic_cast<TextureVK*>(&dstTexture);
        
        vk::ImageResolve resolveRegion
        {
          .srcSubresource = 
              {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                  .mipLevel = 0,
                  .baseArrayLayer = 0,
                  .layerCount = 1,
              },
            .srcOffset = {0, 0, 0},
            .dstSubresource = 
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
                },
            .dstOffset = {0, 0, 0},
            .extent = { width, height, 1},
        };
        
        m_commandBuffers[m_currentFrameIndex].resolveImage
        (
            vkSrc->getNativeImage(), 
            vk::ImageLayout::eTransferSrcOptimal, 
            vkDst->getNativeImage(), 
            vk::ImageLayout::eTransferDstOptimal, 
            resolveRegion
        );
    }

    void CommandBufferVK::submitImageBarrier(vk::Image image, TextureLayout oldLayout, TextureLayout newLayout, vk::ImageAspectFlags aspectFlags, 
        uint32_t arrayLayers, uint32_t mipLevels)
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
                .levelCount = mipLevels,
                .baseArrayLayer = 0,
                .layerCount = arrayLayers
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
            
        case TextureLayout::TransferSrc:
            stageMask = vk::PipelineStageFlagBits2::eTransfer;
            accessMask = vk::AccessFlagBits2::eTransferRead;
            break;
            
        case TextureLayout::TransferDst:
            stageMask = vk::PipelineStageFlagBits2::eTransfer;
            accessMask = vk::AccessFlagBits2::eTransferWrite;
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
        case TextureLayout::TransferSrc: return vk::ImageLayout::eTransferSrcOptimal;
        case TextureLayout::TransferDst: return vk::ImageLayout::eTransferDstOptimal;
        case TextureLayout::Present: return vk::ImageLayout::ePresentSrcKHR;
        case TextureLayout::General: return vk::ImageLayout::eGeneral;
        default:
            throw std::runtime_error("Unsupported TextureLayout passed to Vulkan backend!");
        }
    }
}
