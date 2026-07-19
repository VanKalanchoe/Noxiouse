#include "DescriptorHeapVK.h"
#include "DeviceVK.h"
#include "TextureVK.h"
#include "BufferVK.h"

namespace NRI
{
    uint64_t DescriptorHeapVK::alignSize(uint64_t value, uint64_t alignment) const
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    DescriptorHeapVK::DescriptorHeapVK(DeviceVK& device, const DescriptorHeapDesc& desc)
        : m_deviceVK(device), m_type(desc.type)
    {
        auto deviceProps2 = m_deviceVK.getPhysicalDevice().getProperties2<
            vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
        m_heapProps = deviceProps2.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();

        uint64_t totalSize = 0;

        if (m_type == DescriptorHeapType::Sampler)
        {
            m_samplerDescSize = alignSize(m_heapProps.samplerDescriptorSize, m_heapProps.samplerDescriptorAlignment);
            totalSize = alignSize(m_samplerDescSize * desc.maxSamplerDescriptors + m_heapProps.minSamplerHeapReservedRange, m_heapProps.samplerHeapAlignment);
            m_reservedRangeSize = m_heapProps.minSamplerHeapReservedRange;

            m_reservedRangeOffset = totalSize - m_heapProps.minSamplerHeapReservedRange;
        }
        else // Resource
        {
            m_bufferDescSize = alignSize(m_heapProps.bufferDescriptorSize, m_heapProps.bufferDescriptorAlignment);
            m_imageDescSize = alignSize(m_heapProps.imageDescriptorSize, m_heapProps.imageDescriptorAlignment);

            m_imageHeapOffset = alignSize(desc.maxBufferDescriptors * m_bufferDescSize, m_heapProps.imageDescriptorAlignment);
            m_imageHeapIndexOffset = static_cast<uint32_t>(m_imageHeapOffset / m_imageDescSize);

            totalSize = alignSize(m_imageHeapOffset + (m_imageDescSize * desc.maxImageDescriptors) + m_heapProps.minResourceHeapReservedRange, m_heapProps.resourceHeapAlignment);

            m_reservedRangeSize = m_heapProps.minResourceHeapReservedRange;

            m_reservedRangeOffset =
                totalSize - m_heapProps.minResourceHeapReservedRange;
        }

        // 1. Map allocation calls right into your custom abstraction class!
        m_heapBuffer = m_deviceVK.createBuffer(BufferDesc{
            .size = totalSize,
            .usage = BufferUsage::DescriptorHeap
        });

        // 2. Map it persistently using your wrapper's map API
        m_mappedPtr = m_heapBuffer->map(0, totalSize);

        // Pre-populate samplers if this is a Sampler Heap
        if (m_type == DescriptorHeapType::Sampler)
        {
            vk::PhysicalDeviceProperties properties = m_deviceVK.getPhysicalDevice().getProperties();
            std::array<vk::SamplerCreateInfo, 2> samplerCreateInfos
            {
                vk::SamplerCreateInfo
                {
                    .magFilter = vk::Filter::eLinear,
                    .minFilter = vk::Filter::eLinear,
                    .mipmapMode = vk::SamplerMipmapMode::eLinear,
                    .addressModeU = vk::SamplerAddressMode::eRepeat,
                    .addressModeV = vk::SamplerAddressMode::eRepeat,
                    .addressModeW = vk::SamplerAddressMode::eRepeat,
                    .mipLodBias = 0.0f,
                    .anisotropyEnable = vk::True,
                    .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
                    .compareEnable = vk::False,
                    .compareOp = vk::CompareOp::eAlways,
                    .minLod = 0.0f, // static_cast<float>(mipLevels / 2);
                    .maxLod = vk::LodClampNone
                    /*.maxLod = (float)textures[0].mipLevels,*/
                },
                vk::SamplerCreateInfo
                {
                    .magFilter = vk::Filter::eNearest,
                    .minFilter = vk::Filter::eNearest,
                    .mipmapMode = vk::SamplerMipmapMode::eNearest,
                    .addressModeU = vk::SamplerAddressMode::eRepeat,
                    .addressModeV = vk::SamplerAddressMode::eRepeat,
                    .addressModeW = vk::SamplerAddressMode::eRepeat,
                    .mipLodBias = 0.0f,
                    .anisotropyEnable = vk::True,
                    .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
                    .compareEnable = vk::False,
                    .compareOp = vk::CompareOp::eAlways,
                    .minLod = 0.0f, // static_cast<float>(mipLevels / 2);
                    .maxLod = vk::LodClampNone
                    /*.maxLod = (float)textures[0].mipLevels,*/
                }
            };

            std::array<vk::HostAddressRangeEXT, 2> hostAddressRangesSamplers{};
            for (size_t i = 0; i < samplerCreateInfos.size(); i++)
            {
                hostAddressRangesSamplers[i] = {
                    .address = static_cast<uint8_t*>(m_mappedPtr) + m_samplerDescSize * i,
                    .size = m_samplerDescSize
                };
            }
            m_deviceVK.getDevice().writeSamplerDescriptorsEXT(samplerCreateInfos, hostAddressRangesSamplers);
        }

        m_size = totalSize;
    }

    void DescriptorHeapVK::registerTexture(Texture& texture)
    {
        auto* vkTex = dynamic_cast<TextureVK*>(&texture);
        
        uint32_t slot;
        if (!m_freeImageSlots.empty())
        {
            slot = m_freeImageSlots.back();
            m_freeImageSlots.pop_back();
        }
        else
        {
            slot = m_allocatedImageCount++;
        }

        vk::ImageDescriptorInfoEXT imageDescriptorInfo
        {
            .pView = &vkTex->getNativeViewInfo(),
            .layout = vk::ImageLayout::eShaderReadOnlyOptimal
        };

        vk::ResourceDescriptorInfoEXT info{};
        info.type = vk::DescriptorType::eSampledImage;
        info.data.pImage = &imageDescriptorInfo;


        vk::HostAddressRangeEXT hostRange
        {
            .address = static_cast<uint8_t*>(m_mappedPtr) + m_imageHeapOffset + (m_imageDescSize * slot),
            .size = m_imageDescSize
        };

        m_deviceVK.getDevice().writeResourceDescriptorsEXT(info, hostRange);
        
        vkTex->setDescriptorIndexSlot(slot);
    }

    void DescriptorHeapVK::unregisterTexture(uint32_t slot)
    {
        /*// 1. Create a safe descriptor payload pointing to a null view
        vk::ImageDescriptorInfoEXT nullImageInfo
        {
            .pView = nullptr, //  Null view tells the driver this descriptor is empty
            .layout = vk::ImageLayout::eUndefined
        };

        vk::ResourceDescriptorInfoEXT info{};
        info.type = vk::DescriptorType::eSampledImage;
        info.data.pImage = &nullImageInfo;

        // 2. Locate the exact spot this texture was parked in
        vk::HostAddressRangeEXT hostRange
        {
            .address = static_cast<uint8_t*>(m_mappedPtr) + m_imageHeapOffset + (m_imageDescSize * slot),
            .size = m_imageDescSize
        };

        // 3. Overwrite the memory on the host-mapped pointer
        m_deviceVK.getDevice().writeResourceDescriptorsEXT(info, hostRange);*/

        // 4. Push this index slot back into the recycling bin so another texture can grab it
        m_freeImageSlots.push_back(slot);
    }

    uint64_t DescriptorHeapVK::getBufferDeviceAddress(vk::raii::Buffer& buffer/*vks::Buffer &buffer*/) const
    {
        vk::BufferDeviceAddressInfo addressInfo{.buffer = buffer};

        return m_deviceVK.getDevice().getBufferAddress(addressInfo);
    }

    uint32_t DescriptorHeapVK::registerBuffer(Buffer& buffer, uint64_t size)
    {
        auto* vkBuf = dynamic_cast<BufferVK*>(&buffer);

        uint32_t slot;
        if (!m_freeBufferSlots.empty())
        {
            slot = m_freeBufferSlots.back();
            m_freeBufferSlots.pop_back();
        }
        else
        {
            slot = m_allocatedBufferCount++;
        }

        vk::DeviceAddressRangeEXT addressRange
        {
            .address = getBufferDeviceAddress(vkBuf->getNativeBuffer()), // Utilizing your buffer address tracker
            .size = size
        };

        vk::ResourceDescriptorInfoEXT info{};
        info.type = vk::DescriptorType::eStorageBuffer;
        info.data.pAddressRange = &addressRange;


        vk::HostAddressRangeEXT hostRange{
            .address = static_cast<uint8_t*>(m_mappedPtr) + (m_bufferDescSize * slot),
            .size = m_bufferDescSize
        };

        m_deviceVK.getDevice().writeResourceDescriptorsEXT(info, hostRange);
        return slot;
    }

    void DescriptorHeapVK::unregisterBuffer(uint32_t slot)
    {
        vk::DeviceAddressRangeEXT nullAddressRange{.address = 0, .size = 0};

        vk::ResourceDescriptorInfoEXT info{};
        info.type = vk::DescriptorType::eStorageBuffer;
        info.data.pAddressRange = &nullAddressRange;

        vk::HostAddressRangeEXT hostRange
        {
            .address = static_cast<uint8_t*>(m_mappedPtr) + (m_bufferDescSize * slot),
            .size = m_bufferDescSize
        };

        m_deviceVK.getDevice().writeResourceDescriptorsEXT(info, hostRange);

        m_freeBufferSlots.push_back(slot);
    }
}
