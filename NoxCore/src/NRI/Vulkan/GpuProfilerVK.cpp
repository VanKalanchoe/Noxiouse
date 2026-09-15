#include "GpuProfilerVK.h"

#include "DeviceVK.h"
#include "CommandBufferVK.h"

namespace NRI
{
    GpuProfilerVK::GpuProfilerVK(DeviceVK& device, uint32_t framesInFlight) : m_deviceVK(device)
    {
        const std::vector<vk::QueueFamilyProperties> queueFamilies = m_deviceVK.getPhysicalDevice().getQueueFamilyProperties();
        m_timestampValidBits = queueFamilies[m_deviceVK.getQueueIndex()].timestampValidBits;
        m_timestampPeriod = m_deviceVK.getPhysicalDevice().getProperties().limits.timestampPeriod;
        m_debugLabels = m_deviceVK.isDebugUtilsEnabled();

        if (!isSupported())
            return;

        m_frames.resize(framesInFlight);
        for (FrameQueries& frame : m_frames)
        {
            frame.pool = vk::raii::QueryPool(m_deviceVK.getDevice(), vk::QueryPoolCreateInfo{
                .queryType = vk::QueryType::eTimestamp,
                .queryCount = QueriesPerFrame
            });
        }

        m_readback.reserve(QueriesPerFrame);
        m_resultScratch.resize(static_cast<size_t>(QueriesPerFrame) * 2);
    }

    void GpuProfilerVK::beginFrame(CommandBuffer& cmd, uint32_t frameSlot)
    {
        m_readback.clear();
        m_openScopeHasQuery.clear();
        m_reservedEndQueries = 0;
        m_currentSlot = NoSlot;

        if (!isSupported() || frameSlot >= m_frames.size())
            return;

        readSlotResults(frameSlot);

        // Every query must be reset before it is written again (VUID-vkCmdWriteTimestamp2-None-03864),
        // and the reset must be recorded outside a render pass (VUID-vkCmdResetQueryPool-renderpass).
        FrameQueries& frame = m_frames[frameSlot];
        static_cast<CommandBufferVK&>(cmd).getActiveNativeBuffer().resetQueryPool(*frame.pool, 0, QueriesPerFrame);
        frame.writtenQueries = 0;
        m_currentSlot = frameSlot;
    }

    uint32_t GpuProfilerVK::beginScope(CommandBuffer& cmd, const char* label)
    {
        vk::raii::CommandBuffer& native = static_cast<CommandBufferVK&>(cmd).getActiveNativeBuffer();
        if (m_debugLabels)
            native.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{ .pLabelName = label });

        const bool hasRoom = m_currentSlot != NoSlot &&
            m_frames[m_currentSlot].writtenQueries + m_reservedEndQueries + 2 <= QueriesPerFrame;
        m_openScopeHasQuery.push_back(hasRoom);
        if (!hasRoom)
            return InvalidQueryId;

        FrameQueries& frame = m_frames[m_currentSlot];
        const uint32_t query = frame.writtenQueries++;
        ++m_reservedEndQueries;
        // Single stage bit (VUID-vkCmdWriteTimestamp2-stage-03859); same stage TracyVulkan uses.
        native.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, *frame.pool, query);
        return m_currentSlot * QueriesPerFrame + query;
    }

    uint32_t GpuProfilerVK::endScope(CommandBuffer& cmd)
    {
        vk::raii::CommandBuffer& native = static_cast<CommandBufferVK&>(cmd).getActiveNativeBuffer();
        if (m_debugLabels)
            native.endDebugUtilsLabelEXT();

        if (m_openScopeHasQuery.empty())
            return InvalidQueryId;

        const bool hasQuery = m_openScopeHasQuery.back();
        m_openScopeHasQuery.pop_back();
        if (!hasQuery)
            return InvalidQueryId;

        FrameQueries& frame = m_frames[m_currentSlot];
        const uint32_t query = frame.writtenQueries++;
        --m_reservedEndQueries;
        native.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, *frame.pool, query);
        return m_currentSlot * QueriesPerFrame + query;
    }

    void GpuProfilerVK::readSlotResults(uint32_t frameSlot)
    {
        FrameQueries& frame = m_frames[frameSlot];
        if (frame.writtenQueries == 0)
            return;

        // Only queries written since the last reset are read (VUID-vkGetQueryPoolResults-None-09401).
        // No WAIT flag: the slot's fence has already signaled, availability is still checked per query.
        constexpr vk::DeviceSize stride = sizeof(uint64_t) * 2;
        const vk::Result result = frame.pool.getResults(0, frame.writtenQueries,
                                                        static_cast<size_t>(frame.writtenQueries) * stride,
                                                        m_resultScratch.data(), stride,
                                                        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if (result != vk::Result::eSuccess && result != vk::Result::eNotReady)
            return;

        for (uint32_t query = 0; query < frame.writtenQueries; ++query)
        {
            const uint64_t value = m_resultScratch[query * 2];
            const uint64_t available = m_resultScratch[query * 2 + 1];
            if (available != 0)
                m_readback.push_back({ frameSlot * QueriesPerFrame + query, value });
        }
    }
}
