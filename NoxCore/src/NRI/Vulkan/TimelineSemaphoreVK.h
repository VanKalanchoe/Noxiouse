#pragma once
#include "VulkanCommon.h"
#include "../TimelineSemaphore.h"

namespace NRI
{
    class DeviceVK;

    class TimelineSemaphoreVK : public TimelineSemaphore
    {
    public:
        TimelineSemaphoreVK(DeviceVK& device, uint64_t initialValue);

        uint64_t getValue() const override;
        void wait(uint64_t value) const override;
        const vk::raii::Semaphore& getNativeSemaphore() const { return m_semaphore; }

    private:
        DeviceVK& m_deviceVK;
        vk::raii::Semaphore m_semaphore = nullptr;
    };
}
