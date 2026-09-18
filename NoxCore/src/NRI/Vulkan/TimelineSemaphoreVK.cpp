#include "TimelineSemaphoreVK.h"

#include "DeviceVK.h"

namespace NRI
{
    TimelineSemaphoreVK::TimelineSemaphoreVK(DeviceVK& device, uint64_t initialValue) : m_deviceVK(device)
    {
        vk::SemaphoreTypeCreateInfo typeInfo{ .semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = initialValue };
        m_semaphore = vk::raii::Semaphore(device.getDevice(), vk::SemaphoreCreateInfo{ .pNext = &typeInfo });
    }

    uint64_t TimelineSemaphoreVK::getValue() const
    {
        return m_semaphore.getCounterValue();
    }

    void TimelineSemaphoreVK::wait(uint64_t value) const
    {
        const vk::Semaphore semaphore = *m_semaphore;
        (void)m_deviceVK.getDevice().waitSemaphores(vk::SemaphoreWaitInfo{ .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &value },
                                                    UINT64_MAX);
    }
}
