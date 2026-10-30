#pragma once
#include <cstdint>
#include <span>

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

    // Pages [firstPage, firstPage + pageCount) of a sparse buffer (Buffer::getSparsePageSize bytes each).
    struct SparsePageRange
    {
        uint64_t firstPage = 0;
        uint64_t pageCount = 0;
    };

    struct BufferDesc
    {
        uint64_t size = 0;
        BufferUsage usage;
        // Written on the transfer queue while the graphics queue reads it (concurrent sharing, no ownership transfers).
        bool sharedAcrossQueues = false;
        // Address space only (sparse binding, decision D5): `size` bytes are reserved, memory is bound to them page by page
        // (Device::updateSparseBuffer). Device local, never mappable. Only where Device::supportsSparseBuffers().
        bool sparse = false;
    };
    
    class Buffer
    {
    public:
        virtual ~Buffer() = default;
        
        virtual void* map(uint64_t offset, uint64_t size) = 0;
        virtual void unmap() = 0;
        
        virtual uint64_t getDeviceAddress() const = 0;
        virtual uint64_t getSize() const = 0;
        // Bytes of a sparse buffer's page (the unit memory is bound in); 0 for a buffer that is not sparse.
        virtual uint64_t getSparsePageSize() const { return 0; }
    };
}
