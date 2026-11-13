#include "OutputLogPanel.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <sstream>

#include <imgui.h>
#include <imgui_internal.h>

namespace Nox
{
    namespace
    {
        constexpr const char* WindowName = "Output Log";

        bool IsError(spdlog::level::level_enum level) { return level == spdlog::level::err || level == spdlog::level::critical; }
        bool IsWarning(spdlog::level::level_enum level) { return level == spdlog::level::warn; }
        bool IsVerbose(spdlog::level::level_enum level) { return level == spdlog::level::trace || level == spdlog::level::debug; }

        ImVec4 LevelColor(spdlog::level::level_enum level)
        {
            if (IsError(level))
                return ImVec4(1.0f, 0.42f, 0.42f, 1.0f);
            if (IsWarning(level))
                return ImVec4(1.0f, 0.78f, 0.3f, 1.0f);
            if (IsVerbose(level))
                return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        }

        std::string Lower(std::string_view text)
        {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower;
        }

        // The line as it is copied: "[hh:mm:ss] NOX: text", like Nox.log.
        std::string Format(const LogEntry& entry)
        {
            return std::format("[{}] {}: {}", entry.Clock, entry.Logger, entry.Text);
        }
    }

    void OutputLogPanel::Fetch()
    {
        std::vector<LogEntry> fresh;
        const uint64_t latest = LogBuffer::Get().CopySince(m_LastSequence, fresh);
        if (fresh.empty())
            return;
        m_LastSequence = latest;
        const size_t before = m_Entries.size();
        for (LogEntry& entry : fresh)
            m_Entries.push_back(std::move(entry));
        if (m_Entries.size() > LogBuffer::Capacity)
        {
            // The oldest dropped: indices move, the rows are made again.
            m_Entries.erase(m_Entries.begin(), m_Entries.begin() + static_cast<std::ptrdiff_t>(m_Entries.size() - LogBuffer::Capacity));
            Refilter();
            return;
        }
        for (size_t index = before; index < m_Entries.size(); ++index)
        {
            const LogEntry& entry = m_Entries[index];
            m_ErrorCount += IsError(entry.Level);
            m_WarningCount += IsWarning(entry.Level);
            if (Passes(entry))
                AddRows(static_cast<uint32_t>(index));
        }
    }

    bool OutputLogPanel::Passes(const LogEntry& entry) const
    {
        if (entry.Sequence <= m_ClearedAt)
            return false;
        if (IsError(entry.Level) ? !m_ShowErrors : IsWarning(entry.Level) ? !m_ShowWarnings : IsVerbose(entry.Level) ? !m_ShowVerbose : !m_ShowInfo)
            return false;
        if (m_SearchWords.empty())
            return true;
        const std::string text = Lower(entry.Logger + ": " + entry.Text);
        for (const std::string& word : m_SearchWords)
            if (text.find(word) == std::string::npos)
                return false;
        return true;
    }

    void OutputLogPanel::AddRows(uint32_t entry)
    {
        const std::string& text = m_Entries[entry].Text;
        uint32_t begin = 0;
        while (true)
        {
            const size_t end = text.find('\n', begin);
            const uint32_t stop = end == std::string::npos ? static_cast<uint32_t>(text.size()) : static_cast<uint32_t>(end);
            // (a trailing line break makes no empty row)
            if (stop > begin || begin == 0)
                m_Shown.push_back({ entry, begin, stop > begin && text[stop - 1] == '\r' ? stop - 1 : stop });
            if (end == std::string::npos)
                break;
            begin = static_cast<uint32_t>(end + 1);
            if (begin >= text.size())
                break;
        }
    }

    void OutputLogPanel::Refilter()
    {
        m_SearchWords.clear();
        std::istringstream words(Lower(m_Search));
        for (std::string word; words >> word;)
            m_SearchWords.push_back(word);
        m_Shown.clear();
        m_ErrorCount = m_WarningCount = 0;
        for (size_t index = 0; index < m_Entries.size(); ++index)
        {
            const LogEntry& entry = m_Entries[index];
            if (entry.Sequence > m_ClearedAt)
            {
                m_ErrorCount += IsError(entry.Level);
                m_WarningCount += IsWarning(entry.Level);
            }
            if (Passes(entry))
                AddRows(static_cast<uint32_t>(index));
        }
    }

    void OutputLogPanel::OnImGuiRender()
    {
        if (!m_Open)
            return;
        Fetch();

        // Its first appearance: a tab beside the Content Browser (Unreal's place for it). A saved layout keeps wherever the user put it.
        if (!m_DockPlaced)
        {
            if (ImGui::FindWindowByName(WindowName) || ImGui::FindWindowSettingsByID(ImHashStr(WindowName)))
                m_DockPlaced = true;
            else if (ImGuiWindow* browser = ImGui::FindWindowByName("Content Browser"); browser && browser->DockId != 0)
            {
                ImGui::SetNextWindowDockID(browser->DockId, ImGuiCond_Always);
                m_DockPlaced = true;
            }
            else if (++m_DockAttempts > 30)
                m_DockPlaced = true; // (the Content Browser floats: the log opens on its own)
            if (!m_DockPlaced)
                return; // the Content Browser gets its dock in the first frames
        }

        // (Not brought to the front when it appears: the Content Browser stays the tab shown.)
        if (!ImGui::Begin(WindowName, &m_Open, ImGuiWindowFlags_NoFocusOnAppearing))
        {
            ImGui::End();
            return;
        }
        DrawToolbar();
        ImGui::Separator();
        DrawLines();
        ImGui::End();
    }

    void OutputLogPanel::DrawToolbar()
    {
        if (ImGui::Button("Clear"))
        {
            m_ClearedAt = m_LastSequence;
            m_Selected.clear();
            Refilter();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hide the lines shown so far (Nox.log keeps everything)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.35f));
        if (ImGui::InputTextWithHint("##Search", "Search (every word, e.g. net debug)", m_Search, sizeof(m_Search)))
            Refilter();
        ImGui::SameLine();
        bool changed = false;
        ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(spdlog::level::err));
        changed |= ImGui::Checkbox(std::format("Errors ({})###Errors", m_ErrorCount).c_str(), &m_ShowErrors);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(spdlog::level::warn));
        changed |= ImGui::Checkbox(std::format("Warnings ({})###Warnings", m_WarningCount).c_str(), &m_ShowWarnings);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Info", &m_ShowInfo);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Verbose", &m_ShowVerbose);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Trace and debug lines");
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_AutoScroll);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Follow new lines while the view is at the bottom");
        if (changed)
            Refilter();
    }

    void OutputLogPanel::DrawLines()
    {
        ImGui::BeginChild("##Lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        const float clockWidth = ImGui::CalcTextSize("[00:00:00] ").x;

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_Shown.size()), lineHeight);
        while (clipper.Step())
        {
            for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
            {
                const Row& row = m_Shown[static_cast<size_t>(rowIndex)];
                const LogEntry& entry = m_Entries[row.Entry];
                const bool selected = std::binary_search(m_Selected.begin(), m_Selected.end(), entry.Sequence);
                ImGui::PushID(rowIndex);
                const ImVec2 start = ImGui::GetCursorScreenPos();
                // The row's hit box (its text is drawn over it: a log line may hold "##", which a label would hide).
                if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_SpanAllColumns,
                                      ImVec2(0.0f, ImGui::GetTextLineHeight())))
                {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyShift && m_SelectionAnchor != 0)
                    {
                        // Every entry shown between the anchor and this one.
                        m_Selected.clear();
                        bool inside = false;
                        for (const Row& other : m_Shown)
                        {
                            const uint64_t sequence = m_Entries[other.Entry].Sequence;
                            const bool edge = sequence == m_SelectionAnchor || sequence == entry.Sequence;
                            if (edge || inside)
                                if (m_Selected.empty() || m_Selected.back() != sequence)
                                    m_Selected.push_back(sequence);
                            if (edge && m_SelectionAnchor != entry.Sequence)
                            {
                                inside = !inside;
                                if (!inside)
                                    break;
                            }
                        }
                    }
                    else if (io.KeyCtrl)
                    {
                        auto found = std::lower_bound(m_Selected.begin(), m_Selected.end(), entry.Sequence);
                        if (found != m_Selected.end() && *found == entry.Sequence)
                            m_Selected.erase(found);
                        else
                            m_Selected.insert(found, entry.Sequence);
                        m_SelectionAnchor = entry.Sequence;
                    }
                    else
                    {
                        m_Selected.assign(1, entry.Sequence);
                        m_SelectionAnchor = entry.Sequence;
                    }
                }
                ImGui::SetCursorScreenPos(start);
                if (row.Begin == 0)
                    ImGui::TextDisabled("[%s]", entry.Clock.c_str());
                ImGui::SameLine(clockWidth);
                ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(entry.Level));
                ImGui::TextUnformatted(entry.Text.data() + row.Begin, entry.Text.data() + row.End);
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
        clipper.End();

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
            CopyLines(true);
        if (ImGui::BeginPopupContextWindow("##LogMenu"))
        {
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, !m_Selected.empty()))
                CopyLines(true);
            if (ImGui::MenuItem("Copy All Shown"))
                CopyLines(false);
            ImGui::Separator();
            if (ImGui::MenuItem("Clear"))
            {
                m_ClearedAt = m_LastSequence;
                m_Selected.clear();
                Refilter();
            }
            ImGui::EndPopup();
        }

        if (m_AutoScroll && atBottom)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    void OutputLogPanel::CopyLines(bool selectedOnly) const
    {
        std::string text;
        uint32_t lastEntry = UINT32_MAX;
        for (const Row& row : m_Shown)
        {
            if (row.Entry == lastEntry)
                continue; // (one entry once, all its lines)
            lastEntry = row.Entry;
            const LogEntry& entry = m_Entries[row.Entry];
            if (selectedOnly && !std::binary_search(m_Selected.begin(), m_Selected.end(), entry.Sequence))
                continue;
            text += Format(entry);
            text += '\n';
        }
        if (!text.empty())
            ImGui::SetClipboardText(text.c_str());
    }
}
