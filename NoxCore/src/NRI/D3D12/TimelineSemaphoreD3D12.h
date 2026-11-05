#pragma once
#include "D3D12Common.h"
#include "../TimelineSemaphore.h"

namespace NRI
{
    class DeviceD3D12;

    class TimelineSemaphoreD3D12 final : public TimelineSemaphore
    {
    public:
        TimelineSemaphoreD3D12(DeviceD3D12& device, uint64_t initialValue);
        ~TimelineSemaphoreD3D12() override;

        uint64_t getValue() const override;
        void wait(uint64_t value) const override;

        ID3D12Fence* getNativeFence() const { return m_fence.Get(); }

    private:
        DeviceD3D12& m_deviceD3D12;
        ComPtr<ID3D12Fence> m_fence;
        HANDLE m_event = nullptr;
    };
}
