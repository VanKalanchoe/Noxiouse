#pragma once
#include <atomic>
#include <memory>
#include <vector>

#include "VulkanCommon.h"
#include "../GpuProfiler.h"

namespace NRI
{
    class DeviceVK;

    class GpuProfilerVK final : public GpuProfiler
    {
    public:
        GpuProfilerVK(DeviceVK& device, uint32_t framesInFlight);
        ~GpuProfilerVK() override = default;

        bool isSupported() const override { return m_timestampValidBits != 0; }
        float getTimestampPeriod() const override { return m_timestampPeriod; }
        uint32_t getTimestampValidBits() const override { return m_timestampValidBits; }

        void beginFrame(uint32_t frameSlot) override;
        std::span<const GpuTimestamp> getReadbackTimestamps() const override { return m_readback; }
        void resetQueries(CommandBuffer& cmd) override;

        uint32_t allocateQueryPair() override;
        void writeTimestamp(CommandBuffer& cmd, uint32_t queryId) override;

    private:
        void readSlotResults(uint32_t frameSlot);

    private:
        static constexpr uint32_t QueriesPerFrame = 256;
        static constexpr uint32_t NoSlot = ~0u;

        struct FrameQueries
        {
            vk::raii::QueryPool pool = nullptr;
            // Queries handed out since the last reset (always written in pairs), read back on the next beginFrame.
            std::atomic<uint32_t> allocatedQueries = 0;
        };

        DeviceVK& m_deviceVK;
        std::unique_ptr<FrameQueries[]> m_frames; // atomics do not move
        uint32_t m_frameCount = 0;
        std::vector<GpuTimestamp> m_readback;
        std::vector<uint64_t> m_resultScratch; // (value, availability) pairs for vkGetQueryPoolResults
        uint32_t m_currentSlot = NoSlot;        // written by beginFrame before recording starts
        float m_timestampPeriod = 0.0f;
        uint32_t m_timestampValidBits = 0;
    };
}
