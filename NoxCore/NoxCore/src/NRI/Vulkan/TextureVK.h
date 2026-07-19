#pragma once
#include "../Texture.h"
#include "MemoryAllocatorVK.h"

namespace NRI
{
    struct AllocatedImage;
    class DeviceVK;

    class TextureVK final : public Texture
    {
    public:
        TextureVK(DeviceVK& device, const TextureDesc& desc);
        ~TextureVK() override;

        vma::raii::Image& getNativeImage() { return m_imageResource.image; }
        vk::raii::ImageView& getNativeView() { return m_imageResource.view; }
        vk::ImageViewCreateInfo& getNativeViewInfo() { return m_viewCreateInfo; }
        ImTextureID getImTextureID() override;
        uint32_t getDescriptorIndexSlot() const override { return m_imageResource.descriptorIndexSlot; }
        
        [[nodiscard]] uint32_t getMipLevels() const override { return m_desc.mipLevels; }
        [[nodiscard]] TextureUsage getUsage() const override { return m_desc.usage; }
        void setDescriptorIndexSlot(uint32_t index) { m_imageResource.descriptorIndexSlot = index; }
        
        void uploadFromBuffer(CommandBuffer& cmdBuffer, Buffer& stagingBuffer, uint32_t width, uint32_t height, uint32_t mipLevels) override;
        
    private:
        void generateMipmaps(vk::raii::CommandBuffer& commandBuffer, vk::Format imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels);
        void transitionImageLayout(vk::raii::CommandBuffer& commandBuffer, vk::ImageLayout oldLayout, vk::ImageLayout newLayout, uint32_t mipLevels);
        void copyBufferToImage(vk::raii::CommandBuffer& commandBuffer, const vk::raii::Buffer& buffer, uint32_t width, uint32_t height);

    private:
        DeviceVK& m_deviceVK;
        TextureDesc m_desc;
        ImageResource m_imageResource;
        VkDescriptorSet m_imGuiHandle = nullptr; //imgui only
        
        // Memory-stable structures required for descriptor storage tracking
        vk::ImageViewCreateInfo m_viewCreateInfo;
    };
}