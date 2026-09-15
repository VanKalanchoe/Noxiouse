#pragma once
#include <cstdint>
#include <span>

namespace NRI
{
    class CommandBuffer;

    struct GpuTimestamp
    {
        uint32_t queryId = 0;
        uint64_t ticks = 0;
    };

    // GPU timestamp queries for the frame command buffer. Queries are grouped per frame-in-flight slot:
    // when a slot is recorded again, its previous submission has finished (the slot's fence was waited
    // in Swapchain::acquireNextImage), so beginFrame() can read those results without blocking.
    // Query ids are unique across all slots. Scope names/nesting are owned by the engine profiler.
    class GpuProfiler
    {
    public:
        static constexpr uint32_t InvalidQueryId = ~0u;

        virtual ~GpuProfiler() = default;

        virtual bool isSupported() const = 0;
        // Nanoseconds per tick (VkPhysicalDeviceLimits::timestampPeriod).
        virtual float getTimestampPeriod() const = 0;
        // Number of valid bits in a tick value; tick differences must be masked with it.
        virtual uint32_t getTimestampValidBits() const = 0;

        // Call right after CommandBuffer::begin() for this slot, before any rendering is started.
        // Collects the slot's previous timestamps (getReadbackTimestamps) and resets its queries.
        virtual void beginFrame(CommandBuffer& cmd, uint32_t frameSlot) = 0;
        virtual std::span<const GpuTimestamp> getReadbackTimestamps() const = 0;

        // Write a timestamp and open a debug label. Returns InvalidQueryId when the slot has no room;
        // a valid begin always gets a valid end (room for the end query is reserved).
        virtual uint32_t beginScope(CommandBuffer& cmd, const char* label) = 0;
        virtual uint32_t endScope(CommandBuffer& cmd) = 0;
    };
}
