#pragma once
#include <cstdint>

namespace NRI
{
    struct Extent2D
    {
        uint32_t width;
        uint32_t height;
    };
    
    struct ViewportBounds
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };
    
    // One physical memory heap as reported by the allocator (VK_EXT_memory_budget when available,
    // otherwise the allocator's own estimate).
    struct MemoryHeapStats
    {
        bool deviceLocal = false;
        uint64_t usage = 0;           // bytes the process uses in this heap (all allocations, not only ours)
        uint64_t budget = 0;          // bytes the OS allows this process to use in this heap
        uint64_t blockBytes = 0;      // bytes allocated from the driver by the allocator
        uint64_t allocationBytes = 0; // bytes handed out to resources inside those blocks
        uint32_t allocationCount = 0;
    };

    enum class CompareOp : uint8_t
    {
        Never = 0,
        Less,
        Equal,
        LessOrEqual,
        Greater,
        NotEqual,
        GreaterOrEqual,
        Always
    };
}
