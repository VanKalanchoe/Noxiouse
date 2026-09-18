#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <nri/NRI.h>

namespace Nox
{
    // Where device memory goes (§5.8.3). Everything the engine holds on the device belongs to exactly one of these.
    enum class MemoryCategory : uint32_t
    {
        Geometry,   // the unified geometry streams, by capacity
        Textures,   // asset and renderer images
        RayTracing, // acceleration structure storage, scratch and instance buffers
        Scene,      // the GPU scene tables
        Transient,  // the render graph pool
        Count
    };
    inline constexpr uint32_t MemoryCategoryCount = static_cast<uint32_t>(MemoryCategory::Count);

    struct MemoryCategoryStats
    {
        const char* Name = nullptr;
        uint64_t Committed = 0; // bytes the category holds (a stream counts its capacity, not its live ranges)
        uint64_t Used = 0;      // of those, the bytes actually in use; equal to Committed where there is no difference
        uint64_t Budget = 0;    // this category's share of the device budget
    };

    // Budgets are policy from the top, allocations are requests from the bottom (§5.8.3): the device reports what it
    // allows, settings split that into category shares, and each system reports what it holds. This is a *soft* budget:
    // going over is reported and logged once, never refused -- enforcement needs something evictable and arrives with the
    // residency manager (§5.9).
    class MemoryBudget
    {
    public:
        // From the newest heap sample; the device-local heap decides what there is to split.
        void Update(std::span<const NRI::MemoryHeapStats> heaps);
        void SetCommitted(MemoryCategory category, uint64_t committed, uint64_t used);

        std::span<const MemoryCategoryStats> GetCategories() const { return m_Categories; }
        uint64_t GetDeviceBudget() const { return m_DeviceBudget; }
        // What the categories leave of the device budget (driver, swapchain, Streamline, spikes).
        uint64_t GetHeadroomBytes() const;

    private:
        std::array<MemoryCategoryStats, MemoryCategoryCount> m_Categories{};
        std::array<bool, MemoryCategoryCount> m_Warned{};
        uint64_t m_DeviceBudget = 0;
    };
}
