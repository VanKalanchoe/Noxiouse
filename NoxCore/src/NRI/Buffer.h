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
        StorageStatic,
        DescriptorHeap, // HostVisible | HostCoherent, DescriptorHeap
        Indirect,
        IndirectStatic,
        AccelerationStructure,        // DeviceLocal, AS storage (BLAS/TLAS backing)
        AccelerationStructureScratch, // DeviceLocal, Scratch buffer for AS build/update
        AccelerationStructureInstance, // HostVisible | Mapped, TLAS instance records
        ShaderBindingTable            // HostVisible | Mapped | DeviceAddress, SBT backing
    };

    struct BufferDesc
    {
        uint64_t size = 0;
        BufferUsage usage;
        // Written on the transfer queue while the graphics queue reads it (concurrent sharing, no ownership transfers).
        bool sharedAcrossQueues = false;
    };
    
    class Buffer
    {
    public:
        virtual ~Buffer() = default;
        
        virtual void* map(uint64_t offset, uint64_t size) = 0;
        virtual void unmap() = 0;
        
        virtual uint64_t getDeviceAddress() const = 0;
        virtual uint64_t getSize() const = 0;
    };
}
