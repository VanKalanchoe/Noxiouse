#include "ProfilerPanel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>

#include <imgui.h>

#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    namespace
    {
        constexpr double NanosecondsPerMillisecond = 1'000'000.0;
        constexpr float FrameGraphHeight = 80.0f;
        constexpr float MemoryGraphHeight = 46.0f;
        constexpr ImU32 VramColor = IM_COL32(110, 170, 255, 255);
        constexpr ImU32 RamColor = IM_COL32(235, 160, 80, 255);
        constexpr float LaneLabelWidth = 130.0f;
        constexpr double MinViewNs = 10'000.0;          // 10 us
        constexpr double MaxViewNs = 2'000'000'000.0;   // 2 s
        // GPU work runs after the CPU frame that recorded it: a frame's GPU scopes are looked for this far past its end.
        constexpr int64_t GpuSpillNs = 200'000'000;

        const ImVec4 WarningColor{ 1.0f, 0.75f, 0.3f, 1.0f };

        // A stable color per scope (Tracy and Insights color by name the same way); waits are gray.
        ImU32 ScopeColor(uint32_t scopeId, bool isWait, bool gpu)
        {
            if (isWait)
                return IM_COL32(110, 110, 110, 255);
            const uint32_t hash = (scopeId + 1u) * 2654435761u;
            const float hue = static_cast<float>(hash % 360u) / 360.0f;
            return ImColor::HSV(hue, gpu ? 0.45f : 0.55f, gpu ? 0.85f : 0.75f);
        }

        ImU32 FrameTimeColor(double ms)
        {
            if (ms <= 1000.0 / 60.0 + 0.1)
                return IM_COL32(90, 190, 100, 255);
            if (ms <= 1000.0 / 30.0 + 0.1)
                return IM_COL32(220, 190, 70, 255);
            return IM_COL32(220, 80, 70, 255);
        }

        // 1, 2 or 5 times a power of ten, at least the given step.
        double NiceStep(double minimum)
        {
            const double power = std::pow(10.0, std::floor(std::log10(minimum)));
            for (double factor : { 1.0, 2.0, 5.0, 10.0 })
            {
                if (factor * power >= minimum)
                    return factor * power;
            }
            return 10.0 * power;
        }

        std::string FormatDuration(double ns)
        {
            const double magnitude = std::abs(ns);
            if (magnitude >= 1'000'000.0)
                return std::format("{:.2f} ms", ns / NanosecondsPerMillisecond);
            if (magnitude >= 1'000.0)
                return std::format("{:.1f} us", ns / 1'000.0);
            return std::format("{:.0f} ns", ns);
        }

        bool ContainsCaseInsensitive(std::string_view text, std::string_view pattern)
        {
            if (pattern.empty())
                return true;
            auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            return std::search(text.begin(), text.end(), pattern.begin(), pattern.end(),
                               [&lower](char a, char b) { return lower(a) == lower(b); }) != text.end();
        }

        // The frame and the GPU work it recorded.
        void FrameSpan(const TimelineFrame& frame, int64_t& outStart, int64_t& outEnd)
        {
            outStart = frame.StartNs;
            outEnd = frame.EndNs;
            for (const TimelineScope& scope : frame.Gpu)
            {
                outStart = std::min(outStart, scope.StartNs);
                outEnd = std::max(outEnd, scope.EndNs);
            }
        }

        const char* GpuQueueName(uint32_t lane)
        {
            return lane == 2 ? "GPU, transfer queue" : lane == 1 ? "GPU, async compute queue" : "GPU, graphics queue";
        }
    }

    ProfilerPanel::ProfilerPanel() = default;
    ProfilerPanel::~ProfilerPanel() = default;

    uint32_t ProfilerPanel::FrameCount() const
    {
        if (m_Capture)
            return static_cast<uint32_t>(m_Source == FrameSource::Spikes ? m_Capture->Spikes.size() : m_Capture->Frames.size());
        const Profiler& profiler = Profiler::Get();
        return m_Source == FrameSource::Spikes ? profiler.GetSpikeFrameCount() : profiler.GetTimelineFrameCount();
    }

    const TimelineFrame& ProfilerPanel::Frame(uint32_t index) const
    {
        if (m_Capture)
            return m_Source == FrameSource::Spikes ? m_Capture->Spikes[index] : m_Capture->Frames[index];
        const Profiler& profiler = Profiler::Get();
        return m_Source == FrameSource::Spikes ? profiler.GetSpikeFrame(index) : profiler.GetTimelineFrame(index);
    }

    uint32_t ProfilerPanel::FrameSlots() const
    {
        const uint32_t capacity = m_Source == FrameSource::Spikes ? Profiler::MaxSpikeFrames : Profiler::TimelineFrames;
        return std::max({ capacity, FrameCount(), 1u });
    }

    uint32_t ProfilerPanel::ThreadCount() const
    {
        return m_Capture ? static_cast<uint32_t>(m_Capture->ThreadNames.size()) : Profiler::Get().GetTimelineThreadCount();
    }

    const std::string& ProfilerPanel::ThreadName(uint32_t lane) const
    {
        return m_Capture ? m_Capture->ThreadNames[lane] : Profiler::Get().GetTimelineThreadName(lane);
    }

    ProfilerPanel::ScopeDetails ProfilerPanel::Scope(uint32_t scopeId) const
    {
        ScopeDetails details;
        if (m_Capture)
        {
            if (scopeId < m_Capture->Scopes.size())
            {
                const ProfileCaptureScope& scope = m_Capture->Scopes[scopeId];
                details = { scope.Name.c_str(), scope.Function.c_str(), scope.File.c_str(), scope.Line, scope.IsWait };
            }
        }
        else if (const ProfileScopeInfo* info = Profiler::Get().GetScopeInfo(scopeId))
        {
            details = { info->Name ? info->Name : "?", info->Function ? info->Function : "", info->File ? info->File : "", info->Line, info->IsWait };
        }
        return details;
    }

    void ProfilerPanel::ScopeTooltip(const TimelineScope& scope, const TimelineFrame& frame, bool gpu) const
    {
        const ScopeDetails details = Scope(scope.ScopeId);
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(details.Name);
        ImGui::Separator();
        ImGui::Text("Duration: %s", FormatDuration(static_cast<double>(scope.EndNs - scope.StartNs)).c_str());
        ImGui::Text("Start: %s after frame %llu began", FormatDuration(static_cast<double>(scope.StartNs - frame.StartNs)).c_str(),
                    static_cast<unsigned long long>(frame.Number));
        ImGui::Text("%s, depth %u", gpu ? GpuQueueName(scope.Lane) : (scope.Lane < ThreadCount() ? ThreadName(scope.Lane).c_str() : "?"),
                    scope.Depth);
        if (details.File[0] != '\0')
        {
            std::string_view file(details.File);
            if (const size_t slash = file.find_last_of("/\\"); slash != std::string_view::npos)
                file.remove_prefix(slash + 1);
            ImGui::TextDisabled("%s  %.*s:%u", details.Function, static_cast<int>(file.size()), file.data(), details.Line);
        }
        ImGui::EndTooltip();
    }

    void ProfilerPanel::OnImGuiRender()
    {
        Profiler& profiler = Profiler::Get();
        if (!m_Open)
        {
            profiler.SetTimelineCapture(false);
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(1100.0f, 650.0f), ImGuiCond_FirstUseEver);
        const bool visible = ImGui::Begin("Profiler", &m_Open);
        // Kept only while someone looks at it: the copies cost a little every frame.
        profiler.SetTimelineCapture(visible && m_Open && !m_Paused && !m_Capture);
        if (!visible)
        {
            ImGui::End();
            return;
        }

#if NOX_PROFILE_STATS
        if (m_Follow && (!m_Paused || m_Source == FrameSource::Spikes) && !m_Capture)
            FollowNewest();

        DrawToolbar();
        DrawFrameGraph();
        if (m_ShowMemory)
            DrawMemoryGraph();

        const float available = ImGui::GetContentRegionAvail().y;
        const float tableHeight = std::max(140.0f, available * 0.3f);
        ImGui::BeginChild("##Timeline", ImVec2(0.0f, std::max(100.0f, available - tableHeight - ImGui::GetStyle().ItemSpacing.y)),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollWithMouse);
        DrawTimeline();
        ImGui::EndChild();

        DrawScopeTable();
#else
        ImGui::TextWrapped("This build has no profiler data (NOX_PROFILE_STATS is off).");
#endif
        ImGui::End();
    }

    int32_t ProfilerPanel::FindFrame(uint64_t number) const
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

    void ProfilerPanel::SelectFrame(uint32_t index)
    {
        const TimelineFrame& frame = Frame(index);
        m_SelectedFrame = frame.Number;
        m_HasSelection = true;
        int64_t start = 0;
        int64_t end = 0;
        FrameSpan(frame, start, end);
        const double duration = std::max(static_cast<double>(end - start), MinViewNs);
        m_ViewStartNs = static_cast<double>(start) - duration * 0.03;
        m_ViewDurationNs = duration * 1.06;
    }

    void ProfilerPanel::FollowNewest()
    {
        // The newest frame whose GPU timings came back (they arrive a few frames late), the newest one without a GPU profiler.
        const uint32_t count = FrameCount();
        const bool gpu = Profiler::Get().IsGpuTimingAvailable();
        for (int32_t index = static_cast<int32_t>(count) - 1; index >= 0 && index >= static_cast<int32_t>(count) - 8; --index)
        {
            if (!gpu || Frame(static_cast<uint32_t>(index)).GpuMs >= 0.0)
            {
                SelectFrame(static_cast<uint32_t>(index));
                return;
            }
        }
        if (count > 0)
            SelectFrame(count - 1);
    }

    void ProfilerPanel::DrawToolbar()
    {
        Profiler& profiler = Profiler::Get();
        if (m_Capture)
        {
            if (ImGui::Button("Close Capture"))
            {
                m_Capture.reset();
                m_HasSelection = false;
                m_Follow = true;
                m_Message.clear();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Back to the live frames");
            ImGui::SameLine();
            ImGui::TextColored(WarningColor, "Capture: %s", m_CaptureName.c_str());
        }
        else
        {
            if (ImGui::Button(m_Paused ? "Record" : "Pause", ImVec2(70.0f, 0.0f)))
                m_Paused = !m_Paused;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(m_Paused ? "Keep the frames again" : "Stop keeping frames, to look at the ones kept");
            ImGui::SameLine();
            ImGui::Checkbox("Follow", &m_Follow);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Shows the newest frame (zooming, panning or picking a frame turns this off)");
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
            {
                profiler.ClearTimeline();
                m_HasSelection = false;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Drops the kept frames and spikes");
            ImGui::SameLine();
            if (ImGui::Button("Save"))
                SaveCapture();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Writes the kept frames and spikes to a .noxprof file");
        }
        ImGui::SameLine();
        if (ImGui::Button("Load"))
            LoadCapture();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Opens a .noxprof file (shown instead of the live frames until closed)");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        const uint32_t spikeCount = m_Capture ? static_cast<uint32_t>(m_Capture->Spikes.size()) : profiler.GetSpikeFrameCount();
        const std::string spikesLabel = std::format("Spikes ({})", spikeCount);
        if (ImGui::BeginCombo("##Source", m_Source == FrameSource::Spikes ? spikesLabel.c_str() : "Recent Frames"))
        {
            if (ImGui::Selectable("Recent Frames", m_Source == FrameSource::Recent))
            {
                m_Source = FrameSource::Recent;
                m_HasSelection = false;
            }
            if (ImGui::Selectable(spikesLabel.c_str(), m_Source == FrameSource::Spikes))
            {
                m_Source = FrameSource::Spikes;
                m_HasSelection = false;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Spikes: the last %u frames whose frame or GPU time went above the threshold, kept after they\n"
                              "scrolled out of the recent frames", Profiler::MaxSpikeFrames);
        ImGui::SameLine();
        if (m_Capture)
        {
            ImGui::TextDisabled("(spike threshold %.1f ms)", m_Capture->SpikeThresholdMs);
        }
        else
        {
            float threshold = profiler.GetSpikeThresholdMs();
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::DragFloat("##SpikeThreshold", &threshold, 0.1f, 1.0f, 1000.0f, "Spike > %.1f ms"))
                profiler.SetSpikeThresholdMs(std::clamp(threshold, 1.0f, 1000.0f));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A frame slower than this (frame or GPU time) is kept as a spike");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Memory", &m_ShowMemory);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("VRAM (device-local heaps) and process RAM under the frame graph");

        if (!m_Message.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("| %s", m_Message.c_str());
        }

        const int32_t selected = m_HasSelection ? FindFrame(m_SelectedFrame) : -1;
        if (selected >= 0)
        {
            const TimelineFrame& frame = Frame(static_cast<uint32_t>(selected));
            const double frameMs = static_cast<double>(frame.EndNs - frame.StartNs) / NanosecondsPerMillisecond;
            if (frame.GpuMs >= 0.0)
                ImGui::Text("Frame %llu: %.2f ms (CPU %.2f ms, GPU %.2f ms)", static_cast<unsigned long long>(frame.Number), frameMs,
                            frame.CpuMs, frame.GpuMs);
            else
                ImGui::Text("Frame %llu: %.2f ms (CPU %.2f ms)", static_cast<unsigned long long>(frame.Number), frameMs, frame.CpuMs);
            if (frame.VramMB >= 0.0f)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("| VRAM %.0f MB, RAM %.0f MB", frame.VramMB, frame.RamMB);
            }
            if (frame.DroppedScopes > 0)
            {
                ImGui::SameLine();
                ImGui::TextColored(WarningColor, "| %u scopes not kept", frame.DroppedScopes);
            }
            if (!frame.Gpu.empty() && !frame.GpuCalibrated)
            {
                ImGui::SameLine();
                ImGui::TextColored(WarningColor, "| GPU not on the CPU clock");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The device has no calibrated timestamps (VK_KHR_calibrated_timestamps): the GPU track is drawn\n"
                                      "from the frame's start, so its durations are right but not where it lies against the CPU.");
            }
        }
        else
        {
            ImGui::TextDisabled("No frame picked");
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Graph: click a frame to show it.\nTimeline: wheel zooms, drag pans, click a scope to mark it in the\n"
                              "timeline and the table, double-click zooms to it.\nOn the GPU tracks a pass sits where the GPU ran it: "
                              "usually after\nthe CPU frame that recorded it. The transfer track shows the upload submissions\n"
                              "(texture / geometry streaming) under the frame that submitted them.");
    }

    void ProfilerPanel::SaveCapture()
    {
        std::filesystem::path path = Utility::SaveFile("Nox Profiler Capture\0noxprof\0");
        if (path.empty())
            return;
        if (path.extension() != ".noxprof")
            path += ".noxprof";
        m_Message = Profiler::Get().SaveTimeline(path.string()) ? std::format("Saved {}", path.filename().string())
                                                                 : std::format("Could not write {}", path.string());
    }

    void ProfilerPanel::LoadCapture()
    {
        const std::string path = Utility::OpenFile("Nox Profiler Capture\0noxprof\0");
        if (path.empty())
            return;
        auto capture = std::make_unique<ProfileCapture>();
        if (!Profiler::LoadCapture(path, *capture))
        {
            m_Message = std::format("Not a Nox profiler capture: {}", path);
            return;
        }
        m_Capture = std::move(capture);
        m_CaptureName = std::filesystem::path(path).filename().string();
        m_Message.clear();
        m_HasSelection = false;
        m_Follow = false;
        m_HighlightScope = ~0u;
        if (const uint32_t count = FrameCount(); count > 0)
            SelectFrame(count - 1);
    }

    void ProfilerPanel::DrawFrameGraph()
    {
        const uint32_t count = FrameCount();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = std::max(ImGui::GetContentRegionAvail().x, 100.0f);
        ImGui::InvisibleButton("##FrameGraph", ImVec2(width, FrameGraphHeight));
        const bool hovered = ImGui::IsItemHovered();

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + FrameGraphHeight), IM_COL32(25, 25, 28, 255));

        double maxMs = 1000.0 / 30.0;
        for (uint32_t index = 0; index < count; ++index)
        {
            const TimelineFrame& frame = Frame(index);
            maxMs = std::max({ maxMs, static_cast<double>(frame.EndNs - frame.StartNs) / NanosecondsPerMillisecond, frame.GpuMs });
        }
        maxMs *= 1.05;
        auto msToY = [&](double ms) { return origin.y + FrameGraphHeight - static_cast<float>(std::min(ms / maxMs, 1.0)) * FrameGraphHeight; };

        for (double reference : { 1000.0 / 60.0, 1000.0 / 30.0 })
        {
            const float y = msToY(reference);
            drawList->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + width, y), IM_COL32(255, 255, 255, 40));
            drawList->AddText(ImVec2(origin.x + 2.0f, y - ImGui::GetTextLineHeight()), IM_COL32(255, 255, 255, 90),
                              reference < 20.0 ? "60 fps" : "30 fps");
        }

        // Fixed slots, newest on the right.
        const float slot = width / static_cast<float>(FrameSlots());
        const int32_t selected = m_HasSelection ? FindFrame(m_SelectedFrame) : -1;
        for (uint32_t index = 0; index < count; ++index)
        {
            const TimelineFrame& frame = Frame(index);
            const double frameMs = static_cast<double>(frame.EndNs - frame.StartNs) / NanosecondsPerMillisecond;
            const float x0 = origin.x + width - static_cast<float>(count - index) * slot;
            const float x1 = x0 + std::max(slot - 1.0f, 1.0f);
            drawList->AddRectFilled(ImVec2(x0, msToY(frameMs)), ImVec2(x1, origin.y + FrameGraphHeight), FrameTimeColor(frameMs));
            if (frame.GpuMs >= 0.0)
            {
                const float y = msToY(frame.GpuMs);
                drawList->AddLine(ImVec2(x0, y), ImVec2(x1 + 1.0f, y), IM_COL32(110, 170, 255, 255), 2.0f);
            }
            if (static_cast<int32_t>(index) == selected)
                drawList->AddRect(ImVec2(x0 - 1.0f, origin.y), ImVec2(x1 + 1.0f, origin.y + FrameGraphHeight), IM_COL32(255, 255, 255, 220));
        }

        if (hovered && count > 0)
        {
            const float fromRight = origin.x + width - ImGui::GetIO().MousePos.x;
            const int32_t index = static_cast<int32_t>(count) - 1 - static_cast<int32_t>(fromRight / slot);
            if (index >= 0 && index < static_cast<int32_t>(count))
            {
                const TimelineFrame& frame = Frame(static_cast<uint32_t>(index));
                ImGui::BeginTooltip();
                ImGui::Text("Frame %llu", static_cast<unsigned long long>(frame.Number));
                ImGui::Text("Frame: %.2f ms", static_cast<double>(frame.EndNs - frame.StartNs) / NanosecondsPerMillisecond);
                ImGui::Text("CPU: %.2f ms", frame.CpuMs);
                if (frame.GpuMs >= 0.0)
                    ImGui::TextColored(ImVec4(0.45f, 0.67f, 1.0f, 1.0f), "GPU: %.2f ms", frame.GpuMs);
                if (frame.VramMB >= 0.0f)
                    ImGui::TextDisabled("VRAM %.0f MB, RAM %.0f MB", frame.VramMB, frame.RamMB);
                ImGui::EndTooltip();
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    m_Follow = false;
                    SelectFrame(static_cast<uint32_t>(index));
                }
            }
        }
    }

    void ProfilerPanel::DrawMemoryGraph()
    {
        // VRAM and RAM per frame, on the frame graph's slots (Unreal Insights' memory track; Tracy plots the same two values).
        const uint32_t count = FrameCount();
        float maxMB = 0.0f;
        for (uint32_t index = 0; index < count; ++index)
            maxMB = std::max({ maxMB, Frame(index).VramMB, Frame(index).RamMB });
        if (maxMB <= 0.0f)
            return;
        maxMB *= 1.1f;

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = std::max(ImGui::GetContentRegionAvail().x, 100.0f);
        ImGui::InvisibleButton("##MemoryGraph", ImVec2(width, MemoryGraphHeight));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + MemoryGraphHeight), IM_COL32(25, 25, 28, 255));

        const float slot = width / static_cast<float>(FrameSlots());
        auto mbToY = [&](float mb) { return origin.y + MemoryGraphHeight - std::min(mb / maxMB, 1.0f) * (MemoryGraphHeight - 2.0f); };
        auto slotX = [&](uint32_t index) { return origin.x + width - (static_cast<float>(count - index) - 0.5f) * slot; };
        const ImU32 colors[2] = { VramColor, RamColor };
        for (uint32_t series = 0; series < 2; ++series)
        {
            bool hasPrevious = false;
            ImVec2 previous;
            for (uint32_t index = 0; index < count; ++index)
            {
                const float mb = series == 0 ? Frame(index).VramMB : Frame(index).RamMB;
                if (mb < 0.0f)
                {
                    hasPrevious = false;
                    continue;
                }
                const ImVec2 point(slotX(index), mbToY(mb));
                if (hasPrevious)
                    drawList->AddLine(previous, point, colors[series], 1.5f);
                previous = point;
                hasPrevious = true;
            }
        }
        const std::string scale = std::format("{:.0f} MB", maxMB);
        drawList->AddText(ImVec2(origin.x + 2.0f, origin.y), IM_COL32(255, 255, 255, 90), scale.c_str());
        drawList->AddText(ImVec2(origin.x + 2.0f, origin.y + MemoryGraphHeight - ImGui::GetTextLineHeight()), VramColor, "VRAM");
        drawList->AddText(ImVec2(origin.x + 40.0f, origin.y + MemoryGraphHeight - ImGui::GetTextLineHeight()), RamColor, "RAM");

        if (hovered && count > 0)
        {
            const float fromRight = origin.x + width - ImGui::GetIO().MousePos.x;
            const int32_t index = static_cast<int32_t>(count) - 1 - static_cast<int32_t>(fromRight / slot);
            if (index >= 0 && index < static_cast<int32_t>(count) && Frame(static_cast<uint32_t>(index)).VramMB >= 0.0f)
            {
                const TimelineFrame& frame = Frame(static_cast<uint32_t>(index));
                ImGui::BeginTooltip();
                ImGui::Text("Frame %llu", static_cast<unsigned long long>(frame.Number));
                ImGui::TextColored(ImColor(VramColor).Value, "VRAM: %.0f MB", frame.VramMB);
                ImGui::TextColored(ImColor(RamColor).Value, "RAM: %.0f MB", frame.RamMB);
                ImGui::EndTooltip();
            }
        }
    }

    void ProfilerPanel::DrawTimeline()
    {
        const uint32_t frameCount = FrameCount();
        const int32_t selected = m_HasSelection ? FindFrame(m_SelectedFrame) : -1;
        if (frameCount == 0 || selected < 0 || m_ViewDurationNs <= 0.0)
        {
            ImGui::TextDisabled(m_Capture ? "The capture has no frames here." : m_Source == FrameSource::Spikes ? "No frame above the spike threshold yet."
                                : m_Paused ? "Paused with no frame kept." : "Waiting for frames...");
            return;
        }
        const TimelineFrame& selectedFrame = Frame(static_cast<uint32_t>(selected));

        // The frames with something in view (GPU work lies after its frame).
        const double viewEnd = m_ViewStartNs + m_ViewDurationNs;
        std::vector<const TimelineFrame*> frames;
        for (uint32_t index = 0; index < frameCount; ++index)
        {
            const TimelineFrame& frame = Frame(index);
            if (static_cast<double>(frame.EndNs + GpuSpillNs) >= m_ViewStartNs && static_cast<double>(frame.StartNs - GpuSpillNs) <= viewEnd)
                frames.push_back(&frame);
        }

        // Lanes: the GPU, then the main thread, then the other threads; as many rows as their deepest scope in view.
        const uint32_t threadCount = ThreadCount();
        std::vector<uint32_t> laneDepth(threadCount, 0);
        // GPU tracks: the graphics queue, the async compute queue (its scopes have no "GPU Frame" around them: one level less) and the
        // transfer queue (one scope per upload submission).
        constexpr uint32_t GpuTracks = 3;
        static constexpr const char* GpuTrackNames[GpuTracks] = { "GPU", "GPU Async Compute", "GPU Transfer" };
        uint32_t gpuDepth[GpuTracks] = {};
        bool hasGpu[GpuTracks] = {};
        auto gpuTrack = [](const TimelineScope& scope) { return std::min(scope.Lane, GpuTracks - 1); };
        auto gpuRow = [&](const TimelineScope& scope) { return gpuTrack(scope) == 1 && scope.Depth > 0 ? scope.Depth - 1 : scope.Depth; };
        for (const TimelineFrame* frame : frames)
        {
            for (const TimelineScope& scope : frame->Cpu)
            {
                if (scope.Lane < threadCount)
                    laneDepth[scope.Lane] = std::max(laneDepth[scope.Lane], scope.Depth);
            }
            for (const TimelineScope& scope : frame->Gpu)
            {
                gpuDepth[gpuTrack(scope)] = std::max(gpuDepth[gpuTrack(scope)], gpuRow(scope));
                hasGpu[gpuTrack(scope)] = true;
            }
        }
        std::vector<uint32_t> laneOrder;
        for (uint32_t lane = 0; lane < threadCount; ++lane)
        {
            if (ThreadName(lane) == "Main Thread")
                laneOrder.insert(laneOrder.begin(), lane);
            else
                laneOrder.push_back(lane);
        }

        const float rowHeight = ImGui::GetTextLineHeight() + 4.0f;
        const float laneGap = 6.0f;
        const ImVec2 canvas = ImGui::GetCursorScreenPos();
        const float canvasWidth = std::max(ImGui::GetContentRegionAvail().x, LaneLabelWidth + 100.0f);
        const float trackX = canvas.x + LaneLabelWidth;
        const float trackWidth = canvasWidth - LaneLabelWidth;

        // Lane tops, from below the ruler.
        std::vector<float> laneTop(threadCount, 0.0f);
        float y = rowHeight + laneGap;
        float gpuTop[GpuTracks] = {};
        for (uint32_t track = 0; track < GpuTracks; ++track)
        {
            gpuTop[track] = y;
            if (hasGpu[track])
                y += static_cast<float>(gpuDepth[track] + 1) * rowHeight + laneGap;
        }
        for (uint32_t lane : laneOrder)
        {
            laneTop[lane] = y;
            y += static_cast<float>(laneDepth[lane] + 1) * rowHeight + laneGap;
        }
        const float totalHeight = std::max(y, ImGui::GetContentRegionAvail().y);

        ImGui::InvisibleButton("##TimelineCanvas", ImVec2(canvasWidth, totalHeight),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const ImGuiIO& io = ImGui::GetIO();

        auto toX = [&](double ns) { return trackX + static_cast<float>((ns - m_ViewStartNs) / m_ViewDurationNs) * trackWidth; };
        auto toNs = [&](float x) { return m_ViewStartNs + static_cast<double>((x - trackX) / trackWidth) * m_ViewDurationNs; };

        // Zoom around the mouse, pan by dragging.
        if (hovered && io.MouseWheel != 0.0f)
        {
            const double anchor = toNs(io.MousePos.x);
            const double duration = std::clamp(m_ViewDurationNs * std::pow(1.25, -static_cast<double>(io.MouseWheel)), MinViewNs, MaxViewNs);
            m_ViewStartNs = anchor - (anchor - m_ViewStartNs) * (duration / m_ViewDurationNs);
            m_ViewDurationNs = duration;
            m_Follow = false;
        }
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f) ||
                       ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2.0f)))
        {
            m_ViewStartNs -= static_cast<double>(io.MouseDelta.x / trackWidth) * m_ViewDurationNs;
            m_Follow = false;
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float bottom = canvas.y + totalHeight;
        const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);

        // Lane backgrounds and names.
        auto drawLane = [&](float top, uint32_t depth, const char* name, ImU32 background)
        {
            const float laneBottom = canvas.y + top + static_cast<float>(depth + 1) * rowHeight;
            drawList->AddRectFilled(ImVec2(canvas.x, canvas.y + top), ImVec2(canvas.x + canvasWidth, laneBottom), background);
            drawList->PushClipRect(ImVec2(canvas.x, canvas.y + top), ImVec2(trackX - 4.0f, laneBottom), true);
            drawList->AddText(ImVec2(canvas.x + 4.0f, canvas.y + top + 2.0f), textColor, name);
            drawList->PopClipRect();
        };
        static constexpr ImU32 GpuTrackBackgrounds[GpuTracks] = { IM_COL32(40, 48, 64, 255), IM_COL32(52, 44, 64, 255), IM_COL32(40, 56, 50, 255) };
        for (uint32_t track = 0; track < GpuTracks; ++track)
        {
            if (hasGpu[track])
                drawLane(gpuTop[track], gpuDepth[track], GpuTrackNames[track], GpuTrackBackgrounds[track]);
        }
        for (uint32_t lane : laneOrder)
            drawLane(laneTop[lane], laneDepth[lane], ThreadName(lane).c_str(), IM_COL32(36, 36, 40, 255));

        drawList->PushClipRect(ImVec2(trackX, canvas.y), ImVec2(trackX + trackWidth, bottom), true);

        // Ruler: time from the picked frame's start.
        const double step = NiceStep(m_ViewDurationNs * 90.0 / std::max(trackWidth, 1.0f));
        const double origin = static_cast<double>(selectedFrame.StartNs);
        for (double tick = std::ceil((m_ViewStartNs - origin) / step) * step; origin + tick <= viewEnd; tick += step)
        {
            const float x = toX(origin + tick);
            drawList->AddLine(ImVec2(x, canvas.y), ImVec2(x, canvas.y + rowHeight * 0.6f), dimColor);
            drawList->AddText(ImVec2(x + 3.0f, canvas.y), dimColor, FormatDuration(tick).c_str());
        }

        // Frame boundaries.
        for (const TimelineFrame* frame : frames)
        {
            const float x = toX(static_cast<double>(frame->StartNs));
            const bool isSelected = frame == &selectedFrame;
            drawList->AddLine(ImVec2(x, canvas.y + rowHeight), ImVec2(x, bottom), isSelected ? IM_COL32(255, 255, 255, 120) : IM_COL32(255, 255, 255, 45));
            const std::string label = std::format("Frame {}", frame->Number);
            drawList->AddText(ImVec2(x + 3.0f, canvas.y + rowHeight * 0.55f), isSelected ? textColor : dimColor, label.c_str());
        }

        // Scopes.
        const TimelineScope* hoveredScope = nullptr;
        const TimelineFrame* hoveredFrame = nullptr;
        bool hoveredGpu = false;
        auto drawScope = [&](const TimelineScope& scope, const TimelineFrame& frame, float laneY, bool gpu)
        {
            if (static_cast<double>(scope.EndNs) < m_ViewStartNs || static_cast<double>(scope.StartNs) > viewEnd)
                return;
            float x0 = toX(static_cast<double>(scope.StartNs));
            float x1 = std::max(toX(static_cast<double>(scope.EndNs)), x0 + 1.0f);
            const float y0 = canvas.y + laneY + static_cast<float>(gpu ? gpuRow(scope) : scope.Depth) * rowHeight;
            const float y1 = y0 + rowHeight - 1.0f;
            drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ScopeColor(scope.ScopeId, !gpu && Scope(scope.ScopeId).IsWait, gpu));
            if (scope.ScopeId == m_HighlightScope)
                drawList->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
            x0 = std::max(x0, trackX);
            x1 = std::min(x1, trackX + trackWidth);
            if (x1 - x0 > 24.0f)
            {
                const std::string label = std::format("{} {}", ScopeName(scope.ScopeId), FormatDuration(static_cast<double>(scope.EndNs - scope.StartNs)));
                drawList->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
                drawList->AddText(ImVec2(x0 + 3.0f, y0 + 2.0f), IM_COL32(15, 15, 15, 255), label.c_str());
                drawList->PopClipRect();
            }
            if (hovered && io.MousePos.x >= x0 && io.MousePos.x < x1 && io.MousePos.y >= y0 && io.MousePos.y < y1)
            {
                hoveredScope = &scope;
                hoveredFrame = &frame;
                hoveredGpu = gpu;
            }
        };
        for (const TimelineFrame* frame : frames)
        {
            for (const TimelineScope& scope : frame->Gpu)
                drawScope(scope, *frame, gpuTop[gpuTrack(scope)], true);
            for (const TimelineScope& scope : frame->Cpu)
            {
                if (scope.Lane < threadCount)
                    drawScope(scope, *frame, laneTop[scope.Lane], false);
            }
        }
        drawList->PopClipRect();

        if (hoveredScope)
        {
            ScopeTooltip(*hoveredScope, *hoveredFrame, hoveredGpu);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 4.0f)
                m_HighlightScope = hoveredScope->ScopeId;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                const double duration = std::max(static_cast<double>(hoveredScope->EndNs - hoveredScope->StartNs), MinViewNs);
                m_ViewStartNs = static_cast<double>(hoveredScope->StartNs) - duration * 0.1;
                m_ViewDurationNs = duration * 1.2;
                m_Follow = false;
            }
        }
    }

    void ProfilerPanel::DrawScopeTable()
    {
        const int32_t selected = m_HasSelection ? FindFrame(m_SelectedFrame) : -1;
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##Filter", "Filter scopes", m_Filter, sizeof(m_Filter));
        if (m_HighlightScope != ~0u)
        {
            ImGui::SameLine();
            ImGui::Text("Marked: %s", ScopeName(m_HighlightScope));
            ImGui::SameLine();
            if (ImGui::SmallButton("Unmark"))
                m_HighlightScope = ~0u;
        }
        if (selected < 0)
            return;
        const TimelineFrame& frame = Frame(static_cast<uint32_t>(selected));

        // The frame's scopes summed per scope and track (inclusive times).
        struct Row
        {
            uint32_t ScopeId;
            uint32_t Track; // 0 CPU, 1 GPU (graphics queue), 2 GPU (async compute queue), 3 GPU (transfer queue)
            uint32_t Calls = 0;
            double TotalMs = 0.0;
            double MaxMs = 0.0;
        };
        std::vector<Row> rows;
        std::unordered_map<uint64_t, size_t> rowIndex;
        auto add = [&](const TimelineScope& scope, uint32_t track)
        {
            const uint64_t key = (static_cast<uint64_t>(scope.ScopeId) << 2) | track;
            auto [entry, added] = rowIndex.try_emplace(key, rows.size());
            if (added)
                rows.push_back({ scope.ScopeId, track });
            Row& row = rows[entry->second];
            const double ms = static_cast<double>(scope.EndNs - scope.StartNs) / NanosecondsPerMillisecond;
            row.Calls++;
            row.TotalMs += ms;
            row.MaxMs = std::max(row.MaxMs, ms);
        };
        for (const TimelineScope& scope : frame.Cpu)
            add(scope, 0);
        for (const TimelineScope& scope : frame.Gpu)
            add(scope, std::min(scope.Lane, 2u) + 1u);
        std::erase_if(rows, [this](const Row& row) { return !ContainsCaseInsensitive(ScopeName(row.ScopeId), m_Filter); });

        const double frameMs = static_cast<double>(frame.EndNs - frame.StartNs) / NanosecondsPerMillisecond;
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##Scopes", 6, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Total (ms)", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 1.2f);
        ImGui::TableSetupColumn("Max (ms)", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 1.2f);
        ImGui::TableSetupColumn("% of frame", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 1.2f);
        ImGui::TableHeadersRow();

        if (const ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount > 0)
        {
            const ImGuiTableColumnSortSpecs& spec = sort->Specs[0];
            const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
            std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b)
            {
                auto less = [&](const Row& left, const Row& right)
                {
                    switch (spec.ColumnIndex)
                    {
                    case 0: return std::string_view(ScopeName(left.ScopeId)) < std::string_view(ScopeName(right.ScopeId));
                    case 1: return left.Track < right.Track;
                    case 2: return left.Calls < right.Calls;
                    case 4: return left.MaxMs < right.MaxMs;
                    default: return left.TotalMs < right.TotalMs; // total and % of frame
                    }
                };
                return ascending ? less(a, b) : less(b, a);
            });
        }

        for (const Row& row : rows)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(row.ScopeId * 4 + row.Track));
            if (ImGui::Selectable(ScopeName(row.ScopeId), row.ScopeId == m_HighlightScope, ImGuiSelectableFlags_SpanAllColumns))
                m_HighlightScope = row.ScopeId == m_HighlightScope ? ~0u : row.ScopeId;
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.Track == 3 ? "GPU (transfer)" : row.Track == 2 ? "GPU (async compute)" : row.Track == 1 ? "GPU"
                                   : (Scope(row.ScopeId).IsWait ? "CPU (wait)" : "CPU"));
            ImGui::TableNextColumn();
            ImGui::Text("%u", row.Calls);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", row.TotalMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", row.MaxMs);
            ImGui::TableNextColumn();
            const double reference = row.Track != 0 ? frame.GpuMs : frameMs;
            if (reference > 0.0)
                ImGui::Text("%.1f %%", row.TotalMs / reference * 100.0);
            else
                ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
    }
}
