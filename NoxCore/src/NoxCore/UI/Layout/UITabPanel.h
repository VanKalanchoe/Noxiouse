#pragma once

#include <string>
#include <vector>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UIButton;

    // Dockable tabbed panel container (tab strip + active content host).
    class UITabPanel : public UIStackBox
    {
    public:
        UITabPanel();

        size_t AddTab(const std::string& title, const Ref<UIElement>& content);
        void SetActiveTab(size_t index);
        size_t GetActiveTab() const { return m_ActiveIndex; }
        size_t GetTabCount() const { return m_Tabs.size(); }

    private:
        struct TabEntry
        {
            std::string Title;
            Ref<UIButton> Button;
            Ref<UIElement> Content;
        };

        void RefreshTabStyles();

        Ref<UIStackBox> m_TabBar;
        Ref<UIStackBox> m_ContentHost;
        std::vector<TabEntry> m_Tabs;
        size_t m_ActiveIndex = 0;
    };
}
