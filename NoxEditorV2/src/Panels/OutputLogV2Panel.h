#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "NoxCore/Core/LogBuffer.h"
#include "NoxCore/Core/Ref.h"

namespace Nox
{
    namespace UI
    {
        class UIElement;
        class UILabel;
    }

    class LogLinesView;
    class LogFilterChip;
    class ConsoleLine;

    // Unreal's Output Log: everything the engine, the editor and the scripts log (the LogBuffer) as it happens, docked as a tab
    // beside the Content Browser.
    //  - Filter chips for Errors / Warnings / Messages / Verbose show how many there are since the last Clear; a click hides or
    //    shows that level. The search keeps lines that have every word typed (case ignored: "net debug", "[C#]").
    //  - The view follows new lines while it is scrolled to the bottom. Click a line to select it (Shift / Ctrl for more); Ctrl+C
    //    or the right-click menu copies. Clear hides what is there (Nox.log keeps it all).
    //  - Under the lines, the Cmd line runs engine console commands: Tab completes, Up / Down walk the history, the names that fit
    //    what is typed are listed above it.
    class OutputLogV2Panel
    {
    public:
        OutputLogV2Panel();
        ~OutputLogV2Panel();

        Ref<UI::UIElement> BuildUI();
        // Once a frame: takes the new lines from the LogBuffer.
        void Update(float dt);

        // Since the last Clear (the status bar shows them).
        size_t GetErrorCount() const { return m_ErrorCount; }
        size_t GetWarningCount() const { return m_WarningCount; }
        // Puts the keyboard into the Cmd line (the status bar's counters, the ` key).
        void FocusCommandLine();
        // An element put at the right end of the toolbar (the editor's "Dock in Layout" while the log is in the drawer).
        void SetToolbarExtra(Ref<UI::UIElement> element) { m_ToolbarExtra = std::move(element); }

    private:
        friend class LogLinesView;
        friend class ConsoleLine;

        // What a row shows: one line of one entry (a message with line breaks takes several rows).
        struct Row
        {
            uint32_t Entry = 0;          // index into m_Entries
            uint32_t Begin = 0, End = 0; // the line's characters in the entry's Text
        };

        bool Passes(const LogEntry& entry) const;
        void AddRows(uint32_t entryIndex);
        void Refilter();
        void Clear();
        void CopyLines(bool selectedOnly) const;
        void UpdateSuggestions();
        void RunCommand(const std::string& line);

        std::vector<LogEntry> m_Entries;   // its own copy of the buffer's lines (at most LogBuffer::Capacity)
        uint64_t m_LastSequence = 0;
        uint64_t m_ClearedAt = 0;          // lines up to this sequence are hidden (Clear)
        std::vector<Row> m_Rows;
        bool m_Show[4] = { true, true, true, true }; // errors, warnings, messages, verbose
        std::vector<std::string> m_SearchWords;      // lower case
        size_t m_ErrorCount = 0, m_WarningCount = 0, m_InfoCount = 0, m_VerboseCount = 0;
        std::vector<uint64_t> m_Selected;  // sequences, sorted
        uint64_t m_SelectionAnchor = 0;
        std::vector<std::string> m_History; // commands run, oldest first
        int m_HistoryIndex = -1;

        Ref<UI::UIElement> m_Root;
        Ref<UI::UIElement> m_ToolbarExtra;
        Ref<LogLinesView> m_Lines;
        Ref<LogFilterChip> m_Chips[4];
        Ref<ConsoleLine> m_Command;
        Ref<UI::UILabel> m_Suggestions;
        std::string m_SuggestionsFor;
    };
}
