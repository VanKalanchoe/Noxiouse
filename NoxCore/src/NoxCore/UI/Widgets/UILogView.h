#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    enum class UILogLevel : uint8_t
    {
        Info = 0,
        Warning,
        Error
    };

    struct UILogEntry
    {
        UILogLevel Level = UILogLevel::Info;
        std::string Message;
    };

    // Filterable, virtualized log/console view with severity color coding and auto-scroll.
    class UILogView : public UIStackBox
    {
    public:
        UILogView();

        void AddLog(UILogLevel level, std::string message);
        void Clear();

        size_t MaxEntries = 2000;
        bool AutoScroll = true;

    private:
        void RebuildFiltered();

        std::vector<UILogEntry> m_Entries;
        std::string m_Filter;
        Ref<UIElement> m_ListSurface;
    };
}
