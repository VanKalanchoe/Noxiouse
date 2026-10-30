#pragma once
#include <memory>
#include <optional>
#include <vector>

#include <NRI/NRI.h>
#include <offsetAllocator.hpp>

namespace Nox
{
    // A sub-allocation inside one geometry stream: the element offset the shaders index with, and the allocator node
    // that Free needs back.
    struct GeometryRange
    {
        uint32_t offset = 0;
        uint32_t count = 0;
        uint32_t node = OffsetAllocator::Allocation::NO_SPACE;

        bool IsValid() const { return count > 0; }
    };

    // One geometry stream (§5.8.2): a single device buffer sub-allocated by element count.
    //
    // With sparse buffers (decision D5, NRI::Device::supportsSparseBuffers) the buffer reserves the stream's whole element space
    // as address space up front, and memory is bound to it page by page: a page is committed when the first live range touches it
    // and released when the last one is freed (ranges are freed once frames are done with them). The buffer never moves, nothing
    // is copied, and the stream holds the pages its live ranges touch.
    //
    // Without them, the allocator hands out offsets from a fixed element space (the stream's ceiling) from the start, while the
    // buffer only covers the part of that space in use: an allocation reaching past the buffer grows it by copying the old
    // contents to offset 0. An allocation's offset therefore never changes, only the buffer's device address does, which
    // is why the mesh table stores offsets and the frame constants one base address per stream.
    class GeometryArena
    {
    public:
        void Init(NRI::Device& device, uint32_t elementSize, uint32_t initialCapacity, uint32_t maxCapacity,
                  NRI::BufferUsage usage = NRI::BufferUsage::StorageStatic);

        // Invalid when the stream is out of space. Growing leaves the buffer it grew from in outGrownFrom: the caller
        // copies its contents into the new buffer and releases it once the frames in flight are done with it.
        GeometryRange Allocate(uint32_t count, std::unique_ptr<NRI::Buffer>& outGrownFrom);
        void Free(const GeometryRange& range);

        NRI::Buffer& GetBuffer() const { return *m_buffer; }
        uint64_t GetDeviceAddress() const { return m_buffer ? m_buffer->getDeviceAddress() : 0; }
        // What the stream holds in device memory: its committed pages (sparse), else its buffer.
        uint64_t GetCapacityBytes() const { return IsSparse() ? m_committedPages * m_pageSize : uint64_t(m_capacity) * m_elementSize; }
        uint64_t GetUsedBytes() const { return uint64_t(m_usedElements) * m_elementSize; }
        bool IsSparse() const { return m_pageSize != 0; }

    private:
        std::unique_ptr<NRI::Buffer> createBuffer(uint32_t capacity, bool sparse) const;
        // Sparse: the pages [offset, offset + count) touches gain (or lose) a range; pages whose count leaves 0 are committed (those that
        // reach 0 released). False when the memory could not be committed (nothing changed then).
        bool changePageUse(uint32_t offset, uint32_t count, bool add);

        NRI::Device* m_device = nullptr;
        std::optional<OffsetAllocator::Allocator> m_allocator;
        std::unique_ptr<NRI::Buffer> m_buffer;
        NRI::BufferUsage m_usage = NRI::BufferUsage::StorageStatic;
        uint32_t m_elementSize = 0;
        uint32_t m_capacity = 0;     // elements the buffer covers
        uint32_t m_maxCapacity = 0;  // elements the allocator hands out from
        uint32_t m_usedElements = 0; // elements held by live allocations
        // Sparse: bytes per page (0: not sparse), live ranges touching each page, pages with memory.
        uint64_t m_pageSize = 0;
        std::vector<uint32_t> m_pageUse;
        uint64_t m_committedPages = 0;
        std::vector<NRI::SparsePageRange> m_pageScratch;
    };
}
