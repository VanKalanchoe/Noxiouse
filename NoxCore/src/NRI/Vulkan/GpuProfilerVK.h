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
        bool isComputeQueueSupported() const override { return m_computeQueueTimestamps; }
        float getTimestampPeriod() const override { return m_timestampPeriod; }
        uint32_t getTimestampValidBits() const override { return m_timestampValidBits; }
        bool calibrate(uint64_t& outGpuTicks, int64_t& outHostNanoseconds) override;

        void beginFrame(uint32_t frameSlot) override;
        std::span<const GpuTimestamp> getReadbackTimestamps() const override { return m_readback; }
        void resetQueries(CommandBuffer& cmd) override;

        uint32_t allocateQueryPair() override;
        void writeTimestamp(CommandBuffer& cmd, uint32_t queryId) override;

        bool isPipelineStatisticsSupported() const override { return m_statisticsSupported; }
        uint32_t allocateStatisticsQuery() override;
        void beginStatistics(CommandBuffer& cmd, uint32_t queryId) override;
        void endStatistics(CommandBuffer& cmd, uint32_t queryId) override;
        std::span<const GpuPipelineStatistics> getReadbackStatistics() const override { return m_statisticsReadback; }

        bool isTransferQueueSupported() const override { return m_transferQueueTimestamps; }
        uint32_t beginTransferQuery(CommandBuffer& cmd) override;
        void endTransferQuery(CommandBuffer& cmd, uint32_t queryId) override;
        bool readTransferQuery(uint32_t queryId, uint64_t& outBeginTicks, uint64_t& outEndTicks) override;

    private:
        void readSlotResults(uint32_t frameSlot);

    private:
        static constexpr uint32_t QueriesPerFrame = 256;
        static constexpr uint32_t StatisticsQueriesPerFrame = 64;
        static constexpr uint32_t NoSlot = ~0u;

        struct FrameQueries
        {
            vk::raii::QueryPool pool = nullptr;
            // Queries handed out since the last reset (always written in pairs), read back on the next beginFrame.
            std::atomic<uint32_t> allocatedQueries = 0;
            vk::raii::QueryPool statisticsPool = nullptr;
            std::atomic<uint32_t> allocatedStatistics = 0;
        };

        DeviceVK& m_deviceVK;
        std::unique_ptr<FrameQueries[]> m_frames; // atomics do not move
        uint32_t m_frameCount = 0;
        std::vector<GpuTimestamp> m_readback;
        std::vector<GpuPipelineStatistics> m_statisticsReadback;
        bool m_statisticsSupported = false;
        std::vector<uint64_t> m_resultScratch; // (value, availability) pairs for vkGetQueryPoolResults
        uint32_t m_currentSlot = NoSlot;        // written by beginFrame before recording starts
        float m_timestampPeriod = 0.0f;
        uint32_t m_timestampValidBits = 0;
        int64_t m_hostFrequency = 1; // QueryPerformanceFrequency (Windows)
        bool m_computeQueueTimestamps = false;

        // Transfer queue: pairs (2i, 2i + 1) of their own pool, free ones listed (main thread only).
        static constexpr uint32_t TransferQueryPairs = 64;
        vk::raii::QueryPool m_transferPool = nullptr;
        std::vector<uint32_t> m_freeTransferPairs;
        bool m_transferQueueTimestamps = false;
    };
}
