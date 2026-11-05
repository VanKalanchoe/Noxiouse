#pragma once
#include <vector>
#include <memory>
#include <span>
#include "D3D12Common.h"
#include "../NRITypes.h"

#if __has_include(<D3D12MemAlloc.h>)
    #include <D3D12MemAlloc.h>
    #define NOX_HAS_D3D12MA 1
#elif __has_include("D3D12MemAlloc.h")
    #include "D3D12MemAlloc.h"
    #define NOX_HAS_D3D12MA 1
#else
    #define NOX_HAS_D3D12MA 0
#endif

namespace NRI
{
    class DeviceD3D12;

    struct AllocatedBufferD3D12
    {
#if NOX_HAS_D3D12MA
        ComPtr<D3D12MA::Allocation> allocation;
#endif
        ComPtr<ID3D12Resource> resource;
        uint64_t gpuAddress = 0;
        uint64_t size = 0;
    };

    struct AllocatedImageD3D12
    {
#if NOX_HAS_D3D12MA
        ComPtr<D3D12MA::Allocation> allocation;
#endif
        ComPtr<ID3D12Resource> resource;
    };

    class MemoryAllocatorD3D12
    {
    public:
        explicit MemoryAllocatorD3D12(DeviceD3D12& device);
        ~MemoryAllocatorD3D12();

        AllocatedBufferD3D12 createBuffer(
            uint64_t size,
            D3D12_RESOURCE_FLAGS flags,
            D3D12_RESOURCE_STATES initialState,
            D3D12_HEAP_TYPE heapType
        );

        AllocatedImageD3D12 createImage(
            const D3D12_RESOURCE_DESC& desc,
            D3D12_RESOURCE_STATES initialState,
            const D3D12_CLEAR_VALUE* pClearValue
        );

        void setCurrentFrameIndex(uint32_t frameIndex);
        void getHeapStats(std::vector<MemoryHeapStats>& outHeaps) const;

    private:
        DeviceD3D12& m_deviceD3D12;
#if NOX_HAS_D3D12MA
        ComPtr<D3D12MA::Allocator> m_allocator;
#endif
    };
}
