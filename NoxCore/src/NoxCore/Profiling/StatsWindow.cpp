#include "StatsWindow.h"

#include <imgui.h>

#include <SDL3/SDL_cpuinfo.h>

#include "NoxCore/Audio/AudioEngine.h"
#include "NoxCore/Renderer/Renderer.h"
#include "StatsReport.h"

#if !defined(NOX_BUILD_CONFIG)
    #define NOX_BUILD_CONFIG "Unknown"
#endif

namespace Nox
{
    namespace
    {
        double ToMegabytes(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

        const ImVec4 kWarning(1.0f, 0.45f, 0.4f, 1.0f);

        // A table of timings (last / avg / min / max, ms), nested rows indented by their depth. `id` names the table.
        void TimingTable(const char* id, const std::vector<ProfileScopeStats>& scopes)
        {
            constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                                              ImGuiTableFlags_Resizable;
            if (!ImGui::BeginTable(id, 5, flags))
                return;
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            for (const char* column : { "last", "avg", "min", "max" })
                ImGui::TableSetupColumn(column, ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.6f);
            ImGui::TableHeadersRow();
            const float indent = ImGui::GetFontSize() * 0.9f;
            for (const ProfileScopeStats& scope : scopes)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent * static_cast<float>(scope.Depth));
                ImGui::TextUnformatted(scope.Name ? scope.Name : "?");
                for (double value : { scope.Timing.Last, scope.Timing.Avg, scope.Timing.Min, scope.Timing.Max })
                {
                    ImGui::TableNextColumn();
                    ImGui::Text("%7.2f", value);
                }
            }
            ImGui::EndTable();
        }
    }

    void StatsWindow::Draw(Renderer& renderer, bool* open)
    {
        ImGui::SetNextWindowSize(ImVec2(520.0f, 620.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Stats", open))
        {
            ImGui::End();
            return;
        }
        Profiler& profiler = Profiler::Get();

        // Always: build, frame rate, frame times, memory.
        ImGui::Text("%s | VSync %s", NOX_BUILD_CONFIG, renderer.getVSync() ? "ON" : "OFF");
        if (std::string_view(NOX_BUILD_CONFIG) == "Debug")
        {
            ImGui::SameLine();
            ImGui::TextColored(kWarning, "Debug build: not for performance numbers");
        }
        const ProfileTiming frame = profiler.GetFrameTime();
        ImGui::Text("FPS %.0f  (%.2f ms)", frame.Avg > 0.0 ? 1000.0 / frame.Avg : 0.0, frame.Avg);
        {
            std::vector<ProfileScopeStats> rows;
            rows.push_back({ "CPU frame", 0, profiler.GetCpuFrameTime() });
            if (profiler.IsGpuTimingAvailable())
                rows.push_back({ "GPU frame", 0, profiler.GetGpuFrameTime() });
            TimingTable("##frame", rows);
        }
        const ProfileMemoryStats& memory = profiler.GetMemoryStats();
        uint64_t vramUsage = 0, vramBudget = 0;
        for (const NRI::MemoryHeapStats& heap : memory.Heaps)
        {
            if (!heap.deviceLocal)
                continue;
            vramUsage += heap.usage;
            vramBudget += heap.budget;
        }
        ImGui::Text("RAM  %.0f / %d MB    VRAM  %.0f / %.0f MB", ToMegabytes(memory.Process.WorkingSetBytes), SDL_GetSystemRAM(),
                    ToMegabytes(vramUsage), ToMegabytes(vramBudget));
        if (ImGui::SmallButton("Reset Stats"))
            profiler.ResetStats();
        ImGui::SetItemTooltip("Starts min / max / avg over (Shift+F3).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Save Report"))
            m_LastReport = SaveStatsReport(renderer).string();
        ImGui::SetItemTooltip("Writes everything here to a NoxStats_<date>.txt next to Nox.log (Ctrl+F3).");
        if (!m_LastReport.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("saved %s", m_LastReport.c_str());
        }
        ImGui::Separator();

        // The rest scrolls (the window itself), each section folded or open as left.
        if (AudioEngine::IsAvailable() && ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const AudioStats audio = AudioEngine::GetStats();
            const bool full = audio.MaxRealVoices > 0 && audio.RealVoices >= audio.MaxRealVoices;
            ImGui::TextColored(full ? kWarning : ImGui::GetStyleColorVec4(ImGuiCol_Text), "Voices %u: %u real / %u virtual (budget %u), %u streaming",
                               audio.Voices, audio.RealVoices, audio.VirtualVoices, audio.MaxRealVoices, audio.StreamingVoices);
            const bool late = audio.MixBlockMaxMilliseconds > audio.BlockMilliseconds || audio.Underruns > 0;
            ImGui::TextColored(late ? kWarning : ImGui::GetStyleColorVec4(ImGuiCol_Text), "Mixer block %.2f ms avg, %.2f max of %.2f ms | %llu underruns, %llu starvations",
                               audio.MixBlockMilliseconds, audio.MixBlockMaxMilliseconds, audio.BlockMilliseconds,
                               static_cast<unsigned long long>(audio.Underruns), static_cast<unsigned long long>(audio.StreamStarvations));
            const AudioEngine::ReflectionStats reflections = AudioEngine::GetReflectionStats();
            ImGui::Text("Acoustics: %d reflection voices, %d pathing, listener reverb %s, %.1f ms per run", reflections.Voices,
                        reflections.PathingVoices, reflections.ListenerReverb ? "on" : "off", reflections.Milliseconds);
            ImGui::TextDisabled("%s, %u Hz", audio.DeviceName.c_str(), audio.SampleRate);
        }
        if (ImGui::CollapsingHeader("GPU passes (ms)"))
        {
            if (profiler.IsGpuTimingAvailable())
            {
                profiler.GetGpuScopes(m_GpuScopes);
                TimingTable("##gpu", m_GpuScopes);
            }
            else
                ImGui::TextDisabled("GPU timestamp queries are not supported on this queue.");
        }
        if (ImGui::CollapsingHeader("CPU scopes (ms)"))
        {
            profiler.GetCpuScopes(m_CpuScopes);
            TimingTable("##cpu", m_CpuScopes);
        }
        if (ImGui::CollapsingHeader("Memory"))
        {
            for (size_t index = 0; index < memory.Heaps.size(); ++index)
            {
                const NRI::MemoryHeapStats& heap = memory.Heaps[index];
                ImGui::Text("%s heap %zu: %.0f / %.0f MB, %llu allocations (%.0f in %.0f MB of blocks)", heap.deviceLocal ? "VRAM" : "Host", index,
                            ToMegabytes(heap.usage), ToMegabytes(heap.budget), static_cast<unsigned long long>(heap.allocationCount),
                            ToMegabytes(heap.allocationBytes), ToMegabytes(heap.blockBytes));
            }
            if (!memory.Heaps.empty() && !memory.BudgetFromDriver)
                ImGui::TextDisabled("VK_EXT_memory_budget unavailable: estimated");
            if (!memory.Categories.empty() &&
                ImGui::BeginTable("##categories", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("VRAM by category", ImGuiTableColumnFlags_WidthStretch);
                for (const char* column : { "committed MB", "used MB", "budget MB" })
                    ImGui::TableSetupColumn(column);
                ImGui::TableHeadersRow();
                for (const MemoryCategoryStats& category : memory.Categories)
                {
                    const bool over = category.Budget > 0 && category.Committed > category.Budget;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (over)
                        ImGui::TextColored(kWarning, "%s  OVER", category.Name);
                    else
                        ImGui::TextUnformatted(category.Name);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", ToMegabytes(category.Committed));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", ToMegabytes(category.Used));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", ToMegabytes(category.Budget));
                }
                ImGui::EndTable();
            }
        }
        profiler.GetCounters(m_Counters);
        if (!m_Counters.empty() && ImGui::CollapsingHeader("Counters"))
        {
            if (ImGui::BeginTable("##counters", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("Counter", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value");
                for (const ProfileCounterStats& counter : m_Counters)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(counter.Name ? counter.Name : "?");
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", counter.Value);
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
}
