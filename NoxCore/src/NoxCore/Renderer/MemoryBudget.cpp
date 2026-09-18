#include "MemoryBudget.h"

#include "NoxCore/Core/Log.h"

namespace Nox
{
    namespace
    {
        // Shares of the device budget (D10, from the Phase 0 measurements); what they leave is headroom.
        constexpr std::array<float, MemoryCategoryCount> CategoryShares{ 0.20f, 0.40f, 0.15f, 0.05f, 0.10f };
        constexpr std::array<const char*, MemoryCategoryCount> CategoryNames{ "Geometry", "Textures", "Ray Tracing", "Scene", "Transient" };
    }

    void MemoryBudget::Update(std::span<const NRI::MemoryHeapStats> heaps)
    {
        m_DeviceBudget = 0;
        for (const NRI::MemoryHeapStats& heap : heaps)
        {
            if (heap.deviceLocal)
                m_DeviceBudget = std::max(m_DeviceBudget, heap.budget);
        }

        for (uint32_t category = 0; category < MemoryCategoryCount; ++category)
        {
            m_Categories[category].Name = CategoryNames[category];
            m_Categories[category].Budget = static_cast<uint64_t>(static_cast<double>(m_DeviceBudget) * CategoryShares[category]);
        }
    }

    void MemoryBudget::SetCommitted(MemoryCategory category, uint64_t committed, uint64_t used)
    {
        const uint32_t index = static_cast<uint32_t>(category);
        MemoryCategoryStats& stats = m_Categories[index];
        stats.Committed = committed;
        stats.Used = used;

        // Once per crossing, so a category that settles over budget does not repeat itself every sample.
        const bool overBudget = stats.Budget > 0 && stats.Committed > stats.Budget;
        if (overBudget && !m_Warned[index])
        {
            NOX_CORE_WARN("Memory category '{}' is over budget: {} MB committed of {} MB", stats.Name,
                          stats.Committed / (1024 * 1024), stats.Budget / (1024 * 1024));
        }
        m_Warned[index] = overBudget;
    }

    uint64_t MemoryBudget::GetHeadroomBytes() const
    {
        uint64_t committed = 0;
        for (const MemoryCategoryStats& stats : m_Categories)
            committed += stats.Committed;
        return m_DeviceBudget > committed ? m_DeviceBudget - committed : 0;
    }
}
