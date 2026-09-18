#include "UploadManager.h"

namespace Nox
{
    void UploadManager::Init(NRI::Device& device, uint64_t ringCapacity)
    {
        m_device = &device;
        m_allocator = device.createCommandAllocator(NRI::CommandBufferReset::PerCommandBuffer, NRI::QueueType::Transfer);
        m_timeline = device.createTimelineSemaphore(0);

        m_ringCapacity = ringCapacity;
        m_ring = device.createBuffer(NRI::BufferDesc{ .size = ringCapacity, .usage = NRI::BufferUsage::Staging });
        m_ringMapped = static_cast<uint8_t*>(m_ring->map(0, ringCapacity));
    }

    StagingSpan UploadManager::ReserveStaging(uint64_t size, uint64_t alignment)
    {
        // More than half the ring would evict nearly everything in flight: such an upload gets its own staging buffer,
        // released once the submission reading it completes.
        if (size > m_ringCapacity / 2)
        {
            std::unique_ptr<NRI::Buffer> buffer = m_device->createBuffer(NRI::BufferDesc{ .size = size, .usage = NRI::BufferUsage::Staging });
            StagingSpan span{ buffer.get(), static_cast<uint8_t*>(buffer->map(0, size)), 0, size };
            m_retained.push_back(std::move(buffer));
            return span;
        }

        uint64_t offset = (m_ringHead + alignment - 1) / alignment * alignment;
        if (offset + size > m_ringCapacity)
            offset = 0;
        waitForRingSpace(offset, size);

        m_ringAllocations.push_back({ offset, size, m_submittedValue + 1 });
        m_ringHead = offset + size;
        return { m_ring.get(), m_ringMapped + offset, offset, size };
    }

    void UploadManager::CopyToBuffer(const StagingSpan& source, NRI::Buffer& destination, uint64_t destinationOffset)
    {
        recording().copyBuffer(*source.buffer, destination,
                               NRI::BufferCopyRegion{ .srcOffset = source.offset, .dstOffset = destinationOffset, .size = source.size });
    }

    void UploadManager::CopyToTexture(const StagingSpan& source, NRI::Texture2D& destination, const std::vector<size_t>& mipOffsets)
    {
        destination.recordUpload(recording(), *source.buffer, source.offset, mipOffsets);
    }

    void UploadManager::CopyBuffer(NRI::Buffer& source, NRI::Buffer& destination, uint64_t size)
    {
        NRI::CommandBuffer& cmd = recording();
        cmd.transferBarrier();
        cmd.copyBuffer(source, destination, NRI::BufferCopyRegion{ .srcOffset = 0, .dstOffset = 0, .size = size });
        cmd.transferBarrier();
    }

    uint64_t UploadManager::Flush()
    {
        if (!m_recording)
            return m_submittedValue;

        m_recording->end(0);
        const uint64_t value = ++m_submittedValue;
        NRI::CommandBuffer* commandBuffer = m_recording.get();
        const NRI::TimelinePoint signal{ m_timeline.get(), value };
        m_device->submit(NRI::QueueType::Transfer, std::span<NRI::CommandBuffer* const>(&commandBuffer, 1), {},
                         std::span<const NRI::TimelinePoint>(&signal, 1));

        m_inFlight.push_back(Submission{ value, std::move(m_recording), std::move(m_retained) });
        m_retained.clear();
        return value;
    }

    void UploadManager::Poll()
    {
        const uint64_t completed = m_timeline->getValue();
        while (!m_inFlight.empty() && m_inFlight.front().value <= completed)
        {
            m_freeCommandBuffers.push_back(std::move(m_inFlight.front().commandBuffer));
            m_inFlight.pop_front();
        }
        while (!m_ringAllocations.empty() && m_ringAllocations.front().value <= completed)
            m_ringAllocations.pop_front();
    }

    NRI::CommandBuffer& UploadManager::recording()
    {
        if (!m_recording)
        {
            if (!m_freeCommandBuffers.empty())
            {
                m_recording = std::move(m_freeCommandBuffers.back());
                m_freeCommandBuffers.pop_back();
            }
            else
            {
                m_recording = m_allocator->allocateCommandBuffer(1);
            }
            m_recording->begin(0, true);
        }
        return *m_recording;
    }

    void UploadManager::waitForRingSpace(uint64_t offset, uint64_t size)
    {
        // Allocations are handed out in ring order, so the first one a new range can run into is the oldest.
        auto overlaps = [&](const RingAllocation& allocation)
        {
            return allocation.offset < offset + size && offset < allocation.offset + allocation.size;
        };
        while (!m_ringAllocations.empty() && overlaps(m_ringAllocations.front()))
        {
            const uint64_t value = m_ringAllocations.front().value;
            if (value > m_submittedValue)
                Flush();
            m_timeline->wait(value);
            Poll();
        }
    }
}
