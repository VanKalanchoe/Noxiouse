#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/Profiling/Profiler.h"

namespace NoxUI
{
        class UIContext;
        class UIElement;
        class UILabel;
    }

namespace Nox
{

    // Window > Profiler (Unreal Insights / Tracy in the editor): the frame-time graph of the kept frames (click one to show it), VRAM
    // and RAM under it, the picked frame's timeline (the GPU queues, the main thread, the other threads; wheel zooms, drag pans,
    // click a scope to mark it, double-click zooms to it) and its scopes summed in a table (click a header to sort, a row to mark).
    // Spikes: slow frames kept apart. Save / Load: .noxprof captures.
    class ProfilerPanel
    {
    public:
        const Ref<NoxUI::UIElement>& BuildUI(NoxUI::UIContext* context);
        const Ref<NoxUI::UIElement>& GetRoot() const { return m_Root; }
        // Once a frame: `shown` while the window is open (frames are kept only while someone looks: the copies cost a little).
        void Update(bool shown);

        // The state the graph, timeline and table elements share.
        enum class FrameSource : uint8_t { Recent, Spikes };
        struct State
        {
            bool Paused = false;
            bool Follow = true; // the newest complete frame stays picked and in view
            bool ShowMemory = true;
            FrameSource Source = FrameSource::Recent;
            uint64_t SelectedFrame = 0;
            bool HasSelection = false;
            double ViewStartNs = 0.0;
            double ViewDurationNs = 0.0;
            uint32_t HighlightScope = ~0u;
            std::string Filter;
            std::unique_ptr<ProfileCapture> Capture; // a loaded .noxprof, shown instead of the live frames
            std::string CaptureName;

            uint32_t FrameCount() const;
            const TimelineFrame& Frame(uint32_t index) const;
            uint32_t FrameSlots() const;
            uint32_t ThreadCount() const;
            const std::string& ThreadName(uint32_t lane) const;
            struct ScopeDetails { const char* Name = "?"; const char* Function = ""; const char* File = ""; uint32_t Line = 0; bool IsWait = false; };
            ScopeDetails Scope(uint32_t scopeId) const;
            int32_t FindFrame(uint64_t number) const;
            void SelectFrame(uint32_t index);
            void FollowNewest();
        };

    private:
        void SaveCapture();
        void LoadCapture();
        void LoadCaptureFrom(const std::string& path);

        NoxUI::UIContext* m_Context = nullptr;
        Ref<NoxUI::UIElement> m_Root;
        std::shared_ptr<State> m_State = std::make_shared<State>();
        std::string m_Message;
    };
}
