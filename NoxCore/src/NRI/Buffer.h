#pragma once
#include "span"

namespace NRI
{
    class CommandBuffer;
    
    enum class BufferUsage : uint8_t
    {
        Staging,       // HostVisible | HostCoherent, TransferSrc
        Uniform,       // HostVisible | HostCoherent, UniformBuffer
        Vertex,        // DeviceLocal, VertexBuffer | TransferDst
        Index,         // DeviceLocal, IndexBuffer | TransferDst
        Storage,       // DeviceLocal, StorageBuffer
        DescriptorHeap // HostVisible | HostCoherent, DescriptorHeap
    };

    struct BufferDesc
    {
        uint64_t size = 0;
        BufferUsage usage;
    };
    
    class Buffer
    {
    public:
        virtual ~Buffer() = default;
        
        virtual void* map(uint64_t offset, uint64_t size) = 0;
        virtual void unmap() = 0;
        
        virtual void uploadData(CommandBuffer& cmd, Buffer& stagingBuffer, const void* vectorData, bool singleUpload = true) = 0;
        virtual uint64_t getDeviceAddress() const = 0;
    };
}
