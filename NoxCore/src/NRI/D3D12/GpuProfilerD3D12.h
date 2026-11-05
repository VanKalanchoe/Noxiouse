#pragma once
#include <atomic>
#include <vector>
#include "D3D12Common.h"
#include "../GpuProfiler.h"

namespace NRI
{
    class DeviceD3D12;

    class GpuProfilerD3D12 final : public GpuProfiler
    {
    public:
        GpuProfilerD3D12(DeviceD3D12& device, uint32_t framesInFlight);
        ~GpuProfilerD3D12() override = default;

        bool isSupported() const override { return m_frequency > 0; }
        bool isComputeQueueSupported() const override { return true; }
        float getTimestampPeriod() const override { return m_periodNs; }
        uint32_t getTimestampValidBits() const override { return 64; }
        bool calibrate(uint64_t& outGpuTicks, int64_t& outHostNanoseconds) override;

        void beginFrame(uint32_t frameSlot) override;
        std::span<const GpuTimestamp> getReadbackTimestamps() const override { return m_readback; }
        void resetQueries(CommandBuffer& cmd) override;

        uint32_t allocateQueryPair() override;
        void writeTimestamp(CommandBuffer& cmd, uint32_t queryId) override;

        bool isPipelineStatisticsSupported() const override { return false; }
        uint32_t allocateStatisticsQuery() override { return InvalidQueryId; }
        void beginStatistics(CommandBuffer& cmd, uint32_t queryId) override {}
        void endStatistics(CommandBuffer& cmd, uint32_t queryId) override {}
        std::span<const GpuPipelineStatistics> getReadbackStatistics() const override { return {}; }

    private:
        static constexpr uint32_t QueriesPerFrame = 256;

        struct FrameSlot
        {
            ComPtr<ID3D12QueryHeap> heap;
            ComPtr<ID3D12Resource> readback;
            std::atomic<uint32_t> allocatedQueries = 0;
            std::vector<GpuTimestamp> timestamps;
        };

        DeviceD3D12& m_deviceD3D12;
        uint64_t m_frequency = 0;
        float m_periodNs = 1.0f;
        std::unique_ptr<FrameSlot[]> m_slots;
        uint32_t m_slotCount = 0;
        uint32_t m_activeSlot = 0;
        std::vector<GpuTimestamp> m_readback;
    };
}
