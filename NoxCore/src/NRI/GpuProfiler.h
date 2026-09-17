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

    // GPU timestamp queries for the frame's command buffers. Queries are grouped per frame-in-flight slot: when a slot is
    // recorded again, its previous submission has finished (the slot's fence was waited in Swapchain::acquireNextImage),
    // so beginFrame() can read those results without blocking. Query ids are unique across all slots. Scope names and
    // nesting are owned by the engine profiler.
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

        // Main thread, before the slot's command buffers are recorded: collects the slot's previous timestamps
        // (getReadbackTimestamps) and starts handing out its queries.
        virtual void beginFrame(uint32_t frameSlot) = 0;
        virtual std::span<const GpuTimestamp> getReadbackTimestamps() const = 0;
        // Recorded first into the frame's first submitted command buffer, outside rendering.
        virtual void resetQueries(CommandBuffer& cmd) = 0;

        // Any thread while the frame's command buffers are recorded. A pair is (begin, begin + 1): both must be written.
        // Returns InvalidQueryId when the slot has no room left.
        virtual uint32_t allocateQueryPair() = 0;
        virtual void writeTimestamp(CommandBuffer& cmd, uint32_t queryId) = 0;
    };
}
