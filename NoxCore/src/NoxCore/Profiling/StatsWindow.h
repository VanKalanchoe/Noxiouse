#pragma once
#include <vector>

#include "Profiler.h"

namespace Nox
{
    class Renderer;

    // Nox Stats as an ImGui window (the editor's; the packaged game keeps the text overlay, StatsOverlay): the frame times and memory
    // always, then collapsible sections -- GPU passes, CPU scopes, memory heaps and categories, counters, audio -- in a window that
    // docks and scrolls however long the tables get (Unreal's stat commands grew the same way into Insights' panels).
    class StatsWindow
    {
    public:
        // `open`: the window's close button clears it.
        void Draw(Renderer& renderer, bool* open);

    private:
        std::vector<ProfileScopeStats> m_CpuScopes;
        std::vector<ProfileScopeStats> m_GpuScopes;
        std::vector<ProfileCounterStats> m_Counters;
        std::string m_LastReport; // the last Save Report's path
    };
}
