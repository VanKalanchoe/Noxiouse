#include "UploadManager.h"

#include <algorithm>

#include "NoxCore/Core/core.h"

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
            return allocateDedicated(size);

        const uint64_t offset = ringOffset(size, alignment);
        if (!waitForRingSpace(offset, size))
            return allocateDedicated(size);
        return allocateRing(offset, size);
    }

    std::optional<StagingSpan> UploadManager::TryReserveStaging(uint64_t size, uint64_t alignment)
    {
        if (size > m_ringCapacity / 2)
            return allocateDedicated(size);

        const uint64_t offset = ringOffset(size, alignment);
        if (overlapsLiveAllocation(offset, size))
            return std::nullopt;
        return allocateRing(offset, size);
    }

    void UploadManager::ReleaseStaging(const StagingSpan& span)
    {
        // No copy read it, so nothing on the GPU waits for it.
        if (m_dedicated.erase(span.ticket) > 0)
            return;
        std::erase_if(m_ringAllocations, [&](const RingAllocation& allocation) { return allocation.ticket == span.ticket; });
    }

    void UploadManager::CopyToBuffer(const StagingSpan& source, uint64_t sourceOffset, uint64_t size, NRI::Buffer& destination, uint64_t destinationOffset)
    {
        recording().copyBuffer(*source.buffer, destination,
                               NRI::BufferCopyRegion{ .srcOffset = source.offset + sourceOffset, .dstOffset = destinationOffset, .size = size });
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
        m_frameWaitValue = m_submittedValue + 1;
    }

    uint64_t UploadManager::Commit(const StagingSpan& span, bool nextFrameReads)
    {
        NOX_CORE_ASSERT(m_recording, "UploadManager::Commit without recorded copies");
        const uint64_t value = m_submittedValue + 1;

        if (auto dedicated = m_dedicated.find(span.ticket); dedicated != m_dedicated.end())
        {
            m_retained.push_back(std::move(dedicated->second));
            m_dedicated.erase(dedicated);
        }
        else
        {
            auto allocation = std::find_if(m_ringAllocations.begin(), m_ringAllocations.end(),
                                           [&](const RingAllocation& candidate) { return candidate.ticket == span.ticket; });
            if (allocation != m_ringAllocations.end())
                allocation->value = value;
        }

        if (nextFrameReads)
            m_frameWaitValue = value;
        return value;
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
        m_completedValue = m_timeline->getValue();
        while (!m_inFlight.empty() && m_inFlight.front().value <= m_completedValue)
        {
            m_freeCommandBuffers.push_back(std::move(m_inFlight.front().commandBuffer));
            m_inFlight.pop_front();
        }
        // Not only from the front: a span still being filled must not hold back the space of later, completed ones.
        std::erase_if(m_ringAllocations, [&](const RingAllocation& allocation) { return allocation.value <= m_completedValue; });
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
            // Copies of one submission may write what an earlier one wrote (a stream grown there, a range reused after
            // its copy): a barrier's first scope covers everything submitted to the queue before it.
            m_recording->transferBarrier();
        }
        return *m_recording;
    }

    uint64_t UploadManager::ringOffset(uint64_t size, uint64_t alignment) const
    {
        const uint64_t offset = (m_ringHead + alignment - 1) / alignment * alignment;
        return offset + size > m_ringCapacity ? 0 : offset;
    }

    bool UploadManager::overlapsLiveAllocation(uint64_t offset, uint64_t size) const
    {
        return std::any_of(m_ringAllocations.begin(), m_ringAllocations.end(), [&](const RingAllocation& allocation)
        {
            return allocation.offset < offset + size && offset < allocation.offset + allocation.size;
        });
    }

    bool UploadManager::waitForRingSpace(uint64_t offset, uint64_t size)
    {
        for (;;)
        {
            auto overlapping = std::find_if(m_ringAllocations.begin(), m_ringAllocations.end(), [&](const RingAllocation& allocation)
            {
                return allocation.offset < offset + size && offset < allocation.offset + allocation.size;
            });
            if (overlapping == m_ringAllocations.end())
                return true;
            if (overlapping->value == OpenValue)
                return false;

            const uint64_t value = overlapping->value;
            if (value > m_submittedValue)
                Flush();
            m_timeline->wait(value);
            Poll();
        }
    }

    StagingSpan UploadManager::allocateRing(uint64_t offset, uint64_t size)
    {
        const uint64_t ticket = m_nextTicket++;
        m_ringAllocations.push_back({ offset, size, ticket, OpenValue });
        m_ringHead = offset + size;
        return { m_ring.get(), m_ringMapped + offset, offset, size, ticket };
    }

    StagingSpan UploadManager::allocateDedicated(uint64_t size)
    {
        const uint64_t ticket = m_nextTicket++;
        std::unique_ptr<NRI::Buffer> buffer = m_device->createBuffer(NRI::BufferDesc{ .size = size, .usage = NRI::BufferUsage::Staging });
        const StagingSpan span{ buffer.get(), static_cast<uint8_t*>(buffer->map(0, size)), 0, size, ticket };
        m_dedicated.emplace(ticket, std::move(buffer));
        return span;
    }
}
