#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "JobSystem.h"

namespace Nox
{
    // One T per JobSystem thread slot (§5.2.4): each worker and the main thread only touch their own element, so no
    // locks are needed. Merge or consume the elements in slot order from the main thread when no task runs.
    template <typename T>
    class WorkerLocal
    {
    public:
        WorkerLocal() : m_Slots(JobSystem::Get().GetThreadSlotCount()) {}

        T& Local() { return m_Slots[JobSystem::Get().GetCurrentThreadSlot()].Value; }

        template <typename F>
        void ForEach(F&& function)
        {
            for (Slot& slot : m_Slots)
                function(slot.Value);
        }

    private:
        // Own cache line per slot: no false sharing between workers writing their elements.
        struct Slot
        {
            alignas(64) T Value{};
        };

    private:
        std::vector<Slot> m_Slots;
    };
}
