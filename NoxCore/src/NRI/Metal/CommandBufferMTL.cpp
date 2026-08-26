#include "CommandBufferMTL.h"

#include "DeviceMTL.h"
#include "SwapchainMTL.h"
#include "CommandAllocatorMTL.h"
#include "TextureMTL.h"

namespace NRI
{
    CommandBufferMTL::CommandBufferMTL(DeviceMTL& device, CommandAllocatorMTL& allocator, uint32_t cbCount) : m_deviceMTL(device), m_allocatorMTL(allocator)
    {
        m_commandBuffers.resize(cbCount);
        
        for (uint32_t i = 0; i < cbCount; i++)
        {
            m_commandBuffers[i] = m_deviceMTL.getDevice()->newCommandBuffer();
            
            if(!m_commandBuffers[i])
            {
                throw std::runtime_error("Failed to create Metal command buffer");
            }
        }
    }

    void CommandBufferMTL::begin(uint32_t index, bool oneTimeSubmit)
    {
        m_currentFrameIndex = index;
        
        m_renderEncoder = nullptr;
        m_computeEncoder = nullptr;
        
        MTL4::CommandAllocator* allocator = m_allocatorMTL.getNativeAllocator(index);
        
        allocator->reset();
        
        m_commandBuffers[index]->beginCommandBuffer(allocator);
    }

    void CommandBufferMTL::end(uint32_t index)
    {
        assert(index == m_currentFrameIndex && "Ending wrong Metal commandBuffer slot");
        
        if(m_renderEncoder)
        {
            m_renderEncoder->endEncoding();
            m_renderEncoder = nullptr;
        }
        
        if (m_computeEncoder)
        {
            m_computeEncoder->endEncoding();
            m_computeEncoder = nullptr;
        }
        
        m_commandBuffers[index]->endCommandBuffer();
    }

    void CommandBufferMTL::beginRendering(RenderDesc& desc)
    {
        MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
        
        for(uint32_t i = 0; i < desc.colorAttachments.size(); i++)
        {
            const auto& color = desc.colorAttachments[i];
            
            MTL::RenderPassColorAttachmentDescriptor* attachment = pass->colorAttachments()->object(i);
            
            MTL::Texture* texture = nullptr;
            
            if(color.attachment)
            {
                auto* tex = static_cast<TextureMTL*>(color.attachment);
                
                texture = tex->getNativeTexture();
            }
            else if(color.attachmentSwapchain)
            {
                auto* swapchain = static_cast<SwapchainMTL*>(color.attachmentSwapchain);
                // ImageIndex not needed since metal4 manages the swapchain
                texture = swapchain->getNativeTexture();
            }
            else if(color.resolve)
            {
                auto* resolve = static_cast<TextureMTL*>(color.resolve);
                
                attachment->setResolveTexture(resolve->getNativeTexture());
                
                attachment->setStoreAction(MTL::StoreAction::StoreActionStoreAndMultisampleResolve);
            }
            
            else if(color.resolveSwapchain)
            {
                auto* swapchain = static_cast<SwapchainMTL*>(color.resolveSwapchain);
                // ImageIndex not needed since metal4 manages the swapchain
                attachment->setResolveTexture(swapchain->getNativeTexture());
                
                attachment->setStoreAction(MTL::StoreAction::StoreActionStoreAndMultisampleResolve);
            }
            
            attachment->setTexture(texture);
            
            switch (LoadOP::load)
            {
                case LoadOP::load:
                    attachment->setLoadAction(MTL::LoadAction::LoadActionLoad);
                    break;
                case LoadOP::clear:
                    attachment->setLoadAction(MTL::LoadAction::LoadActionClear);
                    attachment->setClearColor(MTL::ClearColor::Make(color.clearColor.r, color.clearColor.g, color.clearColor.b, color.clearColor.a));
                    break;
                case LoadOP::dontCare:
                    attachment->setLoadAction(MTL::LoadAction::LoadActionDontCare);
                    break;
                    
                default:
                    break;
            }
            
            switch (color.storeOP)
                    {
                    case StoreOP::store:
                        attachment->setStoreAction(
                            MTL::StoreAction::StoreActionStore);
                        break;

                    case StoreOP::dontCare:
                        attachment->setStoreAction(
                            MTL::StoreAction::StoreActionDontCare);
                        break;
                    }
                }
        
        // Depth
        if(desc.depthAttachment.attachment)
        {
            auto* depth =
                        static_cast<TextureMTL*>(
                            desc.depthAttachment.attachment);

                    auto* depthAttachment =
                        pass->depthAttachment();

                    depthAttachment->setTexture(
                        depth->getNativeTexture());

                    switch (desc.depthAttachment.loadOP)
                    {
                    case LoadOP::load:
                        depthAttachment->setLoadAction(
                            MTL::LoadAction::LoadActionLoad);
                        break;

                    case LoadOP::clear:
                        depthAttachment->setLoadAction(
                            MTL::LoadAction::LoadActionClear);

                        depthAttachment->setClearDepth(
                            desc.depthAttachment.clearDepth.depth);
                        break;

                    case LoadOP::dontCare:
                        depthAttachment->setLoadAction(
                            MTL::LoadAction::LoadActionDontCare);
                        break;
                    }

                    switch (desc.depthAttachment.storeOP)
                    {
                    case StoreOP::store:
                        depthAttachment->setStoreAction(
                            MTL::StoreAction::StoreActionStore);
                        break;

                    case StoreOP::dontCare:
                        depthAttachment->setStoreAction(
                            MTL::StoreAction::StoreActionDontCare);
                        break;
                    }
        }
        m_renderEncoder = m_commandBuffers[m_currentFrameIndex]->renderCommandEncoder(pass);
        
        pass->release();
        
        if(!m_renderEncoder)
            throw std::runtime_error("Failed to create Metal render encoder");
    }

    void CommandBufferMTL::endRendering()
    {
        assert(m_renderEncoder);
        
        m_renderEncoder->endEncoding();
        m_renderEncoder = nullptr;
    }

void CommandBufferMTL::bindPipeline(
    PipelineBindPoint bindPoint,
    Pipeline& pipeline)
{
    auto* metalPipeline =
        static_cast<PipelineMTL*>(&pipeline);

    switch (bindPoint)
    {
    case PipelineBindPoint::Graphics:
        m_renderEncoder->setRenderPipelineState(
            metalPipeline->getRenderPipelineState());
        break;

    case PipelineBindPoint::Compute:
        m_computeEncoder->setComputePipelineState(
            metalPipeline->getComputePipelineState());
        break;

    default:
        throw std::runtime_error(
            "Unsupported Metal pipeline bind point");
    }
}

void CommandBufferMTL::setViewport(
    Extent2D extent)
{
    MTL::Viewport viewport{};

    viewport.originX = 0.0;
    viewport.originY = 0.0;

    viewport.width =
        static_cast<double>(extent.width);

    viewport.height =
        static_cast<double>(extent.height);

    viewport.znear = 0.0;
    viewport.zfar = 1.0;

    m_renderEncoder->setViewport(viewport);
}

void CommandBufferMTL::setScissor(
    Extent2D extent)
{
    MTL::ScissorRect rect{};

    rect.x = 0;
    rect.y = 0;
    rect.width = extent.width;
    rect.height = extent.height;

    m_renderEncoder->setScissorRect(rect);
}

void CommandBufferMTL::setViewportWithCount(
    const ViewportBounds& bounds,
    float minDepth,
    float maxDepth)
{
    MTL::Viewport viewport{};

    viewport.originX = bounds.x;
    viewport.originY = bounds.y;
    viewport.width   = bounds.width;
    viewport.height  = bounds.height;
    viewport.znear   = minDepth;
    viewport.zfar    = maxDepth;

    m_renderEncoder->setViewport(viewport);
}

void CommandBufferMTL::draw(
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance)
{
    m_renderEncoder->drawPrimitives(
        MTL::PrimitiveType::PrimitiveTypeTriangle,

        firstVertex,
        vertexCount,
        instanceCount,
        firstInstance);
}

void CommandBufferMTL::drawIndexed(
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t vertexOffset,
    uint32_t firstInstance)
{
    auto* indexBuffer = m_boundIndexBuffer;

    m_renderEncoder->drawIndexedPrimitives(
        MTL::PrimitiveType::PrimitiveTypeTriangle,

        indexCount,

        MTL::IndexType::IndexTypeUInt32,

        indexBuffer->gpuAddress() +
            m_indexBufferOffset +
            static_cast<uint64_t>(firstIndex) * 4,

        m_indexBufferSize,

        instanceCount,

        vertexOffset,

        firstInstance);
}

void CommandBufferMTL::drawMeshTasks(
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ)
{
    m_renderEncoder->drawMeshThreadgroups(
        MTL::Size::Make(
            groupCountX,
            groupCountY,
            groupCountZ),

        MTL::Size::Make(
            1,
            1,
            1),

        MTL::Size::Make(
            32,
            1,
            1));
}

void CommandBufferMTL::drawMeshTasksIndirect(
    Buffer& indirectBuffer,
    uint64_t offset,
    uint32_t drawCount,
    uint32_t stride)
{
    auto& buffer =
        static_cast<BufferMTL&>(indirectBuffer);

    if (drawCount != 1)
    {
        throw std::runtime_error(
            "Current Metal implementation expects one indirect mesh dispatch");
    }

    m_renderEncoder->drawMeshThreadgroups(
        buffer.getGPUAddress() + offset,

        MTL::Size::Make(
            1,
            1,
            1),

        MTL::Size::Make(
            32,
            1,
            1));
}

void CommandBufferMTL::copyBuffer(
    Buffer& srcBuffer,
    Buffer& dstBuffer,
    const BufferCopyRegion& region)
{
    auto& src =
        static_cast<BufferMTL&>(srcBuffer);

    auto& dst =
        static_cast<BufferMTL&>(dstBuffer);

    uint64_t copySize = region.size;

    if (copySize == 0)
    {
        copySize =
            std::min(
                src.getSize() - region.srcOffset,
                dst.getSize() - region.dstOffset);
    }

    MTL4::ComputeCommandEncoder* encoder =
        m_commandBuffers[m_currentFrameIndex]
            ->computeCommandEncoder();

    encoder->copyFromBuffer(
        src.getNativeBuffer(),
        region.srcOffset,

        dst.getNativeBuffer(),
        region.dstOffset,

        copySize);

    encoder->endEncoding();
}


}
