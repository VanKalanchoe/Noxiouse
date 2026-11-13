#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "NoxCore/Core/LogBuffer.h"

namespace Nox
{
    // Unreal's Output Log: everything the engine, the editor and the scripts log (the LogBuffer), as it happens -- a tab beside the
    // Content Browser the first time it opens. Filters by verbosity (errors, warnings, info, verbose) and by text (every word must
    // appear, case ignored: "net debug" or "[C#]"); Clear hides what is there (Nox.log keeps it all); the view follows new lines while
    // it is scrolled to the bottom. Click a line to select it (Shift / Ctrl for more), Ctrl+C or the right-click menu copies; Window >
    // Output Log.
    class OutputLogPanel
    {
    public:
        void OnImGuiRender();

        bool IsOpen() const { return m_Open; }
        void SetOpen(bool open) { m_Open = open; }

    private:
        void Fetch();
        bool Passes(const LogEntry& entry) const;
        void Refilter();
        void AddRows(uint32_t entry);
        void DrawToolbar();
        void DrawLines();
        void CopyLines(bool selectedOnly) const;

    private:
        bool m_Open = true;
        bool m_DockPlaced = false;           // its first appearance put beside the Content Browser (or given up)
        int m_DockAttempts = 0;
        std::vector<LogEntry> m_Entries;     // its own copy of the buffer's lines (at most LogBuffer::Capacity)
        uint64_t m_LastSequence = 0;
        uint64_t m_ClearedAt = 0;            // lines up to this one are hidden (Clear)
        // The rows drawn: each line of each entry that passes the filters (a message with line breaks takes several rows).
        struct Row
        {
            uint32_t Entry = 0;          // index into m_Entries
            uint32_t Begin = 0, End = 0; // the line's characters in its Text
        };
        std::vector<Row> m_Shown;
        bool m_ShowErrors = true, m_ShowWarnings = true, m_ShowInfo = true, m_ShowVerbose = true;
        char m_Search[256] = {};
        std::vector<std::string> m_SearchWords; // lower case
        bool m_AutoScroll = true;
        uint64_t m_SelectionAnchor = 0;      // sequences: what Shift-click extends from
        std::vector<uint64_t> m_Selected;    // sorted
        size_t m_ErrorCount = 0, m_WarningCount = 0; // since Clear
    };
}
