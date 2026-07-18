#pragma once
#include "../Buffer.h"
#include "MemoryAllocatorVK.h"

namespace NRI
{
    class DeviceVK;
    
    class BufferVK final : public Buffer
    {
    public:
        BufferVK(DeviceVK& device, const BufferDesc& desc);
        ~BufferVK() override = default;
        
        void* map(uint64_t offset, uint64_t size) override;
        void unmap() override;
        
        void uploadData(CommandBuffer& cmd, Buffer& stagingBuffer, const void* vectorData, bool singleUpload = true) override;
        uint64_t getDeviceAddress() const override { return m_allocatedBuffer.address; };
        vma::raii::Buffer& getNativeBuffer() { return m_allocatedBuffer.buffer; };
        
    private:
        DeviceVK& m_deviceVK;
        AllocatedBuffer m_allocatedBuffer;
    };   
}
