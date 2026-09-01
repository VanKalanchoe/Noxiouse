#pragma once
#include "../Texture.h"
#include "MemoryAllocatorVK.h"

namespace NRI
{
    struct AllocatedImage;
    class DeviceVK;
    class DescriptorHeap;
    
    class TextureVK final : public Texture2D
    {
    public:
        TextureVK(DeviceVK& device, const TextureDesc& desc);
        ~TextureVK() override;
        
        uint32_t GetWidth() const override { return m_desc.width; }
        uint32_t GetHeight() const override { return m_desc.height; }
        uint32_t GetDescriptorIndexSlot() const override { return m_imageResource.descriptorIndexSlot; }

        vma::raii::Image& getNativeImage() { return m_imageResource.image; }
        vk::raii::ImageView& getNativeView() { return m_imageResource.view; }
        vk::ImageViewCreateInfo& getNativeViewInfo() { return m_viewCreateInfo; }
        [[nodiscard]] vk::Format getFormat() const { return m_format; }
        ImTextureID getImTextureID() override;
        
        [[nodiscard]] uint32_t getMipLevels() const override { return m_desc.mipLevels; }
        [[nodiscard]] uint32_t getArrayLayers() const override { return m_desc.arrayLayers; }
        [[nodiscard]] TextureUsage getUsage() const override { return m_desc.usage; }
        void setDescriptorIndexSlot(uint32_t index) { m_imageResource.descriptorIndexSlot = index; }
        void setDescriptorHeap(DescriptorHeap* heap) { m_boundHeap = heap; }
        
        void uploadFromBuffer(CommandBuffer& cmdBuffer, Buffer& stagingBuffer, uint32_t width, uint32_t height, uint32_t mipLevels, const std::vector<size_t>& mipOffsets) override;
        void copyImageToBuffer(CommandBuffer& commandBuffer, Buffer& dstBuffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
        void generateMipmaps(CommandBuffer& commandBuffer) override;
    private:
        void generateMipmaps(vk::raii::CommandBuffer& commandBuffer, vk::Format imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels);
        void transitionImageLayout(vk::raii::CommandBuffer& commandBuffer, vk::ImageLayout oldLayout, vk::ImageLayout newLayout, uint32_t mipLevels);
        void copyBufferToImage(vk::raii::CommandBuffer& commandBuffer, const vk::raii::Buffer& buffer, std::vector<vk::BufferImageCopy>& regions);

    private:
        DeviceVK& m_deviceVK;
        TextureDesc m_desc;
        ImageResource m_imageResource;
        VkDescriptorSet m_imGuiHandle = nullptr; //imgui only
        DescriptorHeap* m_boundHeap = nullptr;
        vk::Format m_format;
        
        // Memory-stable structures required for descriptor storage tracking
        vk::ImageViewCreateInfo m_viewCreateInfo;
    };
}