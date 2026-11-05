#include "MemoryAllocatorD3D12.h"
#include "DeviceD3D12.h"

namespace NRI
{
    MemoryAllocatorD3D12::MemoryAllocatorD3D12(DeviceD3D12& device)
        : m_deviceD3D12(device)
    {
#if NOX_HAS_D3D12MA
        D3D12MA::ALLOCATOR_DESC desc{};
        desc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
        desc.pDevice = m_deviceD3D12.getNativeDevice();
        desc.pAdapter = m_deviceD3D12.getNativeAdapter();

        HRESULT hr = D3D12MA::CreateAllocator(&desc, &m_allocator);
        if (FAILED(hr))
        {
            m_allocator.Reset();
        }
#endif
    }

    MemoryAllocatorD3D12::~MemoryAllocatorD3D12()
    {
#if NOX_HAS_D3D12MA
        m_allocator.Reset();
#endif
    }

    AllocatedBufferD3D12 MemoryAllocatorD3D12::createBuffer(
        uint64_t size,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState,
        D3D12_HEAP_TYPE heapType)
    {
        AllocatedBufferD3D12 result{};
        result.size = size;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Alignment = 0;
        resDesc.Width = size;
        resDesc.Height = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels = 1;
        resDesc.Format = DXGI_FORMAT_UNKNOWN;
        resDesc.SampleDesc.Count = 1;
        resDesc.SampleDesc.Quality = 0;
        resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resDesc.Flags = flags;

#if NOX_HAS_D3D12MA
        if (m_allocator)
        {
            D3D12MA::ALLOCATION_DESC allocDesc{};
            allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
            allocDesc.HeapType = heapType;

            ThrowIfFailed(m_allocator->CreateResource(
                &allocDesc,
                &resDesc,
                initialState,
                nullptr,
                &result.allocation,
                IID_PPV_ARGS(&result.resource)), "D3D12MA CreateResource (Buffer)");

            result.gpuAddress = result.resource->GetGPUVirtualAddress();
            return result;
        }
#endif

        // Driver committed resource fallback
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = heapType;

        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resDesc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&result.resource)), "CreateCommittedResource (Buffer)");

        result.gpuAddress = result.resource->GetGPUVirtualAddress();
        return result;
    }

    AllocatedImageD3D12 MemoryAllocatorD3D12::createImage(
        const D3D12_RESOURCE_DESC& desc,
        D3D12_RESOURCE_STATES initialState,
        const D3D12_CLEAR_VALUE* pClearValue)
    {
        AllocatedImageD3D12 result{};

#if NOX_HAS_D3D12MA
        if (m_allocator)
        {
            D3D12MA::ALLOCATION_DESC allocDesc{};
            allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
            allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

            ThrowIfFailed(m_allocator->CreateResource(
                &allocDesc,
                &desc,
                initialState,
                pClearValue,
                &result.allocation,
                IID_PPV_ARGS(&result.resource)), "D3D12MA CreateResource (Image)");

            return result;
        }
#endif

        // Driver committed resource fallback
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initialState,
            pClearValue,
            IID_PPV_ARGS(&result.resource)), "CreateCommittedResource (Image)");

        return result;
    }

    void MemoryAllocatorD3D12::setCurrentFrameIndex(uint32_t frameIndex)
    {
#if NOX_HAS_D3D12MA
        if (m_allocator)
        {
            m_allocator->SetCurrentFrameIndex(frameIndex);
        }
#endif
    }

    void MemoryAllocatorD3D12::getHeapStats(std::vector<MemoryHeapStats>& outHeaps) const
    {
        outHeaps.clear();
#if NOX_HAS_D3D12MA
        if (m_allocator)
        {
            D3D12MA::Budget localBudget{}, nonLocalBudget{};
            m_allocator->GetBudget(&localBudget, &nonLocalBudget);

            // Local heap (Dedicated VRAM)
            MemoryHeapStats local{};
            local.deviceLocal = true;
            local.usage = localBudget.UsageBytes;
            local.budget = localBudget.BudgetBytes;
            local.blockBytes = localBudget.Stats.BlockBytes;
            local.allocationBytes = localBudget.Stats.AllocationBytes;
            local.allocationCount = localBudget.Stats.AllocationCount;
            outHeaps.push_back(local);

            // Non-local heap (Shared System Memory)
            MemoryHeapStats nonLocal{};
            nonLocal.deviceLocal = false;
            nonLocal.usage = nonLocalBudget.UsageBytes;
            nonLocal.budget = nonLocalBudget.BudgetBytes;
            nonLocal.blockBytes = nonLocalBudget.Stats.BlockBytes;
            nonLocal.allocationBytes = nonLocalBudget.Stats.AllocationBytes;
            nonLocal.allocationCount = nonLocalBudget.Stats.AllocationCount;
            outHeaps.push_back(nonLocal);
            return;
        }
#endif

        // Fallback using DXGI Video Memory Info
        IDXGIAdapter4* adapter = m_deviceD3D12.getNativeAdapter();
        if (adapter)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO vramInfo{};
            if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vramInfo)))
            {
                MemoryHeapStats local{};
                local.deviceLocal = true;
                local.usage = vramInfo.CurrentUsage;
                local.budget = vramInfo.Budget;
                outHeaps.push_back(local);
            }
        }
    }
}
