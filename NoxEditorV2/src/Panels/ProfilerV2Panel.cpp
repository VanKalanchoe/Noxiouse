#include "ProfilerV2Panel.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <string_view>
#include <unordered_map>

#include <SDL3/SDL_keyboard.h>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UIDropdown.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UISearchBar.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    using State = ProfilerV2Panel::State;

    namespace
    {
        constexpr double kNsPerMs = 1'000'000.0;
        constexpr double kMinViewNs = 10'000.0;
        constexpr double kMaxViewNs = 2'000'000'000.0;
        constexpr int64_t kGpuSpillNs = 200'000'000; // a frame's GPU work is looked for this far past its end
        constexpr float kLaneLabelWidth = 130.0f;
        constexpr float kRow = 18.0f;
        constexpr float kText = 12.0f;

        glm::vec4 Token(const UI::UITheme& theme, const char* name, const UI::UIColor& fallback)
        {
            const UI::UIColor c = theme.GetToken(name, fallback);
            return { c.r, c.g, c.b, c.a };
        }

        glm::vec4 Hsv(float h, float s, float v)
        {
            const float c = v * s, x = c * (1.0f - std::abs(std::fmod(h * 6.0f, 2.0f) - 1.0f)), m = v - c;
            glm::vec3 rgb = h < 1.0f / 6 ? glm::vec3(c, x, 0) : h < 2.0f / 6 ? glm::vec3(x, c, 0) : h < 3.0f / 6 ? glm::vec3(0, c, x)
                          : h < 4.0f / 6 ? glm::vec3(0, x, c) : h < 5.0f / 6 ? glm::vec3(x, 0, c) : glm::vec3(c, 0, x);
            return { rgb + m, 1.0f };
        }

        // A stable colour per scope (Tracy and Insights colour by name the same way); waits are grey.
        glm::vec4 ScopeColor(uint32_t scopeId, bool isWait, bool gpu)
        {
            if (isWait)
                return { 0.43f, 0.43f, 0.43f, 1.0f };
            const uint32_t hash = (scopeId + 1u) * 2654435761u;
            return Hsv(static_cast<float>(hash % 360u) / 360.0f, gpu ? 0.45f : 0.55f, gpu ? 0.85f : 0.75f);
        }

        double NiceStep(double minimum)
        {
            const double power = std::pow(10.0, std::floor(std::log10(minimum)));
            for (double factor : { 1.0, 2.0, 5.0, 10.0 })
                if (factor * power >= minimum)
                    return factor * power;
            return 10.0 * power;
        }

        std::string FormatDuration(double ns)
        {
            const double magnitude = std::abs(ns);
            if (magnitude >= 1'000'000.0)
                return std::format("{:.2f} ms", ns / kNsPerMs);
            if (magnitude >= 1'000.0)
                return std::format("{:.1f} us", ns / 1'000.0);
            return std::format("{:.0f} ns", ns);
        }

        bool ContainsCaseInsensitive(std::string_view text, std::string_view pattern)
        {
            if (pattern.empty())
                return true;
            const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            return std::search(text.begin(), text.end(), pattern.begin(), pattern.end(), [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
        }

        void FrameSpan(const TimelineFrame& frame, int64_t& start, int64_t& end)
        {
            start = frame.StartNs;
            end = frame.EndNs;
            for (const TimelineScope& scope : frame.Gpu)
            {
                start = std::min(start, scope.StartNs);
                end = std::max(end, scope.EndNs);
            }
        }

        const char* GpuQueueName(uint32_t lane)
        {
            return lane == 2 ? "GPU, transfer queue" : lane == 1 ? "GPU, async compute queue" : "GPU, graphics queue";
        }

        // The frame-time graph (fixed slots, newest on the right) or the memory graph under it.
        class FrameGraph final : public UI::UIElement
        {
        public:
            FrameGraph(std::shared_ptr<State> state, bool memory) : m_State(std::move(state)), m_Memory(memory)
            {
                HitTestMode = UI::UIHitTestMode::Self;
                WidthMode = UI::UISizeMode::Fill;
                HeightMode = UI::UISizeMode::Fixed;
                FixedSize = { 0.0f, memory ? 46.0f : 80.0f };
            }
            bool OnPointerMove(const UI::UIPointerEvent& e) override
            {
                const int32_t index = IndexAt(e.Position.x);
                if (index < 0)
                {
                    Tooltip.clear();
                    return true;
                }
                const TimelineFrame& f = m_State->Frame(static_cast<uint32_t>(index));
                if (m_Memory)
                    Tooltip = f.VramMB >= 0.0f ? std::format("Frame {}\nVRAM: {:.0f} MB\nRAM: {:.0f} MB", f.Number, f.VramMB, f.RamMB) : std::string();
                else
                {
                    Tooltip = std::format("Frame {}\nFrame: {:.2f} ms\nCPU: {:.2f} ms", f.Number, static_cast<double>(f.EndNs - f.StartNs) / kNsPerMs, f.CpuMs);
                    if (f.GpuMs >= 0.0)
                        Tooltip += std::format("\nGPU: {:.2f} ms", f.GpuMs);
                    if (f.VramMB >= 0.0f)
                        Tooltip += std::format("\nVRAM {:.0f} MB, RAM {:.0f} MB", f.VramMB, f.RamMB);
                    Tooltip += "\nClick: show this frame";
                }
                return true;
            }
            bool OnPointerDown(const UI::UIPointerEvent& e) override
            {
                if (e.Button != 1 || m_Memory)
                    return false;
                if (const int32_t index = IndexAt(e.Position.x); index >= 0)
                {
                    m_State->Follow = false;
                    m_State->SelectFrame(static_cast<uint32_t>(index));
                }
                return true;
            }

        protected:
            void OnPaint(UI::UIDrawList& d, const UI::UITheme& theme) override
            {
                const UI::UIRect b = GetBounds();
                d.AddRoundedRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y }, Token(theme, "ProfilerBackground", { 0.10f, 0.10f, 0.11f, 1.0f }));
                const uint32_t count = m_State->FrameCount();
                const float slot = b.Width() / static_cast<float>(m_State->FrameSlots());
                d.PushClipRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y });
                if (m_Memory)
                    PaintMemory(d, theme, b, count, slot);
                else
                    PaintFrames(d, theme, b, count, slot);
                d.PopClipRect();
            }

        private:
            int32_t IndexAt(float x) const
            {
                const UI::UIRect b = GetBounds();
                const uint32_t count = m_State->FrameCount();
                if (count == 0)
                    return -1;
                const float slot = b.Width() / static_cast<float>(m_State->FrameSlots());
                const int32_t index = static_cast<int32_t>(count) - 1 - static_cast<int32_t>((b.Max.x - x) / slot);
                return index >= 0 && index < static_cast<int32_t>(count) ? index : -1;
            }

            void PaintFrames(UI::UIDrawList& d, const UI::UITheme& theme, const UI::UIRect& b, uint32_t count, float slot)
            {
                double maxMs = 1000.0 / 30.0;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const TimelineFrame& f = m_State->Frame(i);
                    maxMs = std::max({ maxMs, static_cast<double>(f.EndNs - f.StartNs) / kNsPerMs, f.GpuMs });
                }
                maxMs *= 1.05;
                const auto toY = [&](double ms) { return b.Max.y - static_cast<float>(std::min(ms / maxMs, 1.0)) * b.Height(); };
                for (double reference : { 1000.0 / 60.0, 1000.0 / 30.0 })
                {
                    const float y = toY(reference);
                    d.AddRoundedRect({ b.Min.x, y }, { b.Max.x, y + 1.0f }, { 1.0f, 1.0f, 1.0f, 0.16f });
                    d.AddText(reference < 20.0 ? "60 fps" : "30 fps", nullptr, { b.Min.x + 2.0f, y - 14.0f }, kText, { 1.0f, 1.0f, 1.0f, 0.35f });
                }
                const glm::vec4 good = Token(theme, "ProfilerFrameGood", { 0.35f, 0.75f, 0.39f, 1.0f });
                const glm::vec4 slow = Token(theme, "ProfilerFrameSlow", { 0.86f, 0.75f, 0.27f, 1.0f });
                const glm::vec4 bad = Token(theme, "ProfilerFrameBad", { 0.86f, 0.31f, 0.27f, 1.0f });
                const glm::vec4 gpu = Token(theme, "ProfilerGpu", { 0.43f, 0.67f, 1.0f, 1.0f });
                const int32_t selected = m_State->HasSelection ? m_State->FindFrame(m_State->SelectedFrame) : -1;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const TimelineFrame& f = m_State->Frame(i);
                    const double ms = static_cast<double>(f.EndNs - f.StartNs) / kNsPerMs;
                    const float x0 = b.Max.x - static_cast<float>(count - i) * slot;
                    const float x1 = x0 + std::max(slot - 1.0f, 1.0f);
                    d.AddRoundedRect({ x0, toY(ms) }, { x1, b.Max.y }, ms <= 1000.0 / 60.0 + 0.1 ? good : ms <= 1000.0 / 30.0 + 0.1 ? slow : bad);
                    if (f.GpuMs >= 0.0)
                    {
                        const float y = toY(f.GpuMs);
                        d.AddRoundedRect({ x0, y - 1.0f }, { x1 + 1.0f, y + 1.0f }, gpu);
                    }
                    if (static_cast<int32_t>(i) == selected)
                        d.AddRoundedRect({ x0 - 1.0f, b.Min.y }, { x1 + 1.0f, b.Max.y }, glm::vec4(0.0f), glm::vec4(0.0f), 1.0f, { 1.0f, 1.0f, 1.0f, 0.86f });
                }
            }

            void PaintMemory(UI::UIDrawList& d, const UI::UITheme& theme, const UI::UIRect& b, uint32_t count, float slot)
            {
                float maxMB = 0.0f;
                for (uint32_t i = 0; i < count; ++i)
                    maxMB = std::max({ maxMB, m_State->Frame(i).VramMB, m_State->Frame(i).RamMB });
                if (maxMB <= 0.0f)
                {
                    d.AddText("No memory figures yet", nullptr, { b.Min.x + 4.0f, b.Min.y + 4.0f }, kText, { 1.0f, 1.0f, 1.0f, 0.4f });
                    return;
                }
                maxMB *= 1.1f;
                const glm::vec4 colors[2] = { Token(theme, "ProfilerVram", { 0.43f, 0.67f, 1.0f, 1.0f }), Token(theme, "ProfilerRam", { 0.92f, 0.63f, 0.31f, 1.0f }) };
                for (int series = 0; series < 2; ++series)
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const float mb = series == 0 ? m_State->Frame(i).VramMB : m_State->Frame(i).RamMB;
                        if (mb < 0.0f)
                            continue;
                        const float x = b.Max.x - (static_cast<float>(count - i) - 0.5f) * slot;
                        const float y = b.Max.y - std::min(mb / maxMB, 1.0f) * (b.Height() - 2.0f);
                        d.AddRoundedRect({ x - std::max(slot * 0.5f, 0.75f), y - 1.0f }, { x + std::max(slot * 0.5f, 0.75f), y + 1.0f }, colors[series]);
                    }
                d.AddText(std::format("{:.0f} MB", maxMB), nullptr, { b.Min.x + 2.0f, b.Min.y }, kText, { 1.0f, 1.0f, 1.0f, 0.35f });
                d.AddText("VRAM", nullptr, { b.Min.x + 2.0f, b.Max.y - 14.0f }, kText, colors[0]);
                d.AddText("RAM", nullptr, { b.Min.x + 40.0f, b.Max.y - 14.0f }, kText, colors[1]);
            }

            std::shared_ptr<State> m_State;
            bool m_Memory = false;
        };

        // The picked frame's timeline: GPU tracks, then the main thread, then the other threads; one row per scope depth.
        class Timeline final : public UI::UIElement
        {
        public:
            explicit Timeline(std::shared_ptr<State> state) : m_State(std::move(state))
            {
                HitTestMode = UI::UIHitTestMode::Self;
                WidthMode = UI::UISizeMode::Fill;
                HeightMode = UI::UISizeMode::Fill;
                ClipChildren = true;
            }

            bool OnMouseWheel(const UI::UIPointerEvent& e) override
            {
                if (SDL_GetModState() & SDL_KMOD_SHIFT)
                {
                    m_ScrollY = std::max(0.0f, m_ScrollY - e.ScrollDelta.y * 40.0f); // Shift+wheel: up / down through the lanes
                    return true;
                }
                State& s = *m_State;
                if (s.ViewDurationNs <= 0.0)
                    return true;
                const double anchor = ToNs(e.Position.x);
                const double duration = std::clamp(s.ViewDurationNs * std::pow(1.25, -static_cast<double>(e.ScrollDelta.y)), kMinViewNs, kMaxViewNs);
                s.ViewStartNs = anchor - (anchor - s.ViewStartNs) * (duration / s.ViewDurationNs);
                s.ViewDurationNs = duration;
                s.Follow = false;
                return true;
            }
            bool OnPointerDown(const UI::UIPointerEvent& e) override
            {
                m_Dragging = true;
                m_Moved = false;
                m_Last = e.Position;
                m_PressButton = e.Button;
                return true;
            }
            bool OnPointerMove(const UI::UIPointerEvent& e) override
            {
                if (m_Dragging)
                {
                    const float dx = e.Position.x - m_Last.x;
                    if (std::abs(e.Position.x - m_Last.x) + std::abs(e.Position.y - m_Last.y) > 0.0f)
                        m_Moved = m_Moved || std::abs(dx) > 1.0f;
                    if (m_Moved && m_State->ViewDurationNs > 0.0)
                    {
                        m_State->ViewStartNs -= static_cast<double>(dx / TrackWidth()) * m_State->ViewDurationNs;
                        m_State->Follow = false;
                    }
                    m_ScrollY = std::max(0.0f, m_ScrollY - (e.Position.y - m_Last.y));
                    m_Last = e.Position;
                }
                m_Pointer = e.Position;
                UpdateHover();
                return true;
            }
            bool OnPointerUp(const UI::UIPointerEvent& e) override
            {
                const bool click = m_Dragging && !m_Moved && e.Button == 1 && m_PressButton == 1;
                m_Dragging = false;
                if (!click || !m_Hovered.Valid)
                    return true;
                const auto now = std::chrono::steady_clock::now();
                if (now - m_LastClick < std::chrono::milliseconds(350))
                {
                    const double duration = std::max(static_cast<double>(m_Hovered.EndNs - m_Hovered.StartNs), kMinViewNs);
                    m_State->ViewStartNs = static_cast<double>(m_Hovered.StartNs) - duration * 0.1;
                    m_State->ViewDurationNs = duration * 1.2;
                    m_State->Follow = false;
                }
                else
                    m_State->HighlightScope = m_Hovered.ScopeId;
                m_LastClick = now;
                return true;
            }

        protected:
            void OnPaint(UI::UIDrawList& d, const UI::UITheme& theme) override;

        private:
            float TrackX() const { return GetBounds().Min.x + kLaneLabelWidth; }
            float TrackWidth() const { return std::max(GetBounds().Width() - kLaneLabelWidth, 1.0f); }
            double ToNs(float x) const { return m_State->ViewStartNs + static_cast<double>((x - TrackX()) / TrackWidth()) * m_State->ViewDurationNs; }
            float ToX(double ns) const { return TrackX() + static_cast<float>((ns - m_State->ViewStartNs) / m_State->ViewDurationNs) * TrackWidth(); }
            void UpdateHover()
            {
                Tooltip.clear();
                if (!m_Hovered.Valid)
                    return;
                const State::ScopeDetails details = m_State->Scope(m_Hovered.ScopeId);
                Tooltip = std::string(details.Name) + "\nDuration: " + FormatDuration(static_cast<double>(m_Hovered.EndNs - m_Hovered.StartNs)) +
                          "\nStart: " + FormatDuration(static_cast<double>(m_Hovered.StartNs - m_Hovered.FrameStartNs)) + " after frame " +
                          std::to_string(m_Hovered.FrameNumber) + " began\n" + m_Hovered.Where + ", depth " + std::to_string(m_Hovered.Depth);
                if (details.File[0] != '\0')
                {
                    std::string_view file(details.File);
                    if (const size_t slash = file.find_last_of("/\\"); slash != std::string_view::npos)
                        file.remove_prefix(slash + 1);
                    Tooltip += "\n" + std::string(details.Function) + "  " + std::string(file) + ":" + std::to_string(details.Line);
                }
                Tooltip += "\nClick: mark it; double-click: zoom to it";
            }

            struct Hovered
            {
                bool Valid = false;
                uint32_t ScopeId = 0, Depth = 0;
                int64_t StartNs = 0, EndNs = 0, FrameStartNs = 0;
                uint64_t FrameNumber = 0;
                std::string Where;
            };
            std::shared_ptr<State> m_State;
            bool m_Dragging = false, m_Moved = false;
            int m_PressButton = 0;
            UI::UIVec2 m_Last{ 0.0f }, m_Pointer{ -1.0f };
            float m_ScrollY = 0.0f;
            Hovered m_Hovered;
            std::chrono::steady_clock::time_point m_LastClick{};
        };

        void Timeline::OnPaint(UI::UIDrawList& d, const UI::UITheme& theme)
        {
            const UI::UIRect b = GetBounds();
            State& s = *m_State;
            d.AddRoundedRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y }, Token(theme, "ProfilerBackground", { 0.10f, 0.10f, 0.11f, 1.0f }));
            const glm::vec4 text{ 0.88f, 0.89f, 0.92f, 1.0f };
            const glm::vec4 dim{ 0.55f, 0.57f, 0.62f, 1.0f };
            const uint32_t frameCount = s.FrameCount();
            const int32_t selected = s.HasSelection ? s.FindFrame(s.SelectedFrame) : -1;
            m_Hovered.Valid = false;
            if (frameCount == 0 || selected < 0 || s.ViewDurationNs <= 0.0)
            {
                d.AddText(s.Capture ? "The capture has no frames here." : s.Source == ProfilerV2Panel::FrameSource::Spikes ? "No frame above the spike threshold yet."
                          : s.Paused ? "Paused with no frame kept." : "Waiting for frames...", nullptr, { b.Min.x + 8.0f, b.Min.y + 8.0f }, 13.0f, dim);
                return;
            }
            const TimelineFrame& selectedFrame = s.Frame(static_cast<uint32_t>(selected));
            const double viewEnd = s.ViewStartNs + s.ViewDurationNs;
            std::vector<const TimelineFrame*> frames;
            for (uint32_t i = 0; i < frameCount; ++i)
            {
                const TimelineFrame& f = s.Frame(i);
                if (static_cast<double>(f.EndNs + kGpuSpillNs) >= s.ViewStartNs && static_cast<double>(f.StartNs - kGpuSpillNs) <= viewEnd)
                    frames.push_back(&f);
            }

            const uint32_t threadCount = s.ThreadCount();
            std::vector<uint32_t> laneDepth(threadCount, 0);
            constexpr uint32_t kGpuTracks = 3;
            static constexpr const char* kGpuTrackNames[kGpuTracks] = { "GPU", "GPU Async Compute", "GPU Transfer" };
            uint32_t gpuDepth[kGpuTracks] = {};
            bool hasGpu[kGpuTracks] = {};
            const auto gpuTrack = [](const TimelineScope& scope) { return std::min(scope.Lane, kGpuTracks - 1); };
            const auto gpuRow = [&](const TimelineScope& scope) { return gpuTrack(scope) == 1 && scope.Depth > 0 ? scope.Depth - 1 : scope.Depth; };
            for (const TimelineFrame* f : frames)
            {
                for (const TimelineScope& scope : f->Cpu)
                    if (scope.Lane < threadCount)
                        laneDepth[scope.Lane] = std::max(laneDepth[scope.Lane], scope.Depth);
                for (const TimelineScope& scope : f->Gpu)
                {
                    gpuDepth[gpuTrack(scope)] = std::max(gpuDepth[gpuTrack(scope)], gpuRow(scope));
                    hasGpu[gpuTrack(scope)] = true;
                }
            }
            std::vector<uint32_t> laneOrder;
            for (uint32_t lane = 0; lane < threadCount; ++lane)
            {
                if (s.ThreadName(lane) == "Main Thread")
                    laneOrder.insert(laneOrder.begin(), lane);
                else
                    laneOrder.push_back(lane);
            }
            constexpr float kGap = 6.0f;
            std::vector<float> laneTop(threadCount, 0.0f);
            float y = kRow + kGap;
            float gpuTop[kGpuTracks] = {};
            for (uint32_t track = 0; track < kGpuTracks; ++track)
            {
                gpuTop[track] = y;
                if (hasGpu[track])
                    y += static_cast<float>(gpuDepth[track] + 1) * kRow + kGap;
            }
            for (uint32_t lane : laneOrder)
            {
                laneTop[lane] = y;
                y += static_cast<float>(laneDepth[lane] + 1) * kRow + kGap;
            }
            m_ScrollY = std::clamp(m_ScrollY, 0.0f, std::max(0.0f, y - b.Height()));
            const float top = b.Min.y - m_ScrollY;
            const float trackX = TrackX(), trackWidth = TrackWidth();

            d.PushClipRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y });
            const glm::vec4 gpuBackgrounds[kGpuTracks] = { Token(theme, "ProfilerGpuLane", { 0.16f, 0.19f, 0.25f, 1.0f }),
                                                           Token(theme, "ProfilerComputeLane", { 0.20f, 0.17f, 0.25f, 1.0f }),
                                                           Token(theme, "ProfilerTransferLane", { 0.16f, 0.22f, 0.20f, 1.0f }) };
            const glm::vec4 cpuBackground = Token(theme, "ProfilerCpuLane", { 0.14f, 0.14f, 0.16f, 1.0f });
            const auto drawLane = [&](float laneTopY, uint32_t depth, const std::string& name, const glm::vec4& background)
            {
                const float y0 = top + laneTopY, y1 = y0 + static_cast<float>(depth + 1) * kRow;
                d.AddRoundedRect({ b.Min.x, y0 }, { b.Max.x, y1 }, background);
                d.PushClipRect({ b.Min.x, y0 }, { trackX - 4.0f, y1 });
                d.AddText(name, nullptr, { b.Min.x + 4.0f, y0 + 2.0f }, kText, text);
                d.PopClipRect();
            };
            for (uint32_t track = 0; track < kGpuTracks; ++track)
                if (hasGpu[track])
                    drawLane(gpuTop[track], gpuDepth[track], kGpuTrackNames[track], gpuBackgrounds[track]);
            for (uint32_t lane : laneOrder)
                drawLane(laneTop[lane], laneDepth[lane], s.ThreadName(lane), cpuBackground);

            d.PushClipRect({ trackX, b.Min.y }, { trackX + trackWidth, b.Max.y });
            // Ruler: time from the picked frame's start.
            const double step = NiceStep(s.ViewDurationNs * 90.0 / std::max(trackWidth, 1.0f));
            const double origin = static_cast<double>(selectedFrame.StartNs);
            for (double tick = std::ceil((s.ViewStartNs - origin) / step) * step; origin + tick <= viewEnd; tick += step)
            {
                const float x = ToX(origin + tick);
                d.AddRoundedRect({ x, b.Min.y }, { x + 1.0f, b.Min.y + kRow * 0.6f }, dim);
                d.AddText(FormatDuration(tick), nullptr, { x + 3.0f, b.Min.y }, kText, dim);
            }
            for (const TimelineFrame* f : frames)
            {
                const float x = ToX(static_cast<double>(f->StartNs));
                const bool isSelected = f == &selectedFrame;
                d.AddRoundedRect({ x, b.Min.y + kRow }, { x + 1.0f, b.Max.y }, { 1.0f, 1.0f, 1.0f, isSelected ? 0.47f : 0.18f });
                d.AddText(std::format("Frame {}", f->Number), nullptr, { x + 3.0f, b.Min.y + kRow * 0.55f }, kText, isSelected ? text : dim);
            }

            const bool pointerIn = b.Contains(m_Pointer);
            const auto drawScope = [&](const TimelineScope& scope, const TimelineFrame& frame, float laneY, bool gpu)
            {
                if (static_cast<double>(scope.EndNs) < s.ViewStartNs || static_cast<double>(scope.StartNs) > viewEnd)
                    return;
                float x0 = ToX(static_cast<double>(scope.StartNs));
                float x1 = std::max(ToX(static_cast<double>(scope.EndNs)), x0 + 1.0f);
                const float y0 = top + laneY + static_cast<float>(gpu ? gpuRow(scope) : scope.Depth) * kRow;
                const float y1 = y0 + kRow - 1.0f;
                const State::ScopeDetails details = s.Scope(scope.ScopeId);
                d.AddRoundedRect({ x0, y0 }, { x1, y1 }, ScopeColor(scope.ScopeId, !gpu && details.IsWait, gpu));
                if (scope.ScopeId == s.HighlightScope)
                    d.AddRoundedRect({ x0, y0 }, { x1, y1 }, glm::vec4(0.0f), glm::vec4(0.0f), 2.0f, { 1.0f, 1.0f, 1.0f, 1.0f });
                x0 = std::max(x0, trackX);
                x1 = std::min(x1, trackX + trackWidth);
                if (x1 - x0 > 24.0f)
                {
                    d.PushClipRect({ x0, y0 }, { x1, y1 });
                    d.AddText(std::string(details.Name) + " " + FormatDuration(static_cast<double>(scope.EndNs - scope.StartNs)), nullptr, { x0 + 3.0f, y0 + 2.0f },
                              kText, { 0.06f, 0.06f, 0.06f, 1.0f });
                    d.PopClipRect();
                }
                if (pointerIn && m_Pointer.x >= x0 && m_Pointer.x < x1 && m_Pointer.y >= y0 && m_Pointer.y < y1)
                {
                    m_Hovered = { true, scope.ScopeId, scope.Depth, scope.StartNs, scope.EndNs, frame.StartNs, frame.Number,
                                  gpu ? std::string(GpuQueueName(scope.Lane)) : (scope.Lane < threadCount ? s.ThreadName(scope.Lane) : std::string("?")) };
                }
            };
            for (const TimelineFrame* f : frames)
            {
                for (const TimelineScope& scope : f->Gpu)
                    drawScope(scope, *f, gpuTop[gpuTrack(scope)], true);
                for (const TimelineScope& scope : f->Cpu)
                    if (scope.Lane < threadCount)
                        drawScope(scope, *f, laneTop[scope.Lane], false);
            }
            d.PopClipRect();
            d.PopClipRect();
            UpdateHover();
        }

        // The picked frame's scopes, summed per scope and track: click a header to sort by it, a row to mark the scope.
        class ScopeTable final : public UI::UIElement
        {
        public:
            explicit ScopeTable(std::shared_ptr<State> state) : m_State(std::move(state))
            {
                HitTestMode = UI::UIHitTestMode::Self;
                WidthMode = UI::UISizeMode::Fill;
                HeightMode = UI::UISizeMode::Fixed;
                FixedSize = { 0.0f, 210.0f };
            }
            bool OnMouseWheel(const UI::UIPointerEvent& e) override
            {
                m_Scroll = std::max(0.0f, m_Scroll - e.ScrollDelta.y * 3.0f * kRow);
                return true;
            }
            bool OnPointerUp(const UI::UIPointerEvent& e) override
            {
                if (e.Button != 1)
                    return false;
                const UI::UIRect b = GetBounds();
                if (e.Position.y < b.Min.y + kRow)
                {
                    const int column = ColumnAt(e.Position.x);
                    if (column == m_SortColumn)
                        m_Descending = !m_Descending;
                    else
                    {
                        m_SortColumn = column;
                        m_Descending = column >= 2; // numbers: biggest first
                    }
                    return true;
                }
                const int row = static_cast<int>((e.Position.y - b.Min.y - kRow + m_Scroll) / kRow);
                if (row >= 0 && row < static_cast<int>(m_Rows.size()))
                    m_State->HighlightScope = m_Rows[static_cast<size_t>(row)].ScopeId == m_State->HighlightScope ? ~0u : m_Rows[static_cast<size_t>(row)].ScopeId;
                return true;
            }

        protected:
            void OnPaint(UI::UIDrawList& d, const UI::UITheme& theme) override
            {
                const UI::UIRect b = GetBounds();
                State& s = *m_State;
                d.AddRoundedRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y }, Token(theme, "ProfilerBackground", { 0.10f, 0.10f, 0.11f, 1.0f }));
                const int32_t selected = s.HasSelection ? s.FindFrame(s.SelectedFrame) : -1;
                m_Rows.clear();
                if (selected < 0)
                    return;
                const TimelineFrame& frame = s.Frame(static_cast<uint32_t>(selected));
                std::unordered_map<uint64_t, size_t> index;
                const auto add = [&](const TimelineScope& scope, uint32_t track)
                {
                    const uint64_t key = (static_cast<uint64_t>(scope.ScopeId) << 2) | track;
                    auto [entry, added] = index.try_emplace(key, m_Rows.size());
                    if (added)
                        m_Rows.push_back({ scope.ScopeId, track });
                    Row& row = m_Rows[entry->second];
                    const double ms = static_cast<double>(scope.EndNs - scope.StartNs) / kNsPerMs;
                    row.Calls++;
                    row.TotalMs += ms;
                    row.MaxMs = std::max(row.MaxMs, ms);
                };
                for (const TimelineScope& scope : frame.Cpu)
                    add(scope, 0);
                for (const TimelineScope& scope : frame.Gpu)
                    add(scope, std::min(scope.Lane, 2u) + 1u);
                std::erase_if(m_Rows, [&](const Row& row) { return !ContainsCaseInsensitive(s.Scope(row.ScopeId).Name, s.Filter); });
                std::stable_sort(m_Rows.begin(), m_Rows.end(), [&](const Row& a, const Row& c)
                {
                    const auto less = [&](const Row& l, const Row& r)
                    {
                        switch (m_SortColumn)
                        {
                        case 0: return std::string_view(s.Scope(l.ScopeId).Name) < std::string_view(s.Scope(r.ScopeId).Name);
                        case 1: return l.Track < r.Track;
                        case 2: return l.Calls < r.Calls;
                        case 4: return l.MaxMs < r.MaxMs;
                        default: return l.TotalMs < r.TotalMs;
                        }
                    };
                    return m_Descending ? less(c, a) : less(a, c);
                });
                const double frameMs = static_cast<double>(frame.EndNs - frame.StartNs) / kNsPerMs;
                m_Scroll = std::clamp(m_Scroll, 0.0f, std::max(0.0f, static_cast<float>(m_Rows.size()) * kRow - (b.Height() - kRow)));

                static constexpr const char* kHeaders[6] = { "Scope", "Track", "Calls", "Total (ms)", "Max (ms)", "% of frame" };
                const glm::vec4 text{ 0.86f, 0.88f, 0.92f, 1.0f };
                const glm::vec4 dim{ 0.55f, 0.57f, 0.62f, 1.0f };
                d.PushClipRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Max.y });
                d.AddRoundedRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Min.y + kRow }, { 1.0f, 1.0f, 1.0f, 0.05f });
                for (int column = 0; column < 6; ++column)
                {
                    std::string header = kHeaders[column];
                    if (column == m_SortColumn)
                        header += m_Descending ? "  v" : "  ^";
                    d.AddText(header, nullptr, { ColumnX(column) + 4.0f, b.Min.y + 2.0f }, kText, dim);
                }
                d.PushClipRect({ b.Min.x, b.Min.y + kRow }, { b.Max.x, b.Max.y });
                float y = b.Min.y + kRow - m_Scroll;
                for (size_t i = 0; i < m_Rows.size(); ++i, y += kRow)
                {
                    if (y + kRow < b.Min.y + kRow || y > b.Max.y)
                        continue;
                    const Row& row = m_Rows[i];
                    if (row.ScopeId == s.HighlightScope)
                        d.AddRoundedRect({ b.Min.x, y }, { b.Max.x, y + kRow }, { 0.30f, 0.45f, 0.75f, 0.45f });
                    else if (i % 2)
                        d.AddRoundedRect({ b.Min.x, y }, { b.Max.x, y + kRow }, { 1.0f, 1.0f, 1.0f, 0.03f });
                    const double reference = row.Track != 0 ? frame.GpuMs : frameMs;
                    const std::string cells[6] = {
                        s.Scope(row.ScopeId).Name,
                        row.Track == 3 ? "GPU (transfer)" : row.Track == 2 ? "GPU (async compute)" : row.Track == 1 ? "GPU" : (s.Scope(row.ScopeId).IsWait ? "CPU (wait)" : "CPU"),
                        std::to_string(row.Calls), std::format("{:.3f}", row.TotalMs), std::format("{:.3f}", row.MaxMs),
                        reference > 0.0 ? std::format("{:.1f} %", row.TotalMs / reference * 100.0) : std::string("-") };
                    for (int column = 0; column < 6; ++column)
                    {
                        d.PushClipRect({ ColumnX(column), y }, { ColumnX(column + 1) - 4.0f, y + kRow });
                        d.AddText(cells[column], nullptr, { ColumnX(column) + 4.0f, y + 2.0f }, kText, text);
                        d.PopClipRect();
                    }
                }
                d.PopClipRect();
                d.PopClipRect();
            }

        private:
            struct Row { uint32_t ScopeId; uint32_t Track; uint32_t Calls = 0; double TotalMs = 0.0; double MaxMs = 0.0; };
            float ColumnX(int column) const
            {
                static constexpr float kWeights[7] = { 0.0f, 0.38f, 0.52f, 0.62f, 0.74f, 0.86f, 1.0f };
                const UI::UIRect b = GetBounds();
                return b.Min.x + b.Width() * kWeights[std::clamp(column, 0, 6)];
            }
            int ColumnAt(float x) const
            {
                for (int column = 0; column < 6; ++column)
                    if (x < ColumnX(column + 1))
                        return column;
                return 5;
            }
            std::shared_ptr<State> m_State;
            std::vector<Row> m_Rows;
            int m_SortColumn = 3;
            bool m_Descending = true;
            float m_Scroll = 0.0f;
        };
    }

    // ---- State ----

    uint32_t State::FrameCount() const
    {
        if (Capture)
            return static_cast<uint32_t>(Source == FrameSource::Spikes ? Capture->Spikes.size() : Capture->Frames.size());
        const Profiler& profiler = Profiler::Get();
        return Source == FrameSource::Spikes ? profiler.GetSpikeFrameCount() : profiler.GetTimelineFrameCount();
    }
    const TimelineFrame& State::Frame(uint32_t index) const
    {
        if (Capture)
            return Source == FrameSource::Spikes ? Capture->Spikes[index] : Capture->Frames[index];
        const Profiler& profiler = Profiler::Get();
        return Source == FrameSource::Spikes ? profiler.GetSpikeFrame(index) : profiler.GetTimelineFrame(index);
    }
    uint32_t State::FrameSlots() const
    {
        const uint32_t capacity = Source == FrameSource::Spikes ? Profiler::MaxSpikeFrames : Profiler::TimelineFrames;
        return std::max({ capacity, FrameCount(), 1u });
    }
    uint32_t State::ThreadCount() const
    {
        return Capture ? static_cast<uint32_t>(Capture->ThreadNames.size()) : Profiler::Get().GetTimelineThreadCount();
    }
    const std::string& State::ThreadName(uint32_t lane) const
    {
        return Capture ? Capture->ThreadNames[lane] : Profiler::Get().GetTimelineThreadName(lane);
    }
    State::ScopeDetails State::Scope(uint32_t scopeId) const
    {
        ScopeDetails details;
        if (Capture)
        {
            if (scopeId < Capture->Scopes.size())
            {
                const ProfileCaptureScope& scope = Capture->Scopes[scopeId];
                details = { scope.Name.c_str(), scope.Function.c_str(), scope.File.c_str(), scope.Line, scope.IsWait };
            }
        }
        else if (const ProfileScopeInfo* info = Profiler::Get().GetScopeInfo(scopeId))
            details = { info->Name ? info->Name : "?", info->Function ? info->Function : "", info->File ? info->File : "", info->Line, info->IsWait };
        return details;
    }
    int32_t State::FindFrame(uint64_t number) const
    {
        for (int32_t index = static_cast<int32_t>(FrameCount()) - 1; index >= 0; --index)
        {
            const uint64_t frameNumber = Frame(static_cast<uint32_t>(index)).Number;
            if (frameNumber == number)
                return index;
            if (frameNumber < number)
                break;
        }
        return -1;
    }
    void State::SelectFrame(uint32_t index)
    {
        const TimelineFrame& frame = Frame(index);
        SelectedFrame = frame.Number;
        HasSelection = true;
        int64_t start = 0, end = 0;
        FrameSpan(frame, start, end);
        const double duration = std::max(static_cast<double>(end - start), kMinViewNs);
        ViewStartNs = static_cast<double>(start) - duration * 0.03;
        ViewDurationNs = duration * 1.06;
    }
    void State::FollowNewest()
    {
        // The newest frame whose GPU timings came back (they arrive a few frames late).
        const uint32_t count = FrameCount();
        const bool gpu = Profiler::Get().IsGpuTimingAvailable();
        for (int32_t index = static_cast<int32_t>(count) - 1; index >= 0 && index >= static_cast<int32_t>(count) - 8; --index)
            if (!gpu || Frame(static_cast<uint32_t>(index)).GpuMs >= 0.0)
            {
                SelectFrame(static_cast<uint32_t>(index));
                return;
            }
        if (count > 0)
            SelectFrame(count - 1);
    }

    // ---- Panel ----

    const Ref<UI::UIElement>& ProfilerV2Panel::BuildUI(UI::UIContext* context)
    {
        m_Context = context;
        m_Syncs.clear();
        auto root = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
        root->WidthMode = UI::UISizeMode::Fill;
        root->HeightMode = UI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        root->Padding = UI::UIThickness::All(8.0f);
        m_Root = root;
        std::shared_ptr<State> s = m_State;

#if NOX_PROFILE_STATS
        auto bar = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
        bar->WidthMode = UI::UISizeMode::Fill;
        Ref<UI::UIButton> closeCapture(new UI::UIButton("Close Capture", [s]() { s->Capture.reset(); s->HasSelection = false; s->Follow = true; }));
        closeCapture->Tooltip = "Back to the live frames";
        Ref<UI::UIButton> pause(new UI::UIButton("Pause", [s]() { s->Paused = !s->Paused; }));
        Ref<UI::UICheckBox> follow(new UI::UICheckBox("Follow", s->Follow, [s](bool v) { s->Follow = v; }));
        follow->Tooltip = "Shows the newest frame (zooming, panning or picking a frame turns this off)";
        Ref<UI::UIButton> clear(new UI::UIButton("Clear", [s]() { Profiler::Get().ClearTimeline(); s->HasSelection = false; }));
        clear->Tooltip = "Drops the kept frames and spikes";
        Ref<UI::UIButton> save(new UI::UIButton("Save", [this]() { SaveCapture(); }));
        save->Tooltip = "Writes the kept frames and spikes to a .noxprof file";
        Ref<UI::UIButton> load(new UI::UIButton("Load", [this]() { LoadCapture(); }));
        load->Tooltip = "Opens a .noxprof file (shown instead of the live frames until closed)";
        auto source = UI::MakeUI<UI::UIDropdown>(context, std::vector<std::string>{ "Recent Frames", "Spikes" }, 0, [s](int index)
        {
            s->Source = index == 1 ? FrameSource::Spikes : FrameSource::Recent;
            s->HasSelection = false;
        });
        source->WidthMode = UI::UISizeMode::Fixed;
        source->FixedSize = { 150.0f, 24.0f };
        source->Tooltip = "Spikes: the last " + std::to_string(Profiler::MaxSpikeFrames) + " frames whose frame or GPU time went above the threshold, kept "
                          "after they scrolled out of the recent frames";
        auto threshold = UI::MakeUI<UI::UIDragScalar>(Profiler::Get().GetSpikeThresholdMs(), 0.1f, [](float v) { Profiler::Get().SetSpikeThresholdMs(std::clamp(v, 1.0f, 1000.0f)); });
        threshold->Min = 1.0f;
        threshold->Max = 1000.0f;
        threshold->Prefix = "Spike > ";
        threshold->Format = "{:.1f} ms";
        threshold->WidthMode = UI::UISizeMode::Fixed;
        threshold->FixedSize = { 130.0f, 24.0f };
        threshold->Tooltip = "A frame slower than this (frame or GPU time) is kept as a spike";
        Ref<UI::UICheckBox> memory(new UI::UICheckBox("Memory", s->ShowMemory, [s](bool v) { s->ShowMemory = v; }));
        memory->Tooltip = "VRAM (device-local heaps) and process RAM under the frame graph";
        auto message = UI::Label("", 13.0f);
        Ref<UI::UILabel> messageRef = message;
        for (const Ref<UI::UIElement>& element : std::initializer_list<Ref<UI::UIElement>>{ closeCapture, pause, follow, clear, save, load, source, threshold, memory, messageRef })
            bar->AddChild(element);
        root->AddChild(bar);

        auto info = UI::Label("", 13.5f);
        Ref<UI::UILabel> infoRef = info;
        infoRef->WordWrap = true;
        infoRef->WidthMode = UI::UISizeMode::Fill;
        infoRef->Tooltip = "Graph: click a frame to show it. Timeline: wheel zooms, drag pans, Shift+wheel scrolls the lanes, click a scope to "
                           "mark it in the timeline and the table, double-click zooms to it. On the GPU tracks a pass sits where the GPU ran it: "
                           "usually after the CPU frame that recorded it.";
        root->AddChild(infoRef);

        Ref<FrameGraph> graph(new FrameGraph(s, false));
        Ref<FrameGraph> memoryGraph(new FrameGraph(s, true));
        root->AddChild(graph);
        root->AddChild(memoryGraph);
        root->AddChild(Ref<Timeline>(new Timeline(s)));

        auto tableBar = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
        tableBar->WidthMode = UI::UISizeMode::Fill;
        auto filter = UI::MakeUI<UI::UISearchBar>();
        filter->WidthMode = UI::UISizeMode::Fixed;
        filter->FixedSize = { 220.0f, 24.0f };
        filter->OnSearchChanged = [s](const std::string& text) { s->Filter = text; };
        tableBar->AddChild(filter);
        auto marked = UI::Label("", 13.0f);
        Ref<UI::UILabel> markedRef = marked;
        tableBar->AddChild(markedRef);
        Ref<UI::UIButton> unmark(new UI::UIButton("Unmark", [s]() { s->HighlightScope = ~0u; }));
        tableBar->AddChild(unmark);
        root->AddChild(tableBar);
        root->AddChild(Ref<ScopeTable>(new ScopeTable(s)));

        m_Syncs.push_back([this, s, closeCapture, pause, follow, clear, save, threshold, source, memoryGraph, messageRef, infoRef, markedRef, unmark]()
        {
            const bool capture = s->Capture != nullptr;
            closeCapture->Visible = capture;
            pause->Visible = clear->Visible = save->Visible = follow->Visible = !capture;
            pause->SetText(s->Paused ? "Record" : "Pause");
            pause->Tooltip = s->Paused ? "Keep the frames again" : "Stop keeping frames, to look at the ones kept";
            follow->Checked = s->Follow;
            threshold->SetEnabled(!capture);
            if (!threshold->IsInteracting())
                threshold->Value = capture ? s->Capture->SpikeThresholdMs : Profiler::Get().GetSpikeThresholdMs();
            const uint32_t spikes = capture ? static_cast<uint32_t>(s->Capture->Spikes.size()) : Profiler::Get().GetSpikeFrameCount();
            const std::vector<std::string> items{ "Recent Frames", "Spikes (" + std::to_string(spikes) + ")" };
            if (source->GetItems() != items)
            {
                source->SetItems(items);
                source->SetSelectedIndex(s->Source == FrameSource::Spikes ? 1 : 0, false);
            }
            memoryGraph->Visible = s->ShowMemory;
            messageRef->Text = capture ? "Capture: " + s->CaptureName + (m_Message.empty() ? "" : "  |  " + m_Message) : m_Message;
            const int32_t selected = s->HasSelection ? s->FindFrame(s->SelectedFrame) : -1;
            std::string text = "No frame picked";
            if (selected >= 0)
            {
                const TimelineFrame& f = s->Frame(static_cast<uint32_t>(selected));
                const double ms = static_cast<double>(f.EndNs - f.StartNs) / kNsPerMs;
                text = f.GpuMs >= 0.0 ? std::format("Frame {}: {:.2f} ms (CPU {:.2f} ms, GPU {:.2f} ms)", f.Number, ms, f.CpuMs, f.GpuMs)
                                      : std::format("Frame {}: {:.2f} ms (CPU {:.2f} ms)", f.Number, ms, f.CpuMs);
                if (f.VramMB >= 0.0f)
                    text += std::format("  |  VRAM {:.0f} MB, RAM {:.0f} MB", f.VramMB, f.RamMB);
                if (f.DroppedScopes > 0)
                    text += std::format("  |  {} scopes not kept", f.DroppedScopes);
                if (!f.Gpu.empty() && !f.GpuCalibrated)
                    text += "  |  GPU not on the CPU clock (no calibrated timestamps: the GPU track is drawn from the frame's start)";
            }
            infoRef->Text = text;
            markedRef->Text = s->HighlightScope != ~0u ? std::string("Marked: ") + s->Scope(s->HighlightScope).Name : std::string();
            unmark->Visible = s->HighlightScope != ~0u;
        });
#else
        root->AddChild(UI::Label("This build has no profiler data (NOX_PROFILE_STATS is off).", 14.0f));
#endif
        return m_Root;
    }

    void ProfilerV2Panel::Update(bool shown)
    {
        Profiler& profiler = Profiler::Get();
        // Frames are copied only while someone looks (the copies cost a little every frame).
        profiler.SetTimelineCapture(shown && !m_State->Paused && !m_State->Capture);
        if (!shown || !m_Root)
            return;
#if NOX_PROFILE_STATS
        if (m_State->Follow && (!m_State->Paused || m_State->Source == FrameSource::Spikes) && !m_State->Capture)
            m_State->FollowNewest();
#endif
        for (const auto& sync : m_Syncs)
            sync();
    }

    void ProfilerV2Panel::SaveCapture()
    {
        Utility::SaveFileAsync("Nox Profiler Capture\0noxprof\0", [this](std::string chosen)
        {
            if (chosen.empty())
                return;
            std::filesystem::path path(chosen);
            if (path.extension() != ".noxprof")
                path += ".noxprof";
            m_Message = Profiler::Get().SaveTimeline(path.string()) ? std::format("Saved {}", path.filename().string()) : std::format("Could not write {}", path.string());
        });
    }

    void ProfilerV2Panel::LoadCapture()
    {
        Utility::OpenFileAsync("Nox Profiler Capture\0noxprof\0", [this](std::string path) { LoadCaptureFrom(path); });
    }

    void ProfilerV2Panel::LoadCaptureFrom(const std::string& path)
    {
        if (path.empty())
            return;
        auto capture = std::make_unique<ProfileCapture>();
        if (!Profiler::LoadCapture(path, *capture))
        {
            m_Message = std::format("Not a Nox profiler capture: {}", path);
            return;
        }
        State& s = *m_State;
        s.Capture = std::move(capture);
        s.CaptureName = std::filesystem::path(path).filename().string();
        m_Message.clear();
        s.HasSelection = false;
        s.Follow = false;
        s.HighlightScope = ~0u;
        if (const uint32_t count = s.FrameCount(); count > 0)
            s.SelectFrame(count - 1);
    }
}
