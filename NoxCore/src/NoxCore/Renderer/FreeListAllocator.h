// FreeListAllocator.h
#pragma once
#include <vector>
#include <algorithm>
#include <cstdint>

namespace Nox
{
    class FreeListAllocator
    {
    public:
        struct Block
        {
            uint32_t offset = 0;
            uint32_t size = 0;
        };

        FreeListAllocator() = default;
        explicit FreeListAllocator(uint32_t totalCapacity)
        {
            Init(totalCapacity);
        }

        void Init(uint32_t totalCapacity)
        {
            m_capacity = totalCapacity;
            m_freeBlocks.clear();
            m_freeBlocks.push_back({ 0, totalCapacity });
        }

        // Returns offset if successful, or UINT32_MAX if out of space
        uint32_t Allocate(uint32_t count)
        {
            if (count == 0) return 0;

            // First-fit search
            for (auto it = m_freeBlocks.begin(); it != m_freeBlocks.end(); ++it)
            {
                if (it->size >= count)
                {
                    uint32_t allocatedOffset = it->offset;
                    
                    if (it->size == count)
                    {
                        m_freeBlocks.erase(it);
                    }
                    else
                    {
                        it->offset += count;
                        it->size -= count;
                    }

                    return allocatedOffset;
                }
            }

            return UINT32_MAX; // Out of memory in this buffer!
        }

        // Returns a sub-allocation back to the free list and merges adjacent free blocks
        void Free(uint32_t offset, uint32_t count)
        {
            if (count == 0) return;

            Block newBlock{ offset, count };

            // Keep free list sorted by offset for easy coalescing
            auto it = std::lower_bound(m_freeBlocks.begin(), m_freeBlocks.end(), newBlock,
                [](const Block& a, const Block& b) { return a.offset < b.offset; });

            it = m_freeBlocks.insert(it, newBlock);

            // Merge with next block if contiguous
            auto next = std::next(it);
            if (next != m_freeBlocks.end() && (it->offset + it->size == next->offset))
            {
                it->size += next->size;
                m_freeBlocks.erase(next);
            }

            // Merge with previous block if contiguous
            if (it != m_freeBlocks.begin())
            {
                auto prev = std::prev(it);
                if (prev->offset + prev->size == it->offset)
                {
                    prev->size += it->size;
                    m_freeBlocks.erase(it);
                }
            }
        }
        
        void Grow(uint32_t additionalCapacity)
        {
            if (additionalCapacity == 0) return;

            uint32_t oldCapacity = m_capacity;
            m_capacity += additionalCapacity;

            // Simply free the new range at the end of the buffer!
            // Free() automatically merges it if the last block was also free.
            Free(oldCapacity, additionalCapacity);
        }
        
        uint32_t GetAvailableSpace() const
        {
            uint32_t totalFree = 0;
            for (const auto& block : m_freeBlocks)
            {
                totalFree += block.size;
            }
            return totalFree;
        }

        uint32_t GetCapacity() const { return m_capacity; }

    private:
        uint32_t m_capacity = 0;
        std::vector<Block> m_freeBlocks;
    };
}