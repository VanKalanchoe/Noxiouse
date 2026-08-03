#pragma once
#include <vector>
#include <memory>
#include <algorithm>
#include "FreeListAllocator.h"
#include <nri/NRI.h>

namespace Nox
{
    struct PageAllocation
    {
        uint32_t pageIndex = UINT32_MAX;
        uint32_t offset = 0;
        uint32_t count = 0;

        bool IsValid() const { return pageIndex != UINT32_MAX && count > 0; }
    };

    template <typename T>
    class PagedBufferAllocator
    {
    public:
        struct Page
        {
            std::unique_ptr<NRI::Buffer> buffer;
            FreeListAllocator allocator;
            uint32_t capacity = 0;
        };

        PagedBufferAllocator() = default;

        // Uses NRI::BufferUsage just like your createBuffer calls!
        void Init(NRI::Device* device, uint32_t defaultPageCapacity, NRI::BufferUsage usage = NRI::BufferUsage::StorageStatic)
        {
            m_device = device;
            m_defaultPageCapacity = defaultPageCapacity;
            m_usage = usage;
        }

        PageAllocation Allocate(uint32_t count)
        {
            if (count == 0) return {};

            // 1. Try to allocate from existing pages
            for (uint32_t i = 0; i < m_pages.size(); ++i)
            {
                uint32_t offset = m_pages[i].allocator.Allocate(count);
                if (offset != UINT32_MAX)
                {
                    return { i, offset, count };
                }
            }

            // 2. No existing page has room -> Spawn a new Page!
            uint32_t pageCapacity = std::max(m_defaultPageCapacity, count);

            Page newPage;
            newPage.capacity = pageCapacity;
            newPage.allocator.Init(pageCapacity);
            
            // Matches your EXACT buffer creation code:
            newPage.buffer = m_device->createBuffer(NRI::BufferDesc{
                .size = sizeof(T) * pageCapacity,
                .usage = m_usage
            });

            uint32_t offset = newPage.allocator.Allocate(count);
            m_pages.push_back(std::move(newPage));

            uint32_t pageIndex = static_cast<uint32_t>(m_pages.size() - 1);
            return { pageIndex, offset, count };
        }

        NRI::Buffer* GetBuffer(uint32_t pageIndex) const
        {
            if (pageIndex < m_pages.size())
                return m_pages[pageIndex].buffer.get();
            return nullptr;
        }

        bool Free(uint32_t pageIndex, uint32_t offset, uint32_t count, std::unique_ptr<NRI::Buffer>& outEmptyBuffer)
        {
            if (pageIndex >= m_pages.size()) return false;

            auto& page = m_pages[pageIndex];
            page.allocator.Free(offset, count);

            // Check if page is completely empty
            if (page.allocator.GetAvailableSpace() == page.capacity)
            {
                outEmptyBuffer = std::move(page.buffer);
                m_pages.erase(m_pages.begin() + pageIndex);
                return true; // Page was removed!
            }

            return false;
        }

        size_t GetPageCount() const { return m_pages.size(); }

    private:
        NRI::Device* m_device = nullptr;
        uint32_t m_defaultPageCapacity = 0;
        NRI::BufferUsage m_usage = NRI::BufferUsage::StorageStatic; // Fixed enum type
        std::vector<Page> m_pages;
    };
}