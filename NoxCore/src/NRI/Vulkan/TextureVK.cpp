#include "TextureVK.h"

#include "DeviceVK.h"
#include "CommandBufferVK.h"
#include "BufferVK.h"

namespace NRI
{
    TextureVK::TextureVK(DeviceVK& device, const TextureDesc& desc) : m_deviceVK(device)
    {
        m_desc = desc; // used for image transition layout in commandbuffer colorattachments
        
        // Assumes there is only color depth and shader aka png textures
        vk::Format format;
        vk::ImageAspectFlags aspectFlags;
        vk::ImageUsageFlags usageFlags;
        if (desc.usage == TextureUsage::ShaderResource)
        {
            format = vk::Format::eR8G8B8A8Srgb;
            aspectFlags = vk::ImageAspectFlagBits::eColor;
            usageFlags = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
        }
        else if (desc.usage == TextureUsage::ColorAttachment)
        {
            format = m_deviceVK.getSurfaceFormat().format;
            aspectFlags = vk::ImageAspectFlagBits::eColor;
            usageFlags = vk::ImageUsageFlagBits::eTransientAttachment | vk::ImageUsageFlagBits::eColorAttachment;
        }
        else if (desc.usage == TextureUsage::DepthStencilAttachment)
        {
            format = m_deviceVK.getDepthFormat();
            aspectFlags = vk::ImageAspectFlagBits::eDepth;
            usageFlags = vk::ImageUsageFlagBits::eDepthStencilAttachment;
        }

        vk::ImageCreateInfo imageInfo
        {
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {desc.width, desc.height, 1},
            .mipLevels = desc.mipLevels,
            .arrayLayers = 1,
            .samples = static_cast<vk::SampleCountFlagBits>(desc.sampleCount), // might be a problem converting uint32_t to vk::samplecountflagbits
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usageFlags,
            .sharingMode = vk::SharingMode::eExclusive
        };
        m_imageResource.image = m_deviceVK.getAllocator().createImage(imageInfo).image;
        
        m_viewCreateInfo = 
        {
            .image = *m_imageResource.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = 
            {
                .aspectMask = aspectFlags,
                .baseMipLevel = 0,
                .levelCount = desc.mipLevels,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        if (desc.usage == TextureUsage::ColorAttachment || desc.usage == TextureUsage::DepthStencilAttachment)
        {
            m_imageResource.view = m_deviceVK.createImageView(*m_imageResource.image, format, aspectFlags, 1);
        }
    }

    void TextureVK::uploadFromBuffer(CommandBuffer& cmdBuffer, Buffer& stagingBuffer, uint32_t width, uint32_t height, uint32_t mipLevels)
    {
        auto* cmdBufferVK = dynamic_cast<CommandBufferVK*>(&cmdBuffer);
        auto* stagingBufferVK = dynamic_cast<BufferVK*>(&stagingBuffer);

        vk::raii::CommandBuffer& cb = cmdBufferVK->getNativeBuffer(0);
        vk::raii::Buffer& nativeBuffer = stagingBufferVK->getNativeBuffer();

        transitionImageLayout(cb, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, mipLevels);
        copyBufferToImage(cb, nativeBuffer, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        generateMipmaps(cb, vk::Format::eR8G8B8A8Srgb, width, height, mipLevels);
    }

    void TextureVK::generateMipmaps(vk::raii::CommandBuffer& commandBuffer, vk::Format imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels)
    {
        // Check if image format supports linear blit-ing
        vk::FormatProperties formatProperties = m_deviceVK.getPhysicalDevice().getFormatProperties(imageFormat);

        if (!(formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear))
        {
            throw std::runtime_error("texture image format does not support linear blitting!");
        }

        vk::ImageMemoryBarrier barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite, .dstAccessMask = vk::AccessFlagBits::eTransferRead, .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal, .srcQueueFamilyIndex = vk::QueueFamilyIgnored, .dstQueueFamilyIndex = vk::QueueFamilyIgnored, .image = m_imageResource.image
        };
        barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.subresourceRange.levelCount = 1;

        int32_t mipWidth = texWidth;
        int32_t mipHeight = texHeight;

        for (uint32_t i = 1; i < mipLevels; i++)
        {
            barrier.subresourceRange.baseMipLevel = i - 1;
            barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
            barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;

            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);

            vk::ArrayWrapper1D<vk::Offset3D, 2> offsets, dstOffsets;
            offsets[0] = vk::Offset3D(0, 0, 0);
            offsets[1] = vk::Offset3D(mipWidth, mipHeight, 1);
            dstOffsets[0] = vk::Offset3D(0, 0, 0);
            dstOffsets[1] = vk::Offset3D(mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1);
            vk::ImageBlit blit = {.srcSubresource = {}, .srcOffsets = offsets, .dstSubresource = {}, .dstOffsets = dstOffsets};
            blit.srcSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, i - 1, 0, 1);
            blit.dstSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, i, 0, 1);

            commandBuffer.blitImage(m_imageResource.image, vk::ImageLayout::eTransferSrcOptimal, m_imageResource.image, vk::ImageLayout::eTransferDstOptimal, {blit}, vk::Filter::eLinear);

            barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
            barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;

            commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, barrier);

            if (mipWidth > 1)
                mipWidth /= 2;
            if (mipHeight > 1)
                mipHeight /= 2;
        }

        barrier.subresourceRange.baseMipLevel = mipLevels - 1;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;

        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, barrier);
    }

    void TextureVK::transitionImageLayout(vk::raii::CommandBuffer& commandBuffer, vk::ImageLayout oldLayout, vk::ImageLayout newLayout, uint32_t mipLevels)
    {
        vk::ImageMemoryBarrier barrier{
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = m_imageResource.image,
            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = mipLevels, .layerCount = 1}
        };

        vk::PipelineStageFlags sourceStage;
        vk::PipelineStageFlags destinationStage;

        if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eTransferDstOptimal)
        {
            barrier.srcAccessMask = {};
            barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;

            sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
            destinationStage = vk::PipelineStageFlagBits::eTransfer;
        }
        else if (oldLayout == vk::ImageLayout::eTransferDstOptimal && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
        {
            barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;

            sourceStage = vk::PipelineStageFlagBits::eTransfer;
            destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
        }
        else
        {
            throw std::invalid_argument("unsupported layout transition!");
        }
        commandBuffer.pipelineBarrier(sourceStage, destinationStage, {}, {}, {}, barrier);
    }

    void TextureVK::copyBufferToImage(vk::raii::CommandBuffer& commandBuffer, const vk::raii::Buffer& buffer, uint32_t width, uint32_t height)
    {
        vk::BufferImageCopy region
        {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, 1}
        };
        commandBuffer.copyBufferToImage(buffer, m_imageResource.image, vk::ImageLayout::eTransferDstOptimal, region);
    }
}
