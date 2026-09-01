#pragma once
#include <cstdint>
#include "Texture.h"
#include "Sampler.h"

namespace NRI
{
    enum class DescriptorHeapType : uint8_t
    {
        Resource, // Holds Buffers and Images
        Sampler   // Holds Sampler states
    };
    
    struct DescriptorHeapDesc
    {
        DescriptorHeapType type;
        uint32_t maxSamplerDescriptors = 0;
        uint32_t maxBufferDescriptors = 0;
        uint32_t maxImageDescriptors = 0;
        std::vector<SamplerDesc> samplers;
    };
    
    class DescriptorHeap
    {
    public:
        virtual ~DescriptorHeap() = default;
        
        // Abstract API-portable runtime registration methods
        virtual void registerTexture(class Texture& texture, TextureUsage usageOverride = TextureUsage::Default) = 0;
        virtual uint32_t registerStorageTextureMip(Texture& texture, uint32_t mipLevel) { return 0; }
        virtual void unregisterTexture(uint32_t slot) = 0;
        virtual uint32_t registerBuffer(class Buffer& buffer, uint64_t size) = 0;
        
        // Expose your uniform offset calculation seamlessly to the Renderer
        virtual uint32_t getImageHeapIndexOffset() const = 0;
        // i saw in vk_minimal_latest he didnt provide to slang this how did he do this 
        
        virtual uint64_t getSize() const = 0;
        virtual uint64_t getReservedRangeOffset() const = 0;
        virtual uint64_t getReservedRangeSize() const = 0;
        virtual uint64_t getGPUAddress() const = 0;
    };
}
