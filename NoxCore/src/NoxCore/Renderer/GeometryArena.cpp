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
        m_pageSize = 0;
        m_pageUse.clear();
        m_committedPages = 0;

        // Sparse: the whole element space as address space (as much of it as one buffer may reserve), memory bound as ranges need it.
        if (device.supportsSparseBuffers())
        {
            const uint64_t reservable = device.getSparseBufferReserveLimit() / elementSize;
            const uint32_t reserved = static_cast<uint32_t>(std::min<uint64_t>(m_maxCapacity, reservable));
            if (reserved >= initialCapacity)
            {
                std::unique_ptr<NRI::Buffer> buffer = createBuffer(reserved, true);
                if (buffer && buffer->getSparsePageSize() != 0)
                {
                    m_buffer = std::move(buffer);
                    m_maxCapacity = reserved;
                    m_capacity = reserved;
                    m_pageSize = m_buffer->getSparsePageSize();
                    m_pageUse.assign((uint64_t(reserved) * elementSize + m_pageSize - 1) / m_pageSize, 0);
                    m_allocator.emplace(m_maxCapacity);
                    return;
                }
            }
        }

        m_allocator.emplace(m_maxCapacity);
        m_buffer = createBuffer(m_capacity, false);
    }

    bool GeometryArena::changePageUse(uint32_t offset, uint32_t count, bool add)
    {
        const uint64_t firstPage = uint64_t(offset) * m_elementSize / m_pageSize;
        const uint64_t lastPage = (uint64_t(offset + count) * m_elementSize - 1) / m_pageSize;
        // The pages whose use starts (or ends), as runs.
        m_pageScratch.clear();
        for (uint64_t page = firstPage; page <= lastPage; ++page)
        {
            if (m_pageUse[page] != (add ? 0u : 1u))
                continue;
            if (!m_pageScratch.empty() && m_pageScratch.back().firstPage + m_pageScratch.back().pageCount == page)
                ++m_pageScratch.back().pageCount;
            else
                m_pageScratch.push_back({ page, 1 });
        }
        if (!m_pageScratch.empty())
        {
            const std::span<const NRI::SparsePageRange> changed(m_pageScratch);
            if (!m_device->updateSparseBuffer(*m_buffer, add ? changed : std::span<const NRI::SparsePageRange>{},
                                              add ? std::span<const NRI::SparsePageRange>{} : changed))
                return false;
        }
        for (uint64_t page = firstPage; page <= lastPage; ++page)
            m_pageUse[page] = add ? m_pageUse[page] + 1 : m_pageUse[page] - 1;
        uint64_t changedPages = 0;
        for (const NRI::SparsePageRange& range : m_pageScratch)
            changedPages += range.pageCount;
        m_committedPages = add ? m_committedPages + changedPages : m_committedPages - std::min(m_committedPages, changedPages);
        return true;
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

        if (IsSparse())
        {
            // Memory for the pages it touches that no live range had (bound before any later submission uses them).
            if (!changePageUse(allocation.offset, count, true))
            {
                NOX_CORE_ERROR("GeometryArena::Allocate out of device memory for {} elements of {} bytes", count, m_elementSize);
                m_allocator->free(allocation);
                return {};
            }
            m_usedElements += count;
            return { allocation.offset, count, allocation.metadata };
        }

        const uint32_t end = allocation.offset + count;
        if (end > m_capacity)
        {
            const uint32_t capacity = std::min(std::max(m_capacity * 2, end), m_maxCapacity);
            std::unique_ptr<NRI::Buffer> grown = createBuffer(capacity, false);

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
        // Pages no live range touches any more give their memory back (nothing on the GPU uses a freed range).
        if (IsSparse())
            changePageUse(range.offset, range.count, false);
    }

    bool GeometryArena::Reserve(uint32_t offset, uint32_t count, std::unique_ptr<NRI::Buffer>& outGrownFrom)
    {
        if (count == 0)
            return true;
        if (uint64_t(offset) + count > m_maxCapacity)
            return false;
        if (IsSparse())
        {
            if (!changePageUse(offset, count, true))
                return false;
        }
        else if (offset + count > m_capacity)
        {
            const uint32_t capacity = std::min(std::max(m_capacity * 2, offset + count), m_maxCapacity);
            std::unique_ptr<NRI::Buffer> grown = createBuffer(capacity, false);
            outGrownFrom = std::move(m_buffer);
            m_buffer = std::move(grown);
            m_capacity = capacity;
        }
        m_usedElements += count;
        return true;
    }

    void GeometryArena::Unreserve(uint32_t offset, uint32_t count)
    {
        if (count == 0 || count > m_usedElements)
            return;
        m_usedElements -= count;
        if (IsSparse())
            changePageUse(offset, count, false);
    }

    std::unique_ptr<NRI::Buffer> GeometryArena::createBuffer(uint32_t capacity, bool sparse) const
    {
        return m_device->createBuffer(NRI::BufferDesc{
            .size = uint64_t(capacity) * m_elementSize,
            .usage = m_usage,
            .sharedAcrossQueues = true, // new ranges are copied on the transfer queue while graphics reads the others
            .sparse = sparse
        });
    }
}
