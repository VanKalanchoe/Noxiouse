#pragma once
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

        void beginFrame(CommandBuffer& cmd, uint32_t frameSlot) override;
        std::span<const GpuTimestamp> getReadbackTimestamps() const override { return m_readback; }

        uint32_t beginScope(CommandBuffer& cmd, const char* label) override;
        uint32_t endScope(CommandBuffer& cmd) override;

    private:
        void readSlotResults(uint32_t frameSlot);

    private:
        static constexpr uint32_t QueriesPerFrame = 256;
        static constexpr uint32_t NoSlot = ~0u;

        struct FrameQueries
        {
            vk::raii::QueryPool pool = nullptr;
            uint32_t writtenQueries = 0; // queries written since the last reset, read back on the next beginFrame
        };

        DeviceVK& m_deviceVK;
        std::vector<FrameQueries> m_frames;
        std::vector<GpuTimestamp> m_readback;
        std::vector<uint64_t> m_resultScratch; // (value, availability) pairs for vkGetQueryPoolResults
        std::vector<bool> m_openScopeHasQuery; // per open scope: did its begin get a query (its end then must too)
        uint32_t m_currentSlot = NoSlot;
        uint32_t m_reservedEndQueries = 0;
        float m_timestampPeriod = 0.0f;
        uint32_t m_timestampValidBits = 0;
        bool m_debugLabels = false;
    };
}
