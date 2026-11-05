#include "TimelineSemaphoreD3D12.h"
#include "DeviceD3D12.h"

namespace NRI
{
    TimelineSemaphoreD3D12::TimelineSemaphoreD3D12(DeviceD3D12& device, uint64_t initialValue)
        : m_deviceD3D12(device)
    {
        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateFence(
            initialValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
        m_event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    }

    TimelineSemaphoreD3D12::~TimelineSemaphoreD3D12()
    {
        if (m_event)
        {
            CloseHandle(m_event);
            m_event = nullptr;
        }
    }

    uint64_t TimelineSemaphoreD3D12::getValue() const
    {
        return m_fence->GetCompletedValue();
    }

    void TimelineSemaphoreD3D12::wait(uint64_t value) const
    {
        if (m_fence->GetCompletedValue() < value)
        {
            ThrowIfFailed(m_fence->SetEventOnCompletion(value, m_event), "SetEventOnCompletion");
            WaitForSingleObject(m_event, INFINITE);
        }
    }
}
