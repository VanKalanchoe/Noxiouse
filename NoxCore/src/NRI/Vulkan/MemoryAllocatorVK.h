#pragma once
#include <optional>

#include "DeviceVK.h"
#include "vk_mem_alloc_raii.hpp"

namespace NRI
{
    class DeviceVK;

    struct AllocatedBuffer
    {
        vma::raii::Buffer buffer { nullptr };
        vk::DeviceAddress address = 0;
        uint64_t size = 0;
    };

    struct AllocatedImage
    {
        vma::raii::Image image { nullptr };
    };
    
    struct ImageResource : AllocatedImage
    {
        vk::raii::ImageView view { nullptr };
        uint32_t descriptorIndexSlot = ~0u;
    };

    class MemoryAllocatorVK
    {
    public:
        MemoryAllocatorVK(DeviceVK& device) : m_deviceVK(device)
        {
            vma::AllocatorCreateInfo allocatorInfo{};
            allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;
            allocatorInfo.physicalDevice = m_deviceVK.getPhysicalDevice();
            allocatorInfo.flags = vma::AllocatorCreateFlagBits::eBufferDeviceAddress |
                vma::AllocatorCreateFlagBits::eKhrMaintenance4 |
                vma::AllocatorCreateFlagBits::eKhrMaintenance5;

            m_allocator = vma::raii::Allocator(m_deviceVK.getInstance(), m_deviceVK.getDevice(), allocatorInfo);
        }

        ~MemoryAllocatorVK()
        {
            freeStagingBuffers();
            if (m_allocator)
            {
                m_allocator.reset();
            }
        }

        [[nodiscard]] AllocatedBuffer createBuffer
        (
            vk::DeviceSize size,
            vk::BufferUsageFlags2 usage,
            vma::MemoryUsage memoryUsage = vma::MemoryUsage::eAuto,
            vma::AllocationCreateFlags flags = {},
            vk::DeviceSize minAlignment = {}
        )
        {
            const bool wantsAddress = (usage & vk::BufferUsageFlagBits2::eShaderDeviceAddress) != vk::BufferUsageFlags2{};

            // Maintenance 5 64-bit usage chain structure
            vk::BufferUsageFlags2CreateInfo bufferUsageFlags2CreateInfo
            {
                .usage = usage
            };

            vk::BufferCreateInfo bufferInfo
            {
                .pNext = &bufferUsageFlags2CreateInfo,
                .size = size,
                .usage = vk::BufferUsageFlags{},
                .sharingMode = vk::SharingMode::eExclusive // Only one queue family will access it
            };

            vma::AllocationCreateInfo allocInfo{.flags = flags, .usage = memoryUsage};

            AllocatedBuffer resultBuffer;
            vma::AllocationInfo allocInfoOut{};
            resultBuffer.buffer = m_allocator->createBufferWithAlignment(bufferInfo, allocInfo, minAlignment, allocInfoOut);

            if (wantsAddress)
            {
                vk::BufferDeviceAddressInfo addressInfo
                {
                    .buffer = resultBuffer.buffer
                };
                resultBuffer.address = m_deviceVK.getDevice().getBufferAddress(addressInfo);
            }

            {
                // Find leaks
                static uint32_t counter = 0U;
                if (m_leakID == counter)
                {
#if defined(_MSVC_LANG)
                    __debugbreak();
#endif
                }
                std::string allocID = std::string("allocID: ") + std::to_string(counter++);
                resultBuffer.buffer.getAllocation().setName(allocID.c_str());
            }

            resultBuffer.size = size;

            return resultBuffer;
        }
        
        /*--
        * Create a staging buffer, copy data into it, and track it.
        * This method accepts data, handles the mapping, copying, and unmapping
        * automatically.
        -*/
        std::shared_ptr<AllocatedBuffer> createStagingBuffer(const void* vectorData, uint64_t bufferSize)
        {
            // Create a staging buffer (host-visible, CPU-writes-then-GPU-reads).
            AllocatedBuffer stagingBuffer = createBuffer(bufferSize, vk::BufferUsageFlagBits2::eTransferSrc, vma::MemoryUsage::eAuto,
                                                         vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);

            // Map and copy data to the staging buffer
            void* data = stagingBuffer.buffer.getAllocation().map();
            memcpy(data, vectorData, (size_t)bufferSize);
            stagingBuffer.buffer.getAllocation().unmap();

            // Track the staging buffer for later cleanup
            auto sharedBuffer = std::make_shared<AllocatedBuffer>(std::move(stagingBuffer));
            m_stagingBuffers.push_back(sharedBuffer);

            return sharedBuffer;
        }
        
        /*--
        * Upload a buffer (GPU only) with data, this is done using a staging buffer
        * The staging buffer is a buffer that is used to transfer data from the CPU
        * to the GPU.
        * and cannot be freed until the data is transferred. So the command buffer
        * must be submitted, then
        * the staging buffer can be cleared using the freeStagingBuffers function.
        -*/
        void uploadBufferData
        (
            vk::raii::CommandBuffer& cmd,
            vma::raii::Buffer& dstBuffer,
            const void* vectorData,
            uint64_t bufferSize
        )
        {
            // Create staging buffer and upload data
            auto stagingBuffer = createStagingBuffer(vectorData, bufferSize);

            const std::array<vk::BufferCopy, 1> copyRegion{{{.size = bufferSize}}};
            cmd.copyBuffer(stagingBuffer->buffer, dstBuffer, copyRegion);
        }
        
        /*--
        * Create an image in GPU memory. This does not adding data to the image.
        * This is only creating the image in GPU memory.
        * See createImageAndUploadData for creating an image and uploading data.
        -*/
        AllocatedImage createImage(const vk::ImageCreateInfo& imageInfo)
        {
            const vma::AllocationCreateInfo createInfo{.usage = vma::MemoryUsage::eAutoPreferDevice};

            AllocatedImage resultImage;
            vma::AllocationInfo allocInfo{};
            resultImage.image = m_allocator->createImage(imageInfo, createInfo, allocInfo);
            
            return resultImage;
        }
        
        /*-- Upload image data using a staging buffer --*/
        void uploadImageData
        (
            vk::raii::CommandBuffer& cmd
        )
        {
            /*// Create staging buffer and upload data
            auto stagingBuffer = createStagingBuffer(vectorData, bufferSize);
            
            cmd.*/
        }
        
        /*--
        * The staging buffers are buffers that are used to transfer data from the CPU to the GPU.
        * They cannot be freed until the data is transferred. So the command buffer must be completed, then the staging buffer can be cleared.
        -*/
        void freeStagingBuffers()
        {
            m_stagingBuffers.clear();
        }

        /*--
        * Debug aid: trip the debugger the moment the N-th allocation happens.
        *
        * Each allocation made through this allocator is named "allocID: N" via
        * vmaSetAllocationName. On shutdown, VMA prints the leaked allocations using
        * VMA_LEAK_LOG_FORMAT (see top of file). Pick one of the printed IDs, call
        * setLeakID(N) at startup, and the next run will __debugbreak() inside
        * createBuffer() at the moment that allocation is made -- giving you the
        * exact call stack of the leak. No effect in release builds (no breakpoint).
        -*/
        void setLeakID(uint32_t id) { m_leakID = id; }

    private:
        DeviceVK& m_deviceVK;
        std::optional<vma::raii::Allocator> m_allocator;
        std::vector<std::shared_ptr<AllocatedBuffer>> m_stagingBuffers;
        uint32_t m_leakID = ~0U;
    };
}
