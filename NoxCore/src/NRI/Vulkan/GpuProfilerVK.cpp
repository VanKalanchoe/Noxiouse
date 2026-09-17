#include "GpuProfilerVK.h"

#include "DeviceVK.h"
#include "CommandBufferVK.h"

namespace NRI
{
    namespace
    {
        // Results come in bit order: fragment, task, mesh (clipping/input assembly counters are not allowed while mesh
        // shaders draw).
        constexpr vk::QueryPipelineStatisticFlags StatisticFlags = vk::QueryPipelineStatisticFlagBits::eFragmentShaderInvocations |
            vk::QueryPipelineStatisticFlagBits::eTaskShaderInvocationsEXT | vk::QueryPipelineStatisticFlagBits::eMeshShaderInvocationsEXT;
        constexpr size_t StatisticValues = 3;
    }

    GpuProfilerVK::GpuProfilerVK(DeviceVK& device, uint32_t framesInFlight) : m_deviceVK(device)
    {
        const std::vector<vk::QueueFamilyProperties> queueFamilies = m_deviceVK.getPhysicalDevice().getQueueFamilyProperties();
        m_timestampValidBits = queueFamilies[m_deviceVK.getQueueIndex()].timestampValidBits;
        m_timestampPeriod = m_deviceVK.getPhysicalDevice().getProperties().limits.timestampPeriod;

        if (!isSupported())
            return;

        m_frameCount = framesInFlight;
        m_frames = std::make_unique<FrameQueries[]>(framesInFlight);
        for (uint32_t slot = 0; slot < framesInFlight; ++slot)
        {
            m_frames[slot].pool = vk::raii::QueryPool(m_deviceVK.getDevice(), vk::QueryPoolCreateInfo{
                .queryType = vk::QueryType::eTimestamp,
                .queryCount = QueriesPerFrame
            });
        }

        m_statisticsSupported = m_deviceVK.isPipelineStatisticsEnabled();
        if (m_statisticsSupported)
        {
            for (uint32_t slot = 0; slot < framesInFlight; ++slot)
            {
                m_frames[slot].statisticsPool = vk::raii::QueryPool(m_deviceVK.getDevice(), vk::QueryPoolCreateInfo{
                    .queryType = vk::QueryType::ePipelineStatistics,
                    .queryCount = StatisticsQueriesPerFrame,
                    .pipelineStatistics = StatisticFlags
                });
            }
        }

        m_readback.reserve(QueriesPerFrame);
        m_statisticsReadback.reserve(StatisticsQueriesPerFrame);
        m_resultScratch.resize(std::max<size_t>(static_cast<size_t>(QueriesPerFrame) * 2, static_cast<size_t>(StatisticsQueriesPerFrame) * (StatisticValues + 1)));
    }

    void GpuProfilerVK::beginFrame(uint32_t frameSlot)
    {
        m_readback.clear();
        m_statisticsReadback.clear();
        m_currentSlot = NoSlot;

        if (!isSupported() || frameSlot >= m_frameCount)
            return;

        readSlotResults(frameSlot);
        m_frames[frameSlot].allocatedQueries.store(0, std::memory_order_relaxed);
        m_frames[frameSlot].allocatedStatistics.store(0, std::memory_order_relaxed);
        m_currentSlot = frameSlot;
    }

    void GpuProfilerVK::resetQueries(CommandBuffer& cmd)
    {
        if (m_currentSlot == NoSlot)
            return;

        // Every query must be reset before it is written again (VUID-vkCmdWriteTimestamp2-None-03864), and the reset
        // must be recorded outside a render pass (VUID-vkCmdResetQueryPool-renderpass). The command buffer holding it is
        // submitted first, so it executes before any timestamp of the frame.
        static_cast<CommandBufferVK&>(cmd).getActiveNativeBuffer().resetQueryPool(*m_frames[m_currentSlot].pool, 0, QueriesPerFrame);
        if (m_statisticsSupported)
            static_cast<CommandBufferVK&>(cmd).getActiveNativeBuffer().resetQueryPool(*m_frames[m_currentSlot].statisticsPool, 0, StatisticsQueriesPerFrame);
    }

    uint32_t GpuProfilerVK::allocateStatisticsQuery()
    {
        if (m_currentSlot == NoSlot || !m_statisticsSupported)
            return InvalidQueryId;

        std::atomic<uint32_t>& allocated = m_frames[m_currentSlot].allocatedStatistics;
        uint32_t query = allocated.load(std::memory_order_relaxed);
        do
        {
            if (query + 1 > StatisticsQueriesPerFrame)
                return InvalidQueryId;
        } while (!allocated.compare_exchange_weak(query, query + 1, std::memory_order_relaxed));

        return m_currentSlot * StatisticsQueriesPerFrame + query;
    }

    void GpuProfilerVK::beginStatistics(CommandBuffer& cmd, uint32_t queryId)
    {
        if (queryId == InvalidQueryId)
            return;
        static_cast<CommandBufferVK&>(cmd).getActiveNativeBuffer().beginQuery(
            *m_frames[queryId / StatisticsQueriesPerFrame].statisticsPool, queryId % StatisticsQueriesPerFrame, {});
    }

    void GpuProfilerVK::endStatistics(CommandBuffer& cmd, uint32_t queryId)
    {
        if (queryId == InvalidQueryId)
            return;
        static_cast<CommandBufferVK&>(cmd).getActiveNativeBuffer().endQuery(
            *m_frames[queryId / StatisticsQueriesPerFrame].statisticsPool, queryId % StatisticsQueriesPerFrame);
    }

    uint32_t GpuProfilerVK::allocateQueryPair()
    {
        if (m_currentSlot == NoSlot)
            return InvalidQueryId;

        std::atomic<uint32_t>& allocated = m_frames[m_currentSlot].allocatedQueries;
        uint32_t first = allocated.load(std::memory_order_relaxed);
        do
        {
            if (first + 2 > QueriesPerFrame)
                return InvalidQueryId;
        } while (!allocated.compare_exchange_weak(first, first + 2, std::memory_order_relaxed));

        return m_currentSlot * QueriesPerFrame + first;
    }

    void GpuProfilerVK::writeTimestamp(CommandBuffer& cmd, uint32_t queryId)
    {
        if (queryId == InvalidQueryId)
            return;

        // Single stage bit (VUID-vkCmdWriteTimestamp2-stage-03859); same stage TracyVulkan uses.
        static_cast<CommandBufferVK&>(cmd).getActiveNativeBuffer().writeTimestamp2(
            vk::PipelineStageFlagBits2::eBottomOfPipe, *m_frames[queryId / QueriesPerFrame].pool, queryId % QueriesPerFrame);
    }

    void GpuProfilerVK::readSlotResults(uint32_t frameSlot)
    {
        FrameQueries& frame = m_frames[frameSlot];

        const uint32_t statisticsCount = frame.allocatedStatistics.load(std::memory_order_relaxed);
        if (statisticsCount > 0)
        {
            const vk::DeviceSize stride = sizeof(uint64_t) * (StatisticValues + 1);
            const vk::Result result = frame.statisticsPool.getResults(0, statisticsCount,
                                                                      static_cast<size_t>(statisticsCount) * stride,
                                                                      m_resultScratch.data(), stride,
                                                                      vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
            if (result == vk::Result::eSuccess || result == vk::Result::eNotReady)
            {
                for (uint32_t query = 0; query < statisticsCount; ++query)
                {
                    const uint64_t* values = m_resultScratch.data() + static_cast<size_t>(query) * (StatisticValues + 1);
                    if (values[StatisticValues] != 0)
                        m_statisticsReadback.push_back({ frameSlot * StatisticsQueriesPerFrame + query, values[0], values[1], values[2] });
                }
            }
        }

        const uint32_t queryCount = frame.allocatedQueries.load(std::memory_order_relaxed);
        if (queryCount == 0)
            return;

        // Only queries written since the last reset are read (VUID-vkGetQueryPoolResults-None-09401).
        // No WAIT flag: the slot's fence has already signaled, availability is still checked per query.
        constexpr vk::DeviceSize stride = sizeof(uint64_t) * 2;
        const vk::Result result = frame.pool.getResults(0, queryCount,
                                                        static_cast<size_t>(queryCount) * stride,
                                                        m_resultScratch.data(), stride,
                                                        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if (result != vk::Result::eSuccess && result != vk::Result::eNotReady)
            return;

        for (uint32_t query = 0; query < queryCount; ++query)
        {
            const uint64_t value = m_resultScratch[query * 2];
            const uint64_t available = m_resultScratch[query * 2 + 1];
            if (available != 0)
                m_readback.push_back({ frameSlot * QueriesPerFrame + query, value });
        }
    }
}
