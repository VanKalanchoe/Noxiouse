#pragma once
#include <cstdint>
#include <vector>

namespace Nox
{
    struct TimelineFrame;

    // The frame timeline (docs/Engine_Architecture_Plan_2026.md §5.1, like Unreal Insights' timing view inside the editor): the
    // last frames' times as a graph, and the picked frame's scopes on a time axis, one lane per thread and one for the GPU (on
    // the CPU clock where the device calibrates its timestamps), with the frame's scopes in a sortable table. Opened from
    // Window > Profiler; the profiler keeps the frames only while the window is open and recording.
    class ProfilerPanel
    {
    public:
        void OnImGuiRender();

        bool IsOpen() const { return m_Open; }
        void SetOpen(bool open) { m_Open = open; }

    private:
        void DrawToolbar();
        void DrawFrameGraph();
        void DrawTimeline();
        void DrawScopeTable();

        int32_t FindFrame(uint64_t number) const; // index into the profiler's kept frames, -1: no longer kept
        void SelectFrame(uint32_t index);
        void FollowNewest();

    private:
        bool m_Open = false;
        bool m_Paused = false;
        bool m_Follow = true; // the newest complete frame stays selected and in view
        uint64_t m_SelectedFrame = 0;
        bool m_HasSelection = false;
        double m_ViewStartNs = 0.0;
        double m_ViewDurationNs = 0.0;
        uint32_t m_HighlightScope = ~0u;
        char m_Filter[64] = {};
    };
}
