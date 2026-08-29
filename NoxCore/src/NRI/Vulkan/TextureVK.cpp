#include "TextureVK.h"

#include <imgui_impl_vulkan.h>

#include "DeviceVK.h"
#include "CommandBufferVK.h"
#include "BufferVK.h"
#include "NoxCore/Core/core.h"

namespace NRI
{   
    static vk::Format MapToVulkanFormat(ImageFormat format)
    {
        switch (format)
        {
        case ImageFormat::RGB8: return vk::Format::eR8G8B8A8Unorm;
        case ImageFormat::SRGB8: return vk::Format::eR8G8B8A8Srgb;
            
        case ImageFormat::RGBA8:  return vk::Format::eR8G8B8A8Unorm;
        case ImageFormat::SRGBA8: return vk::Format::eR8G8B8A8Srgb;
            
        case ImageFormat::R16G16: return vk::Format::eR16G16Unorm;
        case ImageFormat::R32SINT: return vk::Format::eR32Sint;
            
        case ImageFormat::R16G16B16A16_SFLOAT: return vk::Format::eR16G16B16A16Sfloat;
        case ImageFormat::R32G32B32A32_SFLOAT: return vk::Format::eR32G32B32A32Sfloat;
            
        //tinyddsloader format
        case ImageFormat::BC7_UNorm: return vk::Format::eBc7UnormBlock;
        case ImageFormat::BC7_UNorm_SRGB: return vk::Format::eBc7SrgbBlock;
            
        default: return vk::Format::eR8G8B8A8Srgb;
        }
    }
    
    static vk::Format MapToVulkanFormat(uint32_t format)
    {
        // For KTX2 files, we can get the format directly
        vk::Format textureFormat = static_cast<vk::Format>(format);
        if (textureFormat == vk::Format::eUndefined)
        {
            // If the format is undefined, fall back to a reasonable default
            textureFormat = vk::Format::eR8G8B8A8Unorm;
        }
        
        return textureFormat;
    }
    
    TextureVK::TextureVK(DeviceVK& device, const TextureDesc& desc) : m_deviceVK(device)
    {
        m_desc = desc; // used for image transition layout in commandbuffer colorattachments

        // Assumes there is only color depth and shader aka png textures
        vk::Format format;
        vk::ImageAspectFlags aspectFlags;
        vk::ImageUsageFlags usageFlags;
        if (desc.usage == TextureUsage::ShaderResource)
        {
            format = desc.directFormat == UINT32_MAX ? MapToVulkanFormat(desc.format) : MapToVulkanFormat(desc.directFormat)/*vk::Format::eR8G8B8A8Srgb*/;
            aspectFlags = vk::ImageAspectFlagBits::eColor;
            usageFlags = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
        }
        else if (desc.usage == TextureUsage::ColorAttachment)
        {
            format = desc.format == ImageFormat::Surface ? m_deviceVK.getSurfaceFormat().format : MapToVulkanFormat(desc.format);
            aspectFlags = vk::ImageAspectFlagBits::eColor;
            usageFlags = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
        }
        else if (desc.usage == TextureUsage::ColorResolveAttachment)
        {
            format = desc.format == ImageFormat::Surface ? m_deviceVK.getSurfaceFormat().format : MapToVulkanFormat(desc.format);
            aspectFlags = vk::ImageAspectFlagBits::eColor;
            usageFlags = vk::ImageUsageFlagBits::eTransientAttachment | vk::ImageUsageFlagBits::eColorAttachment;
        }
        else if (desc.usage == TextureUsage::DepthStencilAttachment)
        {
            format = m_deviceVK.getDepthFormat();
            aspectFlags = vk::ImageAspectFlagBits::eDepth;
            usageFlags = vk::ImageUsageFlagBits::eDepthStencilAttachment;
        }
        else if (desc.usage == TextureUsage::Storage)
        {
            format = desc.directFormat == UINT32_MAX ? MapToVulkanFormat(desc.format) : MapToVulkanFormat(desc.directFormat);
            aspectFlags = vk::ImageAspectFlagBits::eColor;
            usageFlags = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
        }
        
        m_format = format;
        
        vk::ImageCreateFlags createFlags{};
        if (desc.isCubeMap)
        {
            createFlags |= vk::ImageCreateFlagBits::eCubeCompatible;
        }

        vk::ImageCreateInfo imageInfo
        {
            .flags = createFlags,
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {desc.width, desc.height, 1},
            .mipLevels = desc.mipLevels,
            .arrayLayers = desc.arrayLayers,
            .samples = static_cast<vk::SampleCountFlagBits>(desc.sampleCount), // might be a problem converting uint32_t to vk::samplecountflagbits
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usageFlags,
            .sharingMode = vk::SharingMode::eExclusive
        };
        m_imageResource.image = m_deviceVK.getAllocator().createImage(imageInfo).image;

        vk::ImageViewType viewType = vk::ImageViewType::e2D;
        if (desc.isCubeMap)
        {
            viewType = vk::ImageViewType::eCube;
        }
        else if (desc.arrayLayers > 1)
        {
            viewType = vk::ImageViewType::e2DArray;
        }
        
        m_viewCreateInfo =
        {
            .image = *m_imageResource.image,
            .viewType = viewType,
            .format = format,
            .subresourceRange =
            {
                .aspectMask = aspectFlags,
                .baseMipLevel = 0,
                .levelCount = desc.mipLevels,
                .baseArrayLayer = 0,
                .layerCount = desc.arrayLayers
            }
        };

        // because of imgui needing a view to i guess giving all textures the view is fine ? since that means all textures need it no exception sad
        /*if (desc.usage == TextureUsage::ColorAttachment || desc.usage == TextureUsage::ColorResolveAttachment || desc.usage == TextureUsage::DepthStencilAttachment)
        */
        {
            m_imageResource.view = m_deviceVK.createImageView(*m_imageResource.image, format, aspectFlags, 1);
        }
    }

    TextureVK::~TextureVK()
    {
        if (m_boundHeap && m_imageResource.descriptorIndexSlot != ~0u)
        {
            m_boundHeap->unregisterTexture(m_imageResource.descriptorIndexSlot);
        }
        
        if (!Nox::IsEngineShuttingDown && m_deviceVK.isDeviceInit() && m_imGuiHandle != VK_NULL_HANDLE)
        {
            ImGui_ImplVulkan_RemoveTexture(m_imGuiHandle);
            m_imGuiHandle = VK_NULL_HANDLE;
        }
    }

    void TextureVK::uploadFromBuffer(CommandBuffer& cmdBuffer, Buffer& stagingBuffer, uint32_t width, uint32_t height, uint32_t mipLevels, const std::vector<size_t>& mipOffsets)
    {
        auto* cmdBufferVK = dynamic_cast<CommandBufferVK*>(&cmdBuffer);
        auto* stagingBufferVK = dynamic_cast<BufferVK*>(&stagingBuffer);

        vk::raii::CommandBuffer& cb = cmdBufferVK->getNativeBuffer(0);
        vk::raii::Buffer& nativeBuffer = stagingBufferVK->getNativeBuffer();

        transitionImageLayout(cb, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, mipLevels);
        
        std::vector<vk::BufferImageCopy> copyRegions;
        
        if (mipOffsets.empty() || mipOffsets.size() <= 1)
        {
            // STB Fallback: Only one base level provided
            copyRegions.push_back(vk::BufferImageCopy
            {
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource = { .aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
                .imageOffset = {0, 0, 0},
                .imageExtent = { width, height, 1 }
            });
        }
        else
        {
            // DDS/KTX Path: Multiple pre-calculated levels provided
            for (uint32_t i = 0; i < mipOffsets.size(); i++)
            {
                copyRegions.push_back(vk::BufferImageCopy
                {
                    .bufferOffset = mipOffsets[i],
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = { .aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = i, .baseArrayLayer = 0, .layerCount = 1 },
                    .imageOffset = {0, 0, 0},
                    .imageExtent = { width >> i, height >> i, 1 }
                });
            }
        }
        copyBufferToImage(cb, nativeBuffer, copyRegions);
        
        if (mipLevels > 1 && copyRegions.size() == 1)
            generateMipmaps(cb, m_format, width, height, mipLevels);
        else
            transitionImageLayout(cb, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal, mipLevels);
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
            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = mipLevels, .layerCount = m_desc.arrayLayers}
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

    void TextureVK::copyBufferToImage(vk::raii::CommandBuffer& commandBuffer, const vk::raii::Buffer& buffer, std::vector<vk::BufferImageCopy>& regions)
    {
        commandBuffer.copyBufferToImage(buffer, m_imageResource.image, vk::ImageLayout::eTransferDstOptimal, regions);
    }
    
    void TextureVK::copyImageToBuffer(CommandBuffer& commandBuffer, Buffer& dstBuffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
    {
        auto* cmdBufferVK = dynamic_cast<CommandBufferVK*>(&commandBuffer);
        vk::raii::CommandBuffer& cb = cmdBufferVK->getActiveNativeBuffer();
        
        auto* dstBufferVK = dynamic_cast<BufferVK*>(&dstBuffer);
        vk::raii::Buffer& nativeBuffer = dstBufferVK->getNativeBuffer();
        
        vk::BufferImageCopy region = 
        {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = { .aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
            .imageOffset = { static_cast<int32_t>(x), static_cast<int32_t>(y), 0 },
            .imageExtent = { width, height, 1 }
        };
        
        cb.copyImageToBuffer(m_imageResource.image, vk::ImageLayout::eTransferSrcOptimal, nativeBuffer, region);
    }

    ImTextureID TextureVK::getImTextureID()
    {
        if (m_imGuiHandle != VK_NULL_HANDLE)
            return reinterpret_cast<ImTextureID>(m_imGuiHandle);
        
        // Check if the wrapper object itself is valid
        if (!*m_imageResource.view) {
            throw std::runtime_error("m_imageResource.view is null!");
        }
    
        if (m_imGuiHandle == VK_NULL_HANDLE)
        {
#if IMGUI_VERSION_NUM >= 19280
            // Now it is safe to dereference no sampler needeed anymore
            m_imGuiHandle = ImGui_ImplVulkan_AddTexture(
                *m_imageResource.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            );
#elif IMGUI_VERSION_NUM >= 19250
            vk::SamplerCreateInfo samplerInfo{
                .magFilter = vk::Filter::eLinear,
                .minFilter = vk::Filter::eLinear,
                .mipmapMode = vk::SamplerMipmapMode::eLinear,
                .addressModeU = vk::SamplerAddressMode::eRepeat,
                .addressModeV = vk::SamplerAddressMode::eRepeat,
                .addressModeW = vk::SamplerAddressMode::eRepeat
            };

            vk::raii::Sampler sampler(m_deviceVK.getDevice(), samplerInfo);
            // Now it is safe to dereference no sampler needeed anymore
            m_imGuiHandle = ImGui_ImplVulkan_AddTexture(*sampler,
                *m_imageResource.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            );
#endif
        }
    
        return reinterpret_cast<ImTextureID>(m_imGuiHandle);
    }
}
