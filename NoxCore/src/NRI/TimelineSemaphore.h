#pragma once
#include <cstdint>

namespace NRI
{
    // The queues work is submitted to. Without a dedicated transfer family on the device, Transfer is the graphics queue.
    enum class QueueType : uint8_t
    {
        Graphics,
        Transfer
    };

    // A monotonic clock shared by the GPU and the CPU (Vulkan timeline semaphore): a submission signals a value when it
    // completes, a later submission waits for a value before it starts, and the host reads how far the GPU got without
    // blocking.
    class TimelineSemaphore
    {
    public:
        virtual ~TimelineSemaphore() = default;

        // The highest value signalled so far.
        virtual uint64_t getValue() const = 0;
        // Blocks the calling thread until the value is reached.
        virtual void wait(uint64_t value) const = 0;
    };

    // One point on a timeline: a submission waits for it or signals it.
    struct TimelinePoint
    {
        TimelineSemaphore* semaphore = nullptr;
        uint64_t value = 0;
    };
}
