#include "GpuProfilerD3D12.h"
#include "DeviceD3D12.h"
#include "CommandBufferD3D12.h"
#include <chrono>

namespace NRI
{
    GpuProfilerD3D12::GpuProfilerD3D12(DeviceD3D12& device, uint32_t framesInFlight)
        : m_deviceD3D12(device), m_slotCount(framesInFlight)
    {
        if (SUCCEEDED(device.getGraphicsQueue()->GetTimestampFrequency(&m_frequency)) && m_frequency > 0)
        {
            m_periodNs = static_cast<float>(1'000'000'000.0 / static_cast<double>(m_frequency));
        }

        m_slots = std::make_unique<FrameSlot[]>(framesInFlight);
        for (uint32_t i = 0; i < framesInFlight; ++i)
        {
            auto& slot = m_slots[i];
            D3D12_QUERY_HEAP_DESC desc{};
            desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            desc.Count = QueriesPerFrame;
            desc.NodeMask = 0;
            ThrowIfFailed(device.getNativeDevice()->CreateQueryHeap(&desc, IID_PPV_ARGS(&slot.heap)), "CreateQueryHeap (Profiler)");

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_READBACK;

            D3D12_RESOURCE_DESC resDesc{};
            resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resDesc.Width = QueriesPerFrame * sizeof(uint64_t);
            resDesc.Height = 1;
            resDesc.DepthOrArraySize = 1;
            resDesc.MipLevels = 1;
            resDesc.SampleDesc.Count = 1;
            resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ThrowIfFailed(device.getNativeDevice()->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&slot.readback)), "Profiler Readback Buffer");

            slot.timestamps.resize(QueriesPerFrame);
        }
    }

    bool GpuProfilerD3D12::calibrate(uint64_t& outGpuTicks, int64_t& outHostNanoseconds)
    {
        uint64_t cpuTicks = 0;
        if (FAILED(m_deviceD3D12.getGraphicsQueue()->GetClockCalibration(&outGpuTicks, &cpuTicks)))
            return false;

        LARGE_INTEGER qpcFreq;
        QueryPerformanceFrequency(&qpcFreq);
        outHostNanoseconds = static_cast<int64_t>((cpuTicks * 1'000'000'000ULL) / qpcFreq.QuadPart);
        return true;
    }

    void GpuProfilerD3D12::beginFrame(uint32_t frameSlot)
    {
        m_activeSlot = frameSlot;
        auto& slot = m_slots[frameSlot];

        uint32_t count = slot.allocatedQueries.load();
        if (count > 0)
        {
            D3D12_RANGE readRange{ 0, count * sizeof(uint64_t) };
            void* mappedData = nullptr;
            if (SUCCEEDED(slot.readback->Map(0, &readRange, &mappedData)))
            {
                const auto* ticks = static_cast<const uint64_t*>(mappedData);
                m_readback.resize(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    m_readback[i].queryId = i;
                    m_readback[i].ticks = ticks[i];
                }
                D3D12_RANGE writeRange{ 0, 0 };
                slot.readback->Unmap(0, &writeRange);
            }
        }
        slot.allocatedQueries.store(0);
    }

    void GpuProfilerD3D12::resetQueries(CommandBuffer& cmd)
    {
        // No explicit reset needed in D3D12
    }

    uint32_t GpuProfilerD3D12::allocateQueryPair()
    {
        auto& slot = m_slots[m_activeSlot];
        uint32_t id = slot.allocatedQueries.fetch_add(2);
        if (id + 2 > QueriesPerFrame)
            return InvalidQueryId;
        return id;
    }

    void GpuProfilerD3D12::writeTimestamp(CommandBuffer& cmd, uint32_t queryId)
    {
        if (queryId >= QueriesPerFrame) return;

        auto* cmdD3D12 = dynamic_cast<CommandBufferD3D12*>(&cmd);
        if (!cmdD3D12) return;

        auto& slot = m_slots[m_activeSlot];
        cmdD3D12->getNativeCommandList()->EndQuery(slot.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryId);

        // Resolve query to readback buffer
        cmdD3D12->getNativeCommandList()->ResolveQueryData(
            slot.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryId, 1,
            slot.readback.Get(), queryId * sizeof(uint64_t));
    }
}
