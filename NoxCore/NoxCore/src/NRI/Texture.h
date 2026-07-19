#pragma once
#include <cstdint>
#include <memory>

#include <imgui.h>

namespace NRI
{
    enum class TextureLayout : uint8_t
    {
        Undefined,
        ColorAttachment,
        DepthAttachment,
        ShaderResource,
        Present
    };
    
    enum class TextureUsage : uint8_t
    {
        ColorAttachment,
        DepthStencilAttachment,
        ShaderResource, // For regular textures
        ColorResolveAttachment,
    };

    struct TextureDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 1;
        uint32_t sampleCount = 1; // For MSAA
        TextureUsage usage;
    };

    class Texture
    {
    public:
        virtual ~Texture() = default;
        
        virtual void uploadFromBuffer(class CommandBuffer& cmdBuffer, class Buffer& stagingBuffer, uint32_t width, uint32_t height, uint32_t mipLevels) = 0;
        
        [[nodiscard]] virtual uint32_t getMipLevels() const = 0;
        [[nodiscard]] virtual TextureUsage getUsage() const = 0;
        
        virtual uint32_t getDescriptorIndexSlot() const = 0;
        virtual ImTextureID getImTextureID() = 0;
    };
}