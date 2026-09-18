#include "GeometryArena.h"

#include <algorithm>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    void GeometryArena::Init(NRI::Device& device, uint32_t elementSize, uint32_t initialCapacity, uint32_t maxCapacity,
                             NRI::BufferUsage usage)
    {
        m_device = &device;
        m_elementSize = elementSize;
        m_usage = usage;
        m_maxCapacity = std::max(maxCapacity, initialCapacity);
        m_capacity = initialCapacity;
        m_usedElements = 0;
        m_allocator.emplace(m_maxCapacity);
        m_buffer = createBuffer(m_capacity);
    }

    GeometryRange GeometryArena::Allocate(uint32_t count, std::unique_ptr<NRI::Buffer>& outGrownFrom)
    {
        if (count == 0)
            return {};

        const OffsetAllocator::Allocation allocation = m_allocator->allocate(count);
        if (allocation.offset == OffsetAllocator::Allocation::NO_SPACE)
        {
            NOX_CORE_ERROR("GeometryArena::Allocate out of space for {} elements of {} bytes", count, m_elementSize);
            return {};
        }

        const uint32_t end = allocation.offset + count;
        if (end > m_capacity)
        {
            const uint32_t capacity = std::min(std::max(m_capacity * 2, end), m_maxCapacity);
            std::unique_ptr<NRI::Buffer> grown = createBuffer(capacity);

            outGrownFrom = std::move(m_buffer);
            m_buffer = std::move(grown);
            m_capacity = capacity;
        }

        m_usedElements += count;
        return { allocation.offset, count, allocation.metadata };
    }

    void GeometryArena::Free(const GeometryRange& range)
    {
        if (!range.IsValid() || range.node == OffsetAllocator::Allocation::NO_SPACE)
            return;

        // Freeing a range twice, or one this stream never handed out, would corrupt the allocator's nodes and only show
        // up as a wild write much later.
        if (range.offset + range.count > m_capacity || range.count > m_usedElements)
        {
            NOX_CORE_ERROR("GeometryArena::Free rejected range offset {} count {} (capacity {}, used {})", range.offset,
                           range.count, m_capacity, m_usedElements);
            return;
        }

        m_allocator->free(OffsetAllocator::Allocation{ range.offset, range.node });
        m_usedElements -= range.count;
    }

    std::unique_ptr<NRI::Buffer> GeometryArena::createBuffer(uint32_t capacity) const
    {
        return m_device->createBuffer(NRI::BufferDesc{
            .size = uint64_t(capacity) * m_elementSize,
            .usage = m_usage,
            .sharedAcrossQueues = true // new ranges are copied on the transfer queue while graphics reads the others
        });
    }
}
