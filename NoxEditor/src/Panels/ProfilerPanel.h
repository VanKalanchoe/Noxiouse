#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Nox
{
    struct TimelineFrame;
    struct TimelineScope;
    struct ProfileCapture;

    // The frame timeline (docs/Engine_Architecture_Plan_2026.md §5.1, like Unreal Insights' timing view inside the editor): the
    // last frames' times as a graph, and the picked frame's scopes on a time axis, one lane per thread and one per GPU queue
    // (graphics, async compute, transfer; on the CPU clock where the device calibrates its timestamps), with the frame's scopes in
    // a sortable table. The frames slower than a threshold are kept apart (spikes), memory is drawn under the frame graph, and the
    // whole timeline can be saved to a .noxprof file and opened again. Opened from Window > Profiler; the profiler keeps the frames
    // only while the window is open and recording.
    class ProfilerPanel
    {
    public:
        ProfilerPanel();
        ~ProfilerPanel();

        void OnImGuiRender();

        bool IsOpen() const { return m_Open; }
        void SetOpen(bool open) { m_Open = open; }

    private:
        enum class FrameSource : uint8_t
        {
            Recent, // the last frames
            Spikes  // the slow frames kept apart
        };

        struct ScopeDetails
        {
            const char* Name = "?";
            const char* Function = "";
            const char* File = "";
            uint32_t Line = 0;
            bool IsWait = false;
        };

        void DrawToolbar();
        void DrawFrameGraph();
        void DrawMemoryGraph();
        void DrawTimeline();
        void DrawScopeTable();
        void ScopeTooltip(const TimelineScope& scope, const TimelineFrame& frame, bool gpu) const;

        // The frames shown: the live profiler's or a loaded capture's, recent ones or spikes.
        uint32_t FrameCount() const;
        const TimelineFrame& Frame(uint32_t index) const;
        uint32_t FrameSlots() const; // the frame graph's width in frames
        uint32_t ThreadCount() const;
        const std::string& ThreadName(uint32_t lane) const;
        ScopeDetails Scope(uint32_t scopeId) const;
        const char* ScopeName(uint32_t scopeId) const { return Scope(scopeId).Name; }

        int32_t FindFrame(uint64_t number) const; // index into the frames shown, -1: not there
        void SelectFrame(uint32_t index);
        void FollowNewest();
        void SaveCapture();
        void LoadCapture();

    private:
        bool m_Open = false;
        bool m_Paused = false;
        bool m_Follow = true; // the newest complete frame stays selected and in view
        bool m_ShowMemory = true;
        FrameSource m_Source = FrameSource::Recent;
        uint64_t m_SelectedFrame = 0;
        bool m_HasSelection = false;
        double m_ViewStartNs = 0.0;
        double m_ViewDurationNs = 0.0;
        uint32_t m_HighlightScope = ~0u;
        char m_Filter[64] = {};

        std::unique_ptr<ProfileCapture> m_Capture; // a loaded .noxprof, shown instead of the live frames
        std::string m_CaptureName;
        std::string m_Message; // the last save / load result
    };
}
