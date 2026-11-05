#pragma once
#include <vector>
#include <mutex>
#include "D3D12Common.h"
#include "../DescriptorHeap.h"

namespace NRI
{
    class DeviceD3D12;

    class DescriptorHeapD3D12 final : public DescriptorHeap
    {
    public:
        DescriptorHeapD3D12(DeviceD3D12& device, const DescriptorHeapDesc& desc);
        ~DescriptorHeapD3D12() override = default;

        void registerTexture(Texture& texture, TextureUsage usageOverride) override;
        uint32_t registerStorageTextureMip(Texture& texture, uint32_t mipLevel) override;
        void unregisterTexture(uint32_t slot) override;
        uint32_t registerBuffer(Buffer& buffer, uint64_t size) override;
        uint32_t registerAccelerationStructure(AccelerationStructure& as, uint32_t slot = ~0u) override;

        uint32_t getImageHeapIndexOffset() const override { return 0; }
        uint64_t getSize() const override { return m_numDescriptors * m_descriptorSize; }
        uint64_t getReservedRangeOffset() const override { return 0; }
        uint64_t getReservedRangeSize() const override { return 0; }
        uint64_t getGPUAddress() const override { return m_gpuStart.ptr; }

        ID3D12DescriptorHeap* getNativeHeap() const { return m_heap.Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE getCpuHandle(uint32_t slot) const;
        D3D12_GPU_DESCRIPTOR_HANDLE getGpuHandle(uint32_t slot) const;

    private:
        uint32_t allocateSlot();
        void freeSlot(uint32_t slot);

    private:
        DeviceD3D12& m_deviceD3D12;
        DescriptorHeapType m_type;
        ComPtr<ID3D12DescriptorHeap> m_heap;
        uint32_t m_numDescriptors = 0;
        uint32_t m_descriptorSize = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{ 0 };
        D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{ 0 };

        std::mutex m_mutex;
        uint32_t m_nextFreeSlot = 0;
        std::vector<uint32_t> m_freeSlots;
    };
}
